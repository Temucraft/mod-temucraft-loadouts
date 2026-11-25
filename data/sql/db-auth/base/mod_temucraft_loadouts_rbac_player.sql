-- mod-temucraft-loadouts: the player command .loadout (list, save, load, unlock, rename) as RBAC permission 104351
-- (range 104351-104399). Linked to the player roles 195 and 1010, so every account has it (the higher roles include
-- 195). Only adds rows (no DELETE): run again, it changes nothing.
INSERT IGNORE INTO `rbac_permissions` (`id`, `name`) VALUES
(104351, 'Command: loadout');
INSERT IGNORE INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(195, 104351),
(1010, 104351);
