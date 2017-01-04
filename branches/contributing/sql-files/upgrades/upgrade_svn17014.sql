ALTER TABLE `elemental` CHANGE COLUMN `str` `atk1` MEDIUMINT(6) UNSIGNED NOT NULL DEFAULT 0,
 CHANGE COLUMN `agi` `atk2` MEDIUMINT(6) UNSIGNED NOT NULL DEFAULT 0,
 CHANGE COLUMN `vit` `matk` MEDIUMINT(6) UNSIGNED NOT NULL DEFAULT 0,
 CHANGE COLUMN `int` `aspd` SMALLINT(4) UNSIGNED NOT NULL DEFAULT 0,
 CHANGE COLUMN `dex` `def` SMALLINT(4) UNSIGNED NOT NULL DEFAULT 0,
 CHANGE COLUMN `luk` `mdef` SMALLINT(4) UNSIGNED NOT NULL DEFAULT 0,
 CHANGE COLUMN `life_time` `flee` SMALLINT(4) UNSIGNED NOT NULL DEFAULT 0,
 ADD COLUMN `hit` SMALLINT(4) UNSIGNED NOT NULL DEFAULT 0 AFTER `flee`,
 ADD COLUMN `life_time` INT(11) NOT NULL DEFAULT 0 AFTER `hit`;
 