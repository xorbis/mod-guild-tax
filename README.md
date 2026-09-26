# mod-guild-tax

An [AzerothCore](https://www.azerothcore.org/) module: a share of the money guild members loot and earn from quests goes to their guild bank. It is a tax, not a bonus - the money comes out of the member's pocket, nothing is created.

Each guild sets its own rate with a tag in its message of the day, so its officers control it without a GM or a config change:

```
[tax: 10%]
```

`[tax:10]` and `[Tax=5%]` work too; `[tax: 0%]` opts the guild out. Anyone whose guild rank may set the MOTD can change it, and every member sees the MOTD at login, so the rate is never a secret. The server caps it (`GuildTax.MaxPercent`, 50 by default) and provides the rate for guilds without a tag (`GuildTax.DefaultPercent`, 0 by default: guilds opt in).

## What is taxed

- Looted money: each member's actual share of what dropped from creatures and chests (a group splits it between the members in loot range), pickpocketed money, lockboxes and battleground insignias.
- Quest money: the level-scaled reward, and at the level cap the experience that is turned into money.

Vendor sales, mail, trades and the auction house are transfers, not income, and are left alone. Amounts are rounded down to the copper.

## How it is deposited

The tax goes through the same code path as the "Deposit" button of the guild bank window (`Guild::HandleMemberDepositMoney`), so every deposit shows in the bank's money log as "*Name* deposited *amount*" and large ones land in `log_money` like any other. Because that log only keeps the last few entries (`Guild.BankEventLogRecordsCount`, 25 by default), the tax is added up per character and deposited once it reaches `GuildTax.DepositThreshold` (1 gold by default), and in any case at the next character save: the autosave, logging out, or leaving the guild. Set the threshold to 0 to deposit at every loot and quest. The character keeps holding the money until the deposit; if it has been spent by then, the rest stays owed.

A character that leaves or is kicked pays what it accrued while a member. A guild that disbands loses its bank, so nothing is deposited there.

## The ledger: `.guild ledger`

The bank's money log only shows the last few entries, so the module keeps its own running total per member of what went through the bank: what they deposited themselves, what they withdrew, what the bank paid for their repairs and what they paid in tax. `.guild ledger` prints the members with their rank and those four amounts, plus the balance of the four (deposited + tax - withdrawn - repairs), guild master first, offline members included. The guild master and the officers - any rank that hears officer chat - see every member; anyone else sees the header and their own line:

```
No Name Noobs: 12 members, guild bank 1g 20s 39c, tax 15%
Xorbis (General): deposited 4g, withdrawn 27g 72s 36c, repairs 3g 14c, tax 2g 45s 80c, balance -24g 26s 70c
Dacrow (Captain): deposited 11g, withdrawn 1s 50c, repairs 0c, tax 2g 82s 9c, balance 13g 80s 59c
...
```

The totals are booked from the bank events the core logs once money has actually moved (`guild_member_ledger` in the characters database, one row per guild and character, in copper), so a refused deposit or withdrawal never shows. A member who leaves and comes back finds their numbers again; a disbanded guild's rows go with it.

The first time the module starts with an empty ledger it is built from what is still in the banks' money logs (the last `Guild.BankEventLogRecordsCount` entries of each guild, 25 by default), so a guild that predates the module does not start from zero. Those log entries do not say which deposits were the module's: in a guild with a tax rate, a deposit with a silver or copper part is taken to be tax (the module deposits the tax accrued so far, an odd amount) and a whole number of gold the member's own (what one types into the bank window); in a guild without a rate every deposit is the member's own. Anything that had already dropped out of the log is not in the ledger, so the balances need not add up to the bank's money.

## Bots

With [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) installed, the random and addclass bots do not pay the tax unless `GuildTax.Bots = 1`; such a bot in a player's guild would otherwise feed its bank a steady stream of gold. A player's own character played by the bot AI (`.playerbots bot add`) pays like its owner, since that gold is the player's.

## Installation

```
cd modules
git clone https://github.com/xorbis/mod-guild-tax.git
```

Re-run CMake and build. The `guild_member_ledger` table (`data/sql/db-characters/base`) is created in the characters database by the core's updater at the next start. The config template `conf/mod_guild_tax.conf.dist` is installed next to the worldserver config; copy it to `mod_guild_tax.conf` to change the defaults. `.reload config` picks up changes without a restart.

## License

GNU GPL v2, like AzerothCore.
