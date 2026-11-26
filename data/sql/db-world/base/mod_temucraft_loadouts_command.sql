-- mod-temucraft-loadouts: help texts of .loadout. `security` is the commands' own RBAC permission (104351): the core
-- puts this column's value over the one in the code, so 0 (player level) would open .loadout without the permission.
-- The column must hold it: the core's pending update rev_1795737600000000000.sql makes it INT, but update files run
-- in file name order (this one first), so it is widened here too (the same ALTER, safe to run again).
ALTER TABLE `command` MODIFY `security` INT UNSIGNED NOT NULL DEFAULT 0;
DELETE FROM `command` WHERE `name` IN ('loadout', 'loadout list', 'loadout save', 'loadout load', 'loadout unlock', 'loadout rename');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('loadout', 104351, 'Syntax: .loadout list, save <n>, load <n>, unlock <n> [token or gold], rename <n> <name>\r\nTalent loadouts: saved talent builds (with glyphs and action bars) you can load into your active talent group.'),
('loadout list', 104351, 'Syntax: .loadout list\r\nYour loadout slots: their names and builds, which are locked and what unlocking the next one costs.'),
('loadout save', 104351, 'Syntax: .loadout save <n>\r\nSaves the talents, glyphs and action bars of your active talent group to loadout <n>.'),
('loadout load', 104351, 'Syntax: .loadout load <n>\r\nLoads loadout <n> into your active talent group (out of combat, standing still for a moment). No gold cost.'),
('loadout unlock', 104351, 'Syntax: .loadout unlock <n> [token or gold]\r\nShows what unlocking slot <n> costs; with token or gold, buys it. Slots are unlocked in order.'),
('loadout rename', 104351, 'Syntax: .loadout rename <n> <name>\r\nNames loadout <n> (at most 24 letters).');
