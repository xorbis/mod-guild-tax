-- mod-guild-tax: repairs get their own column (they were counted as withdrawals). Only for a
-- ledger created before this file; a fresh install gets the column from the base file, which the
-- updater may apply after this one, hence the guard on the table existing.
SET @has_table := (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'guild_member_ledger');
SET @has_column := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'guild_member_ledger' AND COLUMN_NAME = 'repairs');
SET @sql := IF(@has_table = 1 AND @has_column = 0,
    'ALTER TABLE `guild_member_ledger` ADD COLUMN `repairs` BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER `withdrawn`',
    'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
