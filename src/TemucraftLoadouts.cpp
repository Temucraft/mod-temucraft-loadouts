/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * mod-temucraft-loadouts: talent loadouts. 3.3.5 has two talent groups (dual spec); this keeps up to 10 saved
 * builds per character beside them.
 *
 *   - A loadout holds the talents of the active talent group (talent id and rank), its six glyphs and, optionally,
 *     the action bars. Saving copies the active group into a slot; loading swaps the slot into the active group:
 *     the group's talents are reset without the gold cost, the stored talents are learned again through the core's
 *     own LearnTalent (so tier, prerequisite and point checks and every module's talent hooks still apply), the
 *     glyphs are set and the action buttons put back. Loading takes a short "cast" (Temucraft.Loadouts.CastTime:
 *     stand still, out of combat) and has a cooldown.
 *   - Slot 1 is free. Slot 2 comes with Dual Talent Specialization (bought from the class trainer the stock way;
 *     Temucraft.Loadouts.DualSpecSlot = 0 makes it free). Slots 3 and up are bought one at a time, each for
 *     Temucraft Tokens or gold, at the prices in the config (rising per slot). Unlocks are per character, or per
 *     account with Temucraft.Loadouts.AccountWide = 1.
 *   - Characters DB: mod_temucraft_loadouts (the builds), mod_temucraft_loadouts_unlock (what was bought). A
 *     character's rows go when it is deleted for good; its unlock rows are kept with guid 0 for the account-wide
 *     mode (and the record of what was paid).
 *
 * Chat command (players, RBAC 104351): .loadout list | save <n> | load <n> | unlock <n> [token|gold] |
 * rename <n> <name>.
 *
 * Addon messages (prefix TLO, see TemucraftAddonMessage.h). Client -> server:
 *   HELLO (or LIST), SAVE~n, LOAD~n, UNLOCK~n~token|gold, RENAME~n~name, CANCEL
 * Server -> client:
 *   S~enabled~slots~castMs~cooldownLeftSeconds~actions~glyphs~tokenItem
 *   L~n~state~tokens~gold~a/b/c~name     (each slot; state 0 locked, 1 locked and next to buy, 2 needs dual spec,
 *                                          3 unlocked and empty, 4 saved; tokens / gold the unlock price (gold in
 *                                          whole gold, 0 = not offered); a/b/c the points per talent tree)
 *   E                                      (end of the list)
 *   C~n~ms  loading started, X~n  loading cancelled, D~n  loaded (the list follows)
 * The answers to the player (errors, costs, results) go to the chat as system messages, for both the command and
 * the addon.
 *
 * Reads at login are asynchronous queries answered through the session's query processor, writes asynchronous
 * Executes.
 */
#include "Battleground.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TemucraftAddonMessage.h"
#include "TemucraftModuleText.h"
#include "Tokenize.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-loadouts";
    constexpr char const* PREFIX = "TLO";
    constexpr char const* STATE_KEY = "mod-temucraft-loadouts";
    constexpr uint8 MAX_SLOTS = 10;
    constexpr uint32 GOLD = 10000;
    constexpr uint8 GLYPH_SLOTS = MAX_GLYPH_SLOT_INDEX;
    constexpr size_t NAME_BYTES = 24;
    constexpr uint32 TICK_MS = 250;

    struct Settings
    {
        bool enable = true;
        uint8 slots = 10;
        bool dualSpecSlot = true;
        bool accountWide = false;
        bool saveActions = true;
        bool saveGlyphs = true;
        bool allowInBattleground = false;
        uint32 tokenItem = 608000;
        std::vector<uint32> tokenCost;   // slot 3, 4, ...
        std::vector<uint32> goldCost;    // in gold
        uint32 castTime = 3000;          // ms, 0 = at once
        uint32 cooldown = 10;            // s after a load
        uint8 minLevel = 10;
    } cfg;

    // texts (module_string ids)
    enum Text : uint32
    {
        T_OFF = 1, T_NOT_LOADED, T_NO_SLOT, T_LOCKED, T_NEEDS_DUAL, T_EMPTY, T_SAVED, T_RENAMED, T_BAD_NAME,
        T_LIST_HEAD, T_LIST_SAVED, T_LIST_EMPTY, T_LIST_LOCKED_NEXT, T_LIST_LOCKED, T_LIST_DUAL, T_COST_TOKENS,
        T_COST_GOLD, T_COST_OR, T_COST_FREE, T_UNLOCK_HOW, T_UNLOCK_ORDER, T_ALREADY, T_NO_TOKENS, T_NO_GOLD,
        T_UNLOCKED, T_NOT_OFFERED, T_COMBAT, T_BG, T_ENCOUNTER, T_DEAD, T_FLIGHT, T_LEVEL, T_COOLDOWN, T_BUSY,
        T_LOADING, T_CANCELLED, T_LOADED, T_MISSING_POINTS, T_LIST_HINT, T_CASTING
    };

    char const* Say(Player const* p, Text id, char const* fallback) { return TemucraftModuleText(MODULE, p, id, fallback); }

    std::vector<uint32> ParseList(std::string const& text)
    {
        std::vector<uint32> out;
        for (std::string_view part : Acore::Tokenize(text, ',', false))
        {
            std::string s(part);
            s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
            out.push_back(Acore::StringTo<uint32>(s).value_or(0));
        }
        return out;
    }

    void LoadSettings()
    {
        cfg.enable = sConfigMgr->GetOption<bool>("Temucraft.Loadouts.Enable", true);
        cfg.slots = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Loadouts.Slots", 10), 1, MAX_SLOTS));
        cfg.dualSpecSlot = sConfigMgr->GetOption<bool>("Temucraft.Loadouts.DualSpecSlot", true);
        cfg.accountWide = sConfigMgr->GetOption<bool>("Temucraft.Loadouts.AccountWide", false);
        cfg.saveActions = sConfigMgr->GetOption<bool>("Temucraft.Loadouts.SaveActions", true);
        cfg.saveGlyphs = sConfigMgr->GetOption<bool>("Temucraft.Loadouts.SaveGlyphs", true);
        cfg.allowInBattleground = sConfigMgr->GetOption<bool>("Temucraft.Loadouts.AllowInBattleground", false);
        cfg.tokenItem = sConfigMgr->GetOption<uint32>("Temucraft.Loadouts.TokenItem", 608000);
        cfg.tokenCost = ParseList(sConfigMgr->GetOption<std::string>("Temucraft.Loadouts.TokenCost", "2,3,4,5,6,8,10,12"));
        cfg.goldCost = ParseList(sConfigMgr->GetOption<std::string>("Temucraft.Loadouts.GoldCost", "500,1000,1500,2000,3000,4000,5000,7500"));
        cfg.castTime = sConfigMgr->GetOption<uint32>("Temucraft.Loadouts.CastTime", 3000);
        cfg.cooldown = sConfigMgr->GetOption<uint32>("Temucraft.Loadouts.Cooldown", 10);
        cfg.minLevel = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Loadouts.MinLevel", 10), 255));
    }

    // the price of slot n (3 and up) from a list: the list's last value for slots past its end, 0 = not offered
    uint32 Price(std::vector<uint32> const& list, uint8 slot)
    {
        if (slot < 3 || list.empty())
            return 0;
        size_t i = std::min<size_t>(slot - 3, list.size() - 1);
        return list[i];
    }

    struct Loadout
    {
        bool exists = false;            // has a row
        bool saved = false;             // holds a build
        std::string name;
        std::map<uint32, uint8> talents;   // talent id -> rank (1-based)
        std::array<uint32, GLYPH_SLOTS> glyphs{};
        std::map<uint8, uint32> actions;   // button -> packed action and type
        bool hasActions = false;
    };

    struct State : public DataMap::Base
    {
        bool loaded = false;
        bool helloWaiting = false;
        std::array<Loadout, MAX_SLOTS + 1> slots;   // [1..MAX_SLOTS]
        std::set<uint8> bought;                     // slots 3+ unlocked
        // a load in progress
        uint32 loadToken = 0;
        uint8 loading = 0;
        uint32 elapsed = 0;
        Position start;
        uint32 startMap = 0;
        time_t cooldownUntil = 0;
    };

    State* GetState(Player* player) { return player ? player->CustomData.Get<State>(STATE_KEY) : nullptr; }

    std::string DefaultName(uint8 slot) { return "Loadout " + std::to_string(slot); }

    // names: no separators or colour codes, at most NAME_BYTES bytes, not cut inside a UTF-8 character
    std::string CleanName(std::string name)
    {
        for (char& c : name)
            if (c == '~' || c == '\t' || c == '|' || c == '\n' || c == '\r' || c == '\\' || c == '"' || c == '\'' || c == '`')
                c = ' ';
        size_t b = name.find_first_not_of(' ');
        name = b == std::string::npos ? std::string() : name.substr(b);
        if (name.size() > NAME_BYTES)
        {
            size_t cut = NAME_BYTES;
            while (cut > 0 && (uint8(name[cut]) & 0xC0) == 0x80)
                --cut;
            name.resize(cut);
        }
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        return name;
    }

    // ---- serialisation (characters DB) ----
    std::string TalentsToText(std::map<uint32, uint8> const& t)
    {
        std::string out;
        for (auto const& [id, rank] : t)
        {
            if (!out.empty())
                out += ',';
            out += std::to_string(id) + ':' + std::to_string(rank);
        }
        return out;
    }

    std::map<uint32, uint8> TalentsFromText(std::string const& text)
    {
        std::map<uint32, uint8> out;
        for (std::string_view entry : Acore::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> p = Acore::Tokenize(entry, ':', false);
            if (p.size() != 2)
                continue;
            uint32 id = Acore::StringTo<uint32>(p[0]).value_or(0);
            uint32 rank = Acore::StringTo<uint32>(p[1]).value_or(0);
            if (id && rank && rank <= MAX_TALENT_RANK)
                out[id] = uint8(rank);
        }
        return out;
    }

    std::string GlyphsToText(std::array<uint32, GLYPH_SLOTS> const& g)
    {
        std::string out;
        for (uint8 i = 0; i < GLYPH_SLOTS; ++i)
            out += (i ? "," : "") + std::to_string(g[i]);
        return out;
    }

    std::array<uint32, GLYPH_SLOTS> GlyphsFromText(std::string const& text)
    {
        std::array<uint32, GLYPH_SLOTS> out{};
        uint8 i = 0;
        for (std::string_view part : Acore::Tokenize(text, ',', true))
        {
            if (i >= GLYPH_SLOTS)
                break;
            out[i++] = Acore::StringTo<uint32>(part).value_or(0);
        }
        return out;
    }

    std::string ActionsToText(std::map<uint8, uint32> const& a)
    {
        std::string out;
        for (auto const& [button, packed] : a)
        {
            if (!out.empty())
                out += ',';
            out += std::to_string(button) + ':' + std::to_string(packed);
        }
        return out;
    }

    std::map<uint8, uint32> ActionsFromText(std::string const& text)
    {
        std::map<uint8, uint32> out;
        for (std::string_view entry : Acore::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> p = Acore::Tokenize(entry, ':', false);
            if (p.size() != 2)
                continue;
            uint32 button = Acore::StringTo<uint32>(p[0]).value_or(MAX_ACTION_BUTTONS);
            if (button >= MAX_ACTION_BUTTONS)
                continue;
            out[uint8(button)] = Acore::StringTo<uint32>(p[1]).value_or(0);
        }
        return out;
    }

    void WriteSlot(Player* player, uint8 slot, Loadout const& l)
    {
        std::string name = l.name;
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.Execute("REPLACE INTO `mod_temucraft_loadouts` (`guid`, `slot`, `name`, `talents`, `glyphs`, `actions`, `saved`) "
            "VALUES ({}, {}, '{}', '{}', '{}', '{}', {})", player->GetGUID().GetCounter(), slot, name, TalentsToText(l.talents),
            GlyphsToText(l.glyphs), l.hasActions ? ActionsToText(l.actions) : std::string(), l.saved ? uint32(GameTime::GetGameTime().count()) : 0u);
    }

    // ---- slots ----
    enum SlotState : uint8 { SLOT_LOCKED = 0, SLOT_NEXT = 1, SLOT_NEEDS_DUAL = 2, SLOT_EMPTY = 3, SLOT_SAVED = 4 };

    uint8 NextToBuy(State const& st)
    {
        for (uint8 n = 3; n <= cfg.slots; ++n)
            if (!st.bought.count(n))
                return n;
        return 0;
    }

    bool Unlocked(Player* player, State const& st, uint8 slot)
    {
        if (slot == 1)
            return true;
        if (slot == 2)
            return !cfg.dualSpecSlot || player->GetSpecsCount() > 1;
        return st.bought.count(slot) != 0;
    }

    SlotState GetSlotState(Player* player, State const& st, uint8 slot)
    {
        if (Unlocked(player, st, slot))
            return st.slots[slot].saved ? SLOT_SAVED : SLOT_EMPTY;
        if (slot == 2)
            return SLOT_NEEDS_DUAL;
        return slot == NextToBuy(st) ? SLOT_NEXT : SLOT_LOCKED;
    }

    std::string SlotName(State const& st, uint8 slot)
    {
        Loadout const& l = st.slots[slot];
        return l.name.empty() ? DefaultName(slot) : l.name;
    }

    // points per talent tree (tab page 0, 1, 2)
    std::array<uint32, 3> TreePoints(std::map<uint32, uint8> const& talents)
    {
        std::array<uint32, 3> out{};
        for (auto const& [id, rank] : talents)
            if (TalentEntry const* t = sTalentStore.LookupEntry(id))
                if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(t->TalentTab))
                    if (tab->tabpage < 3)
                        out[tab->tabpage] += rank;
        return out;
    }

    std::string TreeText(std::map<uint32, uint8> const& talents)
    {
        std::array<uint32, 3> p = TreePoints(talents);
        return Acore::StringFormat("{}/{}/{}", p[0], p[1], p[2]);
    }

    std::string CostText(Player* player, uint8 slot)
    {
        uint32 tokens = Price(cfg.tokenCost, slot), gold = Price(cfg.goldCost, slot);
        if (!tokens && !gold)
            return Say(player, T_COST_FREE, "free");
        std::string t = tokens ? Acore::StringFormat(Say(player, T_COST_TOKENS, "{} Temucraft Tokens"), tokens) : std::string();
        std::string g = gold ? Acore::StringFormat(Say(player, T_COST_GOLD, "{} gold"), gold) : std::string();
        if (t.empty())
            return g;
        if (g.empty())
            return t;
        return Acore::StringFormat(Say(player, T_COST_OR, "{} or {}"), t, g);
    }

    // ---- the client ----
    void SendList(Player* player)
    {
        State* st = GetState(player);
        if (!st)
            return;
        if (!st->loaded)
        {
            st->helloWaiting = true;
            return;
        }
        time_t now = GameTime::GetGameTime().count();
        uint32 cooldownLeft = st->cooldownUntil > now ? uint32(st->cooldownUntil - now) : 0;
        TemucraftAddonSend(player, PREFIX, Acore::StringFormat("S~{}~{}~{}~{}~{}~{}~{}", cfg.enable ? 1 : 0, cfg.slots, cfg.castTime,
            cooldownLeft, cfg.saveActions ? 1 : 0, cfg.saveGlyphs ? 1 : 0, cfg.tokenItem));
        for (uint8 n = 1; n <= cfg.slots; ++n)
        {
            SlotState s = GetSlotState(player, *st, n);
            Loadout const& l = st->slots[n];
            TemucraftAddonSend(player, PREFIX, Acore::StringFormat("L~{}~{}~{}~{}~{}~{}", n, uint32(s), Price(cfg.tokenCost, n),
                Price(cfg.goldCost, n), TreeText(l.talents), TemucraftAddonClean(SlotName(*st, n))));
        }
        TemucraftAddonSend(player, PREFIX, "E");
    }

    void Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // common checks: the module is on and the character's rows are read. Returns the state or nullptr (told why).
    State* Ready(Player* player)
    {
        if (!cfg.enable)
        {
            Tell(player, Say(player, T_OFF, "Talent loadouts are turned off."));
            return nullptr;
        }
        State* st = GetState(player);
        if (!st || !st->loaded)
        {
            Tell(player, Say(player, T_NOT_LOADED, "Your loadouts are still loading, try again in a moment."));
            return nullptr;
        }
        return st;
    }

    bool ValidSlot(Player* player, uint8 slot)
    {
        if (slot >= 1 && slot <= cfg.slots)
            return true;
        Tell(player, Acore::StringFormat(Say(player, T_NO_SLOT, "There is no loadout slot {}. The slots are 1 to {}."), slot, cfg.slots));
        return false;
    }

    // the slot can be used (saved to, loaded, renamed); tells why not
    bool UsableSlot(Player* player, State const& st, uint8 slot)
    {
        if (!ValidSlot(player, slot))
            return false;
        SlotState s = GetSlotState(player, st, slot);
        if (s == SLOT_NEEDS_DUAL)
        {
            Tell(player, Say(player, T_NEEDS_DUAL, "Loadout slot 2 comes with Dual Talent Specialization, from your class trainer."));
            return false;
        }
        if (s == SLOT_LOCKED || s == SLOT_NEXT)
        {
            Tell(player, Acore::StringFormat(Say(player, T_LOCKED, "Loadout slot {} is locked. Unlock it with .loadout unlock {} ({})."),
                slot, slot, CostText(player, slot)));
            return false;
        }
        return true;
    }

    void ShowList(Player* player)
    {
        State* st = Ready(player);
        if (!st)
            return;
        uint32 open = 0;
        for (uint8 n = 1; n <= cfg.slots; ++n)
            if (Unlocked(player, *st, n))
                ++open;
        Tell(player, Acore::StringFormat(Say(player, T_LIST_HEAD, "|cff66ccffTalent loadouts|r ({} of {} unlocked):"), open, cfg.slots));
        for (uint8 n = 1; n <= cfg.slots; ++n)
        {
            switch (GetSlotState(player, *st, n))
            {
                case SLOT_SAVED:
                    Tell(player, Acore::StringFormat(Say(player, T_LIST_SAVED, "  {}. {} - {}"), n, SlotName(*st, n), TreeText(st->slots[n].talents)));
                    break;
                case SLOT_EMPTY:
                    Tell(player, Acore::StringFormat(Say(player, T_LIST_EMPTY, "  {}. {} - empty"), n, SlotName(*st, n)));
                    break;
                case SLOT_NEEDS_DUAL:
                    Tell(player, Acore::StringFormat(Say(player, T_LIST_DUAL, "  {}. needs Dual Talent Specialization (class trainer)"), n));
                    break;
                case SLOT_NEXT:
                    Tell(player, Acore::StringFormat(Say(player, T_LIST_LOCKED_NEXT, "  {}. locked - unlock for {}"), n, CostText(player, n)));
                    break;
                default:
                    Tell(player, Acore::StringFormat(Say(player, T_LIST_LOCKED, "  {}. locked"), n));
                    break;
            }
        }
        Tell(player, Say(player, T_LIST_HINT, "  .loadout save <n>, load <n>, rename <n> <name>, unlock <n> token or gold"));
    }

    void Save(Player* player, uint8 slot)
    {
        State* st = Ready(player);
        if (!st || !UsableSlot(player, *st, slot))
            return;
        if (st->loading)
        {
            Tell(player, Say(player, T_BUSY, "A loadout is being loaded."));
            return;
        }
        Loadout& l = st->slots[slot];
        l.talents.clear();
        uint8 spec = player->GetActiveSpec();
        for (auto const& [spellId, talent] : player->GetTalentMap())
        {
            if (!talent || talent->State == PLAYERSPELL_REMOVED || !talent->IsInSpec(spec))
                continue;
            TalentSpellPos const* pos = GetTalentSpellPos(spellId);
            if (!pos)
                continue;
            uint8& rank = l.talents[pos->talent_id];
            rank = std::max<uint8>(rank, pos->rank + 1);
        }
        for (uint8 g = 0; g < GLYPH_SLOTS; ++g)
            l.glyphs[g] = player->GetGlyph(g);
        l.actions.clear();
        l.hasActions = cfg.saveActions;
        if (cfg.saveActions)
            for (uint8 b = 0; b < MAX_ACTION_BUTTONS; ++b)
                if (ActionButton const* ab = player->GetActionButton(b))
                    if (ab->packedData)
                        l.actions[b] = ab->packedData;
        if (l.name.empty())
            l.name = DefaultName(slot);
        l.exists = l.saved = true;
        WriteSlot(player, slot, l);
        Tell(player, Acore::StringFormat(Say(player, T_SAVED, "Your talents were saved to loadout {} ({}, {})."), slot, l.name, TreeText(l.talents)));
        SendList(player);
    }

    void Rename(Player* player, uint8 slot, std::string const& raw)
    {
        State* st = Ready(player);
        if (!st || !UsableSlot(player, *st, slot))
            return;
        std::string name = CleanName(raw);
        if (name.empty())
        {
            Tell(player, Say(player, T_BAD_NAME, "Give the loadout a name: .loadout rename <n> <name>."));
            return;
        }
        Loadout& l = st->slots[slot];
        l.name = name;
        l.exists = true;
        WriteSlot(player, slot, l);
        Tell(player, Acore::StringFormat(Say(player, T_RENAMED, "Loadout {} is now called {}."), slot, name));
        SendList(player);
    }

    void Unlock(Player* player, uint8 slot, std::string currency)
    {
        State* st = Ready(player);
        if (!st || !ValidSlot(player, slot))
            return;
        SlotState s = GetSlotState(player, *st, slot);
        if (s == SLOT_NEEDS_DUAL)
        {
            Tell(player, Say(player, T_NEEDS_DUAL, "Loadout slot 2 comes with Dual Talent Specialization, from your class trainer."));
            return;
        }
        if (s == SLOT_EMPTY || s == SLOT_SAVED)
        {
            Tell(player, Acore::StringFormat(Say(player, T_ALREADY, "Loadout slot {} is already unlocked."), slot));
            return;
        }
        if (s == SLOT_LOCKED)
        {
            Tell(player, Acore::StringFormat(Say(player, T_UNLOCK_ORDER, "Slots are unlocked in order: slot {} comes first."), NextToBuy(*st)));
            return;
        }
        uint32 tokens = Price(cfg.tokenCost, slot), gold = Price(cfg.goldCost, slot);
        std::transform(currency.begin(), currency.end(), currency.begin(), ::tolower);
        bool isFree = !tokens && !gold;
        bool byToken = currency == "token" || currency == "tokens" || currency == "t";
        bool byGold = currency == "gold" || currency == "g";
        if (!isFree && !byToken && !byGold)
        {
            Tell(player, Acore::StringFormat(Say(player, T_UNLOCK_HOW, "Unlocking loadout slot {} costs {}. To buy it: .loadout unlock {} token or .loadout unlock {} gold."),
                slot, CostText(player, slot), slot, slot));
            return;
        }
        std::string paid = "free";
        uint32 cost = 0;
        if (!isFree)
        {
            if ((byToken && !tokens) || (byGold && !gold))
            {
                Tell(player, Acore::StringFormat(Say(player, T_NOT_OFFERED, "Slot {} can't be bought that way: it costs {}."), slot, CostText(player, slot)));
                return;
            }
            if (byToken)
            {
                if (!cfg.tokenItem || !player->HasItemCount(cfg.tokenItem, tokens))
                {
                    Tell(player, Acore::StringFormat(Say(player, T_NO_TOKENS, "You need {} Temucraft Tokens; you have {}."), tokens,
                        cfg.tokenItem ? player->GetItemCount(cfg.tokenItem) : 0));
                    return;
                }
                player->DestroyItemCount(cfg.tokenItem, tokens, true);
                paid = "token";
                cost = tokens;
            }
            else
            {
                if (!player->HasEnoughMoney(gold * GOLD))
                {
                    Tell(player, Acore::StringFormat(Say(player, T_NO_GOLD, "You need {} gold."), gold));
                    return;
                }
                player->ModifyMoney(-int32(gold * GOLD));
                paid = "gold";
                cost = gold;
            }
        }
        st->bought.insert(slot);
        CharacterDatabase.Execute("INSERT INTO `mod_temucraft_loadouts_unlock` (`account`, `guid`, `slot`, `paid`, `cost`, `time`) VALUES ({}, {}, {}, '{}', {}, {})",
            player->GetSession()->GetAccountId(), player->GetGUID().GetCounter(), slot, paid, cost, uint32(GameTime::GetGameTime().count()));
        LOG_INFO("module", "[Loadouts] {} (GUID {}, account {}) unlocked slot {} ({} {})", player->GetName(), player->GetGUID().GetCounter(),
            player->GetSession()->GetAccountId(), slot, cost, paid);
        Tell(player, Acore::StringFormat(Say(player, T_UNLOCKED, "Loadout slot {} unlocked."), slot));
        SendList(player);
    }

    // can a loadout be loaded now? (tells why not)
    bool CanLoadNow(Player* player, State const& st)
    {
        if (!player->IsAlive())
        {
            Tell(player, Say(player, T_DEAD, "You can't change talents while dead."));
            return false;
        }
        if (player->IsInCombat())
        {
            Tell(player, Say(player, T_COMBAT, "You can't change talents in combat."));
            return false;
        }
        if (player->IsInFlight())
        {
            Tell(player, Say(player, T_FLIGHT, "You can't change talents while flying."));
            return false;
        }
        if (player->GetLevel() < cfg.minLevel)
        {
            Tell(player, Acore::StringFormat(Say(player, T_LEVEL, "Loadouts can be loaded from level {}."), cfg.minLevel));
            return false;
        }
        if (player->InBattleground() || player->InArena())
        {
            Battleground const* bg = player->GetBattleground();
            if (!cfg.allowInBattleground || (bg && bg->GetStatus() == STATUS_IN_PROGRESS))
            {
                Tell(player, Say(player, T_BG, "You can't change talents in a battleground or arena."));
                return false;
            }
        }
        if (Map* map = player->GetMap())
            if (map->IsDungeon())
                if (InstanceScript* instance = player->GetInstanceScript())
                    if (instance->IsEncounterInProgress())
                    {
                        Tell(player, Say(player, T_ENCOUNTER, "You can't change talents during a boss fight."));
                        return false;
                    }
        time_t now = GameTime::GetGameTime().count();
        if (st.cooldownUntil > now)
        {
            Tell(player, Acore::StringFormat(Say(player, T_COOLDOWN, "You can load another loadout in {} s."), uint32(st.cooldownUntil - now)));
            return false;
        }
        return true;
    }

    void RemoveGlyphAura(Player* player, uint32 glyphId)
    {
        GlyphPropertiesEntry const* old = sGlyphPropertiesStore.LookupEntry(glyphId);
        if (!old)
            return;
        player->RemoveAurasDueToSpell(old->SpellId);
        Unit::AuraMap& owned = player->GetOwnedAuras();
        for (Unit::AuraMap::iterator itr = owned.begin(); itr != owned.end();)
        {
            SpellInfo const* by = itr->second->GetTriggeredByAuraSpellInfo();
            if (by && by->Id == old->SpellId)
            {
                player->RemoveOwnedAura(itr);
                continue;
            }
            ++itr;
        }
        player->SendLearnPacket(old->SpellId, false);
    }

    // the glyphs of the active talent group, as the glyph spell effect sets them (socket open, slot type matching)
    void ApplyGlyphs(Player* player, std::array<uint32, GLYPH_SLOTS> const& glyphs)
    {
        uint32 enabled = player->GetUInt32Value(PLAYER_GLYPHS_ENABLED);
        for (uint8 slot = 0; slot < GLYPH_SLOTS; ++slot)
        {
            uint32 want = glyphs[slot];
            uint32 have = player->GetGlyph(slot);
            if (want == have)
                continue;
            GlyphPropertiesEntry const* entry = want ? sGlyphPropertiesStore.LookupEntry(want) : nullptr;
            if (want)
            {
                if (!entry || !(enabled & (1 << slot)))
                    continue;   // socket not open (level) or unknown glyph: keep what is there
                GlyphSlotEntry const* slotEntry = sGlyphSlotStore.LookupEntry(player->GetGlyphSlot(slot));
                if (slotEntry && slotEntry->TypeFlags != entry->TypeFlags)
                    continue;
            }
            if (have)
                RemoveGlyphAura(player, have);
            if (entry)
            {
                player->SendLearnPacket(entry->SpellId, true);
                player->CastSpell(player, entry->SpellId, TriggerCastFlags(TRIGGERED_FULL_MASK & ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
            }
            player->SetGlyph(slot, entry ? want : 0, true);
        }
    }

    void ApplyActions(Player* player, std::map<uint8, uint32> const& actions)
    {
        for (uint8 b = 0; b < MAX_ACTION_BUTTONS; ++b)
        {
            auto it = actions.find(b);
            if (it == actions.end() || !it->second)
            {
                player->removeActionButton(b);
                continue;
            }
            uint32 action = ACTION_BUTTON_ACTION(it->second);
            uint8 type = uint8(ACTION_BUTTON_TYPE(it->second));
            if (!player->addActionButton(b, action, type))
                player->removeActionButton(b);   // a spell this build doesn't have
        }
        player->SendActionButtons(1);
    }

    // the swap itself
    void DoLoad(Player* player, State& st, uint8 slot)
    {
        Loadout const& l = st.slots[slot];

        // 1. the active group's talents, without the gold cost (this also gives back every point)
        player->resetTalents(true);

        // 2. the stored talents, tier by tier, through the core's own checks (points, tiers, prerequisites)
        struct Want { TalentEntry const* talent; uint8 rank; };
        std::vector<Want> wants;
        uint32 wanted = 0;
        for (auto const& [id, rank] : l.talents)
        {
            TalentEntry const* t = sTalentStore.LookupEntry(id);
            if (!t)
                continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(t->TalentTab);
            if (!tab || !(tab->ClassMask & player->getClassMask()))
                continue;
            wants.push_back({ t, rank });
            wanted += rank;
        }
        std::sort(wants.begin(), wants.end(), [](Want const& a, Want const& b)
        {
            return a.talent->Row != b.talent->Row ? a.talent->Row < b.talent->Row : a.talent->TalentID < b.talent->TalentID;
        });
        auto CurrentRank = [player](TalentEntry const* t) -> uint8
        {
            for (int8 r = MAX_TALENT_RANK - 1; r >= 0; --r)
                if (t->RankID[r] && player->HasTalent(t->RankID[r], player->GetActiveSpec()))
                    return uint8(r + 1);
            return 0;
        };
        for (uint32 pass = 0; pass < 16; ++pass)
        {
            bool progress = false;
            for (Want const& w : wants)
            {
                uint8 have = CurrentRank(w.talent);
                if (have >= w.rank || !player->GetFreeTalentPoints())
                    continue;
                // the highest rank that fits in the points left
                uint8 target = uint8(std::min<uint32>(w.rank, have + player->GetFreeTalentPoints()));
                player->LearnTalent(w.talent->TalentID, target - 1);
                if (CurrentRank(w.talent) > have)
                    progress = true;
            }
            if (!progress)
                break;
        }
        uint32 placed = 0;
        for (Want const& w : wants)
            placed += std::min(CurrentRank(w.talent), w.rank);

        // 3. glyphs and action bars
        if (cfg.saveGlyphs)
            ApplyGlyphs(player, l.glyphs);
        if (cfg.saveActions && l.hasActions)
            ApplyActions(player, l.actions);

        player->SendTalentsInfoData(false);

        st.cooldownUntil = GameTime::GetGameTime().count() + cfg.cooldown;
        Tell(player, Acore::StringFormat(Say(player, T_LOADED, "Loadout {} ({}) loaded."), slot, SlotName(st, slot)));
        if (placed < wanted)
            Tell(player, Acore::StringFormat(Say(player, T_MISSING_POINTS, "{} of its talent points could not be placed (not enough points or a missing prerequisite): spend them yourself."),
                wanted - placed));
        LOG_DEBUG("module", "[Loadouts] {} loaded slot {} ({} of {} points)", player->GetName(), slot, placed, wanted);
        TemucraftAddonSend(player, PREFIX, Acore::StringFormat("D~{}", slot));
        SendList(player);
    }

    void CancelLoad(Player* player, State& st, bool tell)
    {
        if (!st.loading)
            return;
        uint8 slot = st.loading;
        st.loading = 0;
        ++st.loadToken;
        if (tell)
            Tell(player, Say(player, T_CANCELLED, "Loading the loadout was interrupted."));
        TemucraftAddonSend(player, PREFIX, Acore::StringFormat("X~{}", slot));
    }

    // the "cast": checked every TICK_MS; moving, combat, casting, dying or changing map interrupts it
    void Tick(Player* player, uint32 token)
    {
        State* st = GetState(player);
        if (!st || st->loadToken != token || !st->loading)
            return;
        bool moved = player->GetMapId() != st->startMap || player->GetExactDist(&st->start) > 0.5f;
        if (moved || player->IsInCombat() || !player->IsAlive() || player->IsNonMeleeSpellCast(false) || player->IsInFlight())
        {
            CancelLoad(player, *st, true);
            return;
        }
        st->elapsed += TICK_MS;
        if (st->elapsed < cfg.castTime)
        {
            player->m_Events.AddEventAtOffset([player, token]() { Tick(player, token); }, Milliseconds(TICK_MS));
            return;
        }
        uint8 slot = st->loading;
        st->loading = 0;
        ++st->loadToken;
        if (CanLoadNow(player, *st))
            DoLoad(player, *st, slot);
        else
            TemucraftAddonSend(player, PREFIX, Acore::StringFormat("X~{}", slot));
    }

    void Load(Player* player, uint8 slot)
    {
        State* st = Ready(player);
        if (!st || !UsableSlot(player, *st, slot))
            return;
        if (!st->slots[slot].saved)
        {
            Tell(player, Acore::StringFormat(Say(player, T_EMPTY, "Loadout {} is empty: save your talents to it first (.loadout save {})."), slot, slot));
            return;
        }
        if (st->loading)
        {
            Tell(player, Say(player, T_BUSY, "A loadout is being loaded."));
            return;
        }
        if (!CanLoadNow(player, *st))
            return;
        if (player->IsNonMeleeSpellCast(false))
        {
            Tell(player, Say(player, T_CASTING, "You are casting something."));
            return;
        }
        if (!cfg.castTime)
        {
            DoLoad(player, *st, slot);
            return;
        }
        st->loading = slot;
        st->elapsed = 0;
        st->start = player->GetPosition();
        st->startMap = player->GetMapId();
        uint32 token = ++st->loadToken;
        Tell(player, Acore::StringFormat(Say(player, T_LOADING, "Loading loadout {} ({})... stand still for {:.1f} s."), slot, SlotName(*st, slot), cfg.castTime / 1000.0f));
        TemucraftAddonSend(player, PREFIX, Acore::StringFormat("C~{}~{}", slot, cfg.castTime));
        player->m_Events.AddEventAtOffset([player, token]() { Tick(player, token); }, Milliseconds(TICK_MS));
    }

    // ---- login ----
    void Read(WorldSession* session, std::string const& sql, std::function<void(Player*, QueryResult)> answer)
    {
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(sql).WithCallback([session, answer](QueryResult r)
        {
            if (Player* player = session->GetPlayer())
                answer(player, r);
        }));
    }

    void OnLogin(Player* player)
    {
        WorldSession* session = player->GetSession();
        if (!session)
            return;
        player->CustomData.Set(STATE_KEY, new State());
        uint32 guid = player->GetGUID().GetCounter();
        uint32 account = session->GetAccountId();
        Read(session, Acore::StringFormat("SELECT `slot`, `name`, `talents`, `glyphs`, `actions`, `saved` FROM `mod_temucraft_loadouts` WHERE `guid` = {}", guid),
            [session, guid, account](Player* p, QueryResult r)
        {
            State* st = GetState(p);
            if (!st)
                return;
            if (r)
                do
                {
                    Field* f = r->Fetch();
                    uint8 slot = f[0].Get<uint8>();
                    if (slot < 1 || slot > MAX_SLOTS)
                        continue;
                    Loadout& l = st->slots[slot];
                    l.exists = true;
                    l.name = f[1].Get<std::string>();
                    l.talents = TalentsFromText(f[2].Get<std::string>());
                    l.glyphs = GlyphsFromText(f[3].Get<std::string>());
                    std::string actions = f[4].Get<std::string>();
                    l.actions = ActionsFromText(actions);
                    l.hasActions = !actions.empty();
                    l.saved = f[5].Get<uint32>() != 0;
                } while (r->NextRow());
            std::string sql = cfg.accountWide
                ? Acore::StringFormat("SELECT DISTINCT `slot` FROM `mod_temucraft_loadouts_unlock` WHERE `account` = {}", account)
                : Acore::StringFormat("SELECT DISTINCT `slot` FROM `mod_temucraft_loadouts_unlock` WHERE `guid` = {}", guid);
            Read(session, sql, [](Player* p2, QueryResult r2)
            {
                State* st2 = GetState(p2);
                if (!st2)
                    return;
                if (r2)
                    do
                    {
                        uint8 slot = r2->Fetch()[0].Get<uint8>();
                        if (slot >= 3 && slot <= MAX_SLOTS)
                            st2->bought.insert(slot);
                    } while (r2->NextRow());
                st2->loaded = true;
                if (st2->helloWaiting)
                {
                    st2->helloWaiting = false;
                    SendList(p2);
                }
            });
        });
    }
}

