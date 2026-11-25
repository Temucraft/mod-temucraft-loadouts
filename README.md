# mod-temucraft-loadouts

Talent loadouts for Temucraft (AzerothCore 3.3.5a). The client has two talent groups (dual spec); this module keeps
up to 10 saved builds per character beside them and swaps one into the active talent group on request. The client
half is the Loadouts menu of the Temucraft talent window (`TemucraftTalents.lua` in
[mod-temucraft-client-version](https://github.com/Temucraft/mod-temucraft-client-version)); the chat command works
with any talent window (stock, DragonUI).

## For players

- **Slots.** Slot 1 is free. Slot 2 opens with Dual Talent Specialization, bought from the class trainer as usual
  (1000 gold in stock). Slots 3 to 10 are bought one at a time, in order, each for Temucraft Tokens *or* gold; the
  price rises per slot. By default unlocks are per character (`AccountWide = 1` shares them across the account).
- **Save** copies the active talent group into a slot: the learned talents (not the talent window's unapplied
  preview), the six glyphs and the action bars.
- **Load** swaps a slot into the active talent group: its talents are reset without the gold cost, the saved
  talents learned again, the glyphs set and the action bars put back. It takes 3 seconds of standing still (moving,
  combat, casting or dying interrupts it), then a 10 second cooldown. Not in combat, not during a boss fight, not
  in battlegrounds and arenas, not while flying or dead. The other talent group (dual spec) is not touched: switch
  to it the stock way and load a loadout there too.
  - Talents go through the core's own LearnTalent (point, tier and prerequisite checks, and every module's talent
    hooks). A build saved at a higher level than the character has now gets as many points as fit, tier by tier;
    the player is told how many points were left to spend.
  - A glyph is only put back in a socket the character has open. Glyphs are not used up (as with dual spec).
  - Hunters: resetting talents dismisses the pet (the stock respec does too): call it again.
- **Rename**: a name of up to 24 letters.

Chat command (every player, RBAC 104351):

| Command | |
|---|---|
| `.loadout list` | the slots, their builds (points per tree), which are locked, the next one's price |
| `.loadout save <n>` | save the active talent group to slot n |
| `.loadout load <n>` | load slot n into the active talent group |
| `.loadout unlock <n>` | show what slot n costs; `.loadout unlock <n> token` or `... gold` buys it |
| `.loadout rename <n> <name>` | name slot n |

## Settings

Copy `conf/mod-temucraft-loadouts.conf.dist` to `configs/modules/` as `mod-temucraft-loadouts.conf` (`.reload config`
applies them; `AccountWide` counts from the next login).

| Setting | Default | |
|---|---|---|
| `Temucraft.Loadouts.Enable` | 1 | the module |
| `Temucraft.Loadouts.Slots` | 10 | slots per character (1-10) |
| `Temucraft.Loadouts.DualSpecSlot` | 1 | slot 2 needs dual spec (0 = free) |
| `Temucraft.Loadouts.TokenItem` | 608000 | the Temucraft Token (mod-temucraft-shop) |
| `Temucraft.Loadouts.TokenCost` | `"2,3,4,5,6,8,10,12"` | tokens for slots 3, 4, ... (past the end: the last value; 0 = not for tokens) |
| `Temucraft.Loadouts.GoldCost` | `"500,1000,1500,2000,3000,4000,5000,7500"` | whole gold for slots 3, 4, ... (0 in both lists = free) |
| `Temucraft.Loadouts.AccountWide` | 0 | bought slots count for the whole account |
| `Temucraft.Loadouts.SaveActions` | 1 | save and restore the action bars |
| `Temucraft.Loadouts.SaveGlyphs` | 1 | save and restore the glyphs |
| `Temucraft.Loadouts.CastTime` | 3000 | ms of standing still before a load (0 = at once) |
| `Temucraft.Loadouts.Cooldown` | 10 | s between loads |
| `Temucraft.Loadouts.MinLevel` | 10 | loading from this level |
| `Temucraft.Loadouts.AllowInBattleground` | 0 | 1 = also in battlegrounds and arenas before they start |

## Install

1. Put this folder in `modules/` of the AzerothCore source, re-run CMake and build.
2. The SQL in `data/sql/` is applied by the worldserver on its next start:
   - characters: `mod_temucraft_loadouts` (the builds: guid, slot, name, talents `talentId:rank,...`, glyphs,
     action buttons `button:packed,...`, save time) and `mod_temucraft_loadouts_unlock` (account, guid, slot, paid
     with, cost, time). `CREATE TABLE IF NOT EXISTS` only.
   - world: the texts (`module_string`, translations in `module_string_locale`) and the help of `.loadout`
     (`command`).
   - auth: RBAC permission 104351 (range 104351-104399), linked to the player roles 195 and 1010. Adds rows only.
3. Optionally copy the config (above).

When a character is deleted for good its builds are deleted; its unlock rows are kept with `guid = 0` (they still
count for the account-wide mode, and they record what was paid).

## The client's messages

Addon messages with the prefix `TLO`, whispered to oneself (`AddonChannel = 1`, the default). The client sends
`HELLO` / `LIST`, `SAVE~n`, `LOAD~n`, `UNLOCK~n~token|gold`, `RENAME~n~name`, `CANCEL`; the server answers with the
settings (`S`), one `L` record per slot (state, price, points per tree, name), `E`, and `C` / `X` / `D` when a load
starts, is interrupted or is done. Details at the top of `src/TemucraftLoadouts.cpp`. Errors and results go to the
chat window as system messages, the same as for the command.

Bots (mod-playerbots) are left alone: nothing in the module acts for them.
