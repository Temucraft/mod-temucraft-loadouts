-- mod-temucraft-loadouts: talent loadouts (characters DB). Only creates tables: run again, it changes nothing.
CREATE TABLE IF NOT EXISTS `mod_temucraft_loadouts` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `slot` TINYINT UNSIGNED NOT NULL COMMENT '1-10',
  `name` VARCHAR(48) NOT NULL DEFAULT '',
  `talents` TEXT NOT NULL COMMENT 'talentId:rank,... (Talent.dbc id, rank from 1)',
  `glyphs` VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'six GlyphProperties ids, comma separated',
  `actions` TEXT NOT NULL COMMENT 'button:packed,... (empty = action bars not stored)',
  `saved` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time of the last save, 0 = named but empty',
  PRIMARY KEY (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-loadouts: saved talent builds';

CREATE TABLE IF NOT EXISTS `mod_temucraft_loadouts_unlock` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `account` INT UNSIGNED NOT NULL,
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid, 0 once the character is deleted',
  `slot` TINYINT UNSIGNED NOT NULL COMMENT '3-10',
  `paid` VARCHAR(8) NOT NULL DEFAULT '' COMMENT 'token, gold or free',
  `cost` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'tokens or whole gold',
  `time` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_guid` (`guid`),
  KEY `idx_account` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-loadouts: bought loadout slots';
