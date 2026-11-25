-- mod-temucraft-loadouts: help texts of .loadout
DELETE FROM `command` WHERE `name` IN ('loadout', 'loadout list', 'loadout save', 'loadout load', 'loadout unlock', 'loadout rename');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('loadout', 0, 'Syntax: .loadout list, save <n>, load <n>, unlock <n> [token or gold], rename <n> <name>\r\nTalent loadouts: saved talent builds (with glyphs and action bars) you can load into your active talent group.'),
('loadout list', 0, 'Syntax: .loadout list\r\nYour loadout slots: their names and builds, which are locked and what unlocking the next one costs.'),
('loadout save', 0, 'Syntax: .loadout save <n>\r\nSaves the talents, glyphs and action bars of your active talent group to loadout <n>.'),
('loadout load', 0, 'Syntax: .loadout load <n>\r\nLoads loadout <n> into your active talent group (out of combat, standing still for a moment). No gold cost.'),
('loadout unlock', 0, 'Syntax: .loadout unlock <n> [token or gold]\r\nShows what unlocking slot <n> costs; with token or gold, buys it. Slots are unlocked in order.'),
('loadout rename', 0, 'Syntax: .loadout rename <n> <name>\r\nNames loadout <n> (at most 24 letters).');
