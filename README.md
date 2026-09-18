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

## Bots

With [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) installed, bots do not pay the tax unless `GuildTax.Bots = 1`; a bot in a player's guild would otherwise feed its bank a steady stream of gold.

## Installation

```
cd modules
git clone https://github.com/xorbis/mod-guild-tax.git
```

Re-run CMake and build. The config template `conf/mod_guild_tax.conf.dist` is installed next to the worldserver config; copy it to `mod_guild_tax.conf` to change the defaults. `.reload config` picks up changes without a restart.

## License

GNU GPL v2, like AzerothCore.