class TemucraftLoadouts_Player : public PlayerScript
{
public:
    TemucraftLoadouts_Player() : PlayerScript("TemucraftLoadouts_Player", {
        PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_ON_PLAYER_ENTER_COMBAT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    void OnPlayerLogin(Player* player) override { OnLogin(player); }

    // deleted for good: the builds go; the unlock rows stay with guid 0 (account-wide unlocks, and the record of the payment)
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_temucraft_loadouts` WHERE `guid` = {}", guid);
        trans->Append("UPDATE `mod_temucraft_loadouts_unlock` SET `guid` = 0 WHERE `guid` = {}", guid);
    }

    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (State* st = GetState(player))
            CancelLoad(player, *st, true);
    }

    // the client's requests
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        std::string body;
        if (!TemucraftAddonParse(player, type, language, msg, receiver, PREFIX, body))
            return true;
        std::vector<std::string> f = TemucraftAddonFields(body);
        if (f.empty())
            return false;
        uint8 slot = f.size() > 1 ? uint8(std::min<uint32>(Acore::StringTo<uint32>(f[1]).value_or(0), 255)) : 0;
        if (f[0] == "HELLO" || f[0] == "LIST")
            SendList(player);
        else if (f[0] == "SAVE")
            Save(player, slot);
        else if (f[0] == "LOAD")
            Load(player, slot);
        else if (f[0] == "UNLOCK")
            Unlock(player, slot, f.size() > 2 ? f[2] : std::string());
        else if (f[0] == "RENAME")
            Rename(player, slot, f.size() > 2 ? f[2] : std::string());
        else if (f[0] == "CANCEL")
        {
            if (State* st = GetState(player))
                CancelLoad(player, *st, true);
        }
        return false;
    }
};

class TemucraftLoadouts_World : public WorldScript
{
public:
    TemucraftLoadouts_World() : WorldScript("TemucraftLoadouts_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override { LoadSettings(); }
};

class TemucraftLoadouts_Commands : public CommandScript
{
public:
    TemucraftLoadouts_Commands() : CommandScript("TemucraftLoadouts_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable loadout =
        {
            { "list",   HandleList,   104351, Console::No },
            { "save",   HandleSave,   104351, Console::No },
            { "load",   HandleLoad,   104351, Console::No },
            { "unlock", HandleUnlock, 104351, Console::No },
            { "rename", HandleRename, 104351, Console::No },
        };
        static ChatCommandTable root =
        {
            { "loadout", loadout },
        };
        return root;
    }

    static Player* Me(ChatHandler* handler) { return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr; }

    static bool HandleList(ChatHandler* handler)
    {
        if (Player* p = Me(handler))
            ShowList(p);
        return true;
    }

    static bool HandleSave(ChatHandler* handler, uint8 slot)
    {
        if (Player* p = Me(handler))
            Save(p, slot);
        return true;
    }

    static bool HandleLoad(ChatHandler* handler, uint8 slot)
    {
        if (Player* p = Me(handler))
            Load(p, slot);
        return true;
    }

    static bool HandleUnlock(ChatHandler* handler, uint8 slot, Optional<std::string> currency)
    {
        if (Player* p = Me(handler))
            Unlock(p, slot, currency ? *currency : std::string());
        return true;
    }

    static bool HandleRename(ChatHandler* handler, uint8 slot, Tail name)
    {
        if (Player* p = Me(handler))
            Rename(p, slot, std::string(name));
        return true;
    }
};

void AddTemucraftLoadoutsScripts()
{
    new TemucraftLoadouts_Player();
    new TemucraftLoadouts_World();
    new TemucraftLoadouts_Commands();
}
