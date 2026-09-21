-- mod-guild-tax: what each member has put into and taken out of the guild bank (in copper): their
-- own deposits, their withdrawals (guild repairs included) and the tax the module deposited for
-- them. One row per guild and character, created on the first booking; shown by ".guild ledger".
-- The bank's own money log only keeps the last few entries, this keeps the totals. On its first
-- start with an empty table the module fills it from what is still in that log.
CREATE TABLE IF NOT EXISTS `guild_member_ledger` (
  `guildid` INT UNSIGNED NOT NULL,
  `guid` INT UNSIGNED NOT NULL,
  `deposited` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `withdrawn` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `tax` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guildid`, `guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
