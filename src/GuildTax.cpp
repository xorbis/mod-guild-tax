/*
 * This file is part of mod-guild-tax, a module for AzerothCore, released under the
 * GNU GPL v2 license: https://github.com/xorbis/mod-guild-tax/blob/main/LICENSE
 */

// A share of the money guild members loot and earn from quests goes to their guild bank.
// The rate is a "[tax: N%]" tag in the guild MOTD, so the guild's own officers set it and
// every member sees it at login. The deposits go through the same path as the bank window
// (Guild::HandleMemberDepositMoney), so they show up in the bank's money log.
//
// That log only keeps the last few entries, so the module also keeps a running total per member
// of what went through the bank (guild_member_ledger: deposits, withdrawals, tax) and shows it
// with ".guild ledger": every member with their rank, what they deposited, withdrew and paid in
// tax, and the balance of the three.

#include "Chat.h"
#include "CommandScript.h"
#include "ConfigValueCache.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    enum class GuildTaxConfig
    {
        ENABLE,
        DEFAULT_PERCENT,
        MAX_PERCENT,
        DEPOSIT_THRESHOLD,
        BOTS,
        ANNOUNCE,

        NUM_CONFIGS
    };

    class GuildTaxConfigData : public ConfigValueCache<GuildTaxConfig>
    {
    public:
        GuildTaxConfigData() : ConfigValueCache(GuildTaxConfig::NUM_CONFIGS) { }

        void BuildConfigCache() override
        {
            SetConfigValue<bool>(GuildTaxConfig::ENABLE, "GuildTax.Enable", true);
            SetConfigValue<uint32>(GuildTaxConfig::DEFAULT_PERCENT, "GuildTax.DefaultPercent", 0);
            SetConfigValue<uint32>(GuildTaxConfig::MAX_PERCENT, "GuildTax.MaxPercent", 50,
                Reloadable::Yes, [](uint32 const& value) { return value <= 100; }, "0-100");
            SetConfigValue<uint32>(GuildTaxConfig::DEPOSIT_THRESHOLD, "GuildTax.DepositThreshold", uint32(GOLD));
            SetConfigValue<bool>(GuildTaxConfig::BOTS, "GuildTax.Bots", false);
            SetConfigValue<bool>(GuildTaxConfig::ANNOUNCE, "GuildTax.Announce", true);
        }
    };

    GuildTaxConfigData settings;

    // Tax a character owes its guild, added up until it is worth a deposit (GuildTax.DepositThreshold).
    struct Tab : public DataMap::Base
    {
        uint32 guildId = 0;
        uint32 owed = 0;    // copper
    };

    constexpr char TAB_KEY[] = "mod-guild-tax";

    // Set while the module deposits, so the bank event it raises is booked as tax and not as a
    // deposit the member made (see GuildTaxLedger). Per thread: the bank window's deposits are
    // handled on the world thread, the module's on the map threads.
    thread_local bool depositingTax = false;

    // The guild's rate: the "[tax: N%]" tag in its MOTD if there is one (also [tax:10], [Tax=5%]),
    // else the realm default; never above the realm cap.
    uint32 TaxPercent(Guild const* guild)
    {
        static std::regex const tag(R"(\[\s*tax\s*[:=]?\s*(\d{1,3})\s*%?\s*\])", std::regex::icase | std::regex::optimize);

        uint32 percent = settings.GetConfigValue<uint32>(GuildTaxConfig::DEFAULT_PERCENT);
        std::smatch match;
        if (std::regex_search(guild->GetMOTD(), match, tag))
            percent = std::stoul(match[1].str());

        return std::min(percent, settings.GetConfigValue<uint32>(GuildTaxConfig::MAX_PERCENT));
    }

    std::string MoneyToString(uint32 copper)
    {
        std::string out;
        if (uint32 gold = copper / GOLD)
            out += Acore::StringFormat("{} gold", gold);
        if (uint32 silver = (copper % GOLD) / SILVER)
            out += Acore::StringFormat("{}{} silver", out.empty() ? "" : " ", silver);
        if (uint32 rest = copper % SILVER; rest || out.empty())
            out += Acore::StringFormat("{}{} copper", out.empty() ? "" : " ", rest);
        return out;
    }

    // Moves what the character owes into the guild bank. The character may have spent part of it
    // since it was accrued; what cannot be paid stays owed.
    //
    // Quest turn-ins and autosaves run on the map update threads, so two members on different maps
    // can get here at once; the core only touches the bank money and its log from the world thread,
    // which never runs alongside a map update, so one lock over the module's own deposits is enough.
    void Deposit(Player* player, Guild* guild, Tab& tab)
    {
        static std::mutex depositLock;
        std::lock_guard<std::mutex> lock(depositLock);

        uint32 amount = std::min(tab.owed, player->GetMoney());
        if (!amount || guild->GetTotalBankMoney() + amount > GUILD_BANK_MONEY_LIMIT)
            return;

        depositingTax = true;
        guild->HandleMemberDepositMoney(player->GetSession(), amount);
        depositingTax = false;
        tab.owed -= amount;

        if (settings.GetConfigValue<bool>(GuildTaxConfig::ANNOUNCE))
            ChatHandler(player->GetSession()).PSendSysMessage("Guild tax: {} deposited to the guild bank.", MoneyToString(amount));
    }

    // Puts the tax on money the character just received on its tab.
    void Accrue(Player* player, uint32 income)
    {
        if (!income || !settings.GetConfigValue<bool>(GuildTaxConfig::ENABLE))
            return;

#ifdef MOD_PLAYERBOTS
        if (!settings.GetConfigValue<bool>(GuildTaxConfig::BOTS) && player->GetSession()->IsBot())
            return;
#endif

        Guild* guild = player->GetGuild();
        if (!guild)
            return;

        uint32 tax = uint64(income) * TaxPercent(guild) / 100;
        if (!tax)
            return;

        Tab* tab = player->CustomData.GetDefault<Tab>(TAB_KEY);
        if (tab->guildId != guild->GetId())
        {
            // another guild than the one the rest was accrued for (see OnRemoveMember)
            tab->guildId = guild->GetId();
            tab->owed = 0;
        }
        tab->owed += tax;

        if (tab->owed >= settings.GetConfigValue<uint32>(GuildTaxConfig::DEPOSIT_THRESHOLD))
            Deposit(player, guild, *tab);
    }

    // Deposits whatever is owed, as long as the character is still in the guild it is owed to.
    void Settle(Player* player)
    {
        Tab* tab = player->CustomData.Get<Tab>(TAB_KEY);
        if (!tab || !tab->owed)
            return;

        Guild* guild = player->GetGuild();
        if (guild && guild->GetId() == tab->guildId)
            Deposit(player, guild, *tab);
        else
            tab->owed = 0;
    }

    // What the core actually paid out of a share: the loot and quest hooks report the amount that
    // dropped, and the play time restriction (CAIS) may have halved or withheld it.
    uint32 Awarded(Player const* player, uint32 share)
    {
        if (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
            return 0;
        if (player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            return share / 2;
        return share;
    }

    class GuildTaxWorld : public WorldScript
    {
    public:
        GuildTaxWorld() : WorldScript("GuildTaxWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnAfterConfigLoad(bool reload) override
        {
            settings.Initialize(reload);
        }
    };

    class GuildTaxLoot : public LootScript
    {
    public:
        GuildTaxLoot() : LootScript("GuildTaxLoot", { LOOTHOOK_ON_LOOT_MONEY }) { }

        // Fired once per money loot, after the shares are paid, with the amount that dropped. Who got
        // what is worked out the way WorldSession::HandleLootMoneyOpcode did: a group splits creature
        // and chest money between the members in loot range; pickpocketed money, lockboxes and
        // battleground insignias go to the looter alone.
        void OnLootMoney(Player* looter, uint32 gold) override
        {
            ObjectGuid lootGuid = looter->GetLootGUID();
            bool shared = !lootGuid.IsItem() && !lootGuid.IsCorpse();
            if (lootGuid.IsCreatureOrVehicle())
                if (Creature* creature = looter->GetMap()->GetCreature(lootGuid))
                    shared = !creature->IsAlive();   // alive: pickpocketing

            Group* group = shared ? looter->GetGroup() : nullptr;
            if (!group)
            {
                Accrue(looter, Awarded(looter, gold));
                return;
            }

            std::vector<Player*> membersInRange;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (looter->IsAtLootRewardDistance(member))
                        membersInRange.push_back(member);

            if (membersInRange.empty())
                return;

            uint32 share = gold / membersInRange.size();
            for (Player* member : membersInRange)
                Accrue(member, Awarded(member, share));
        }
    };

    class GuildTaxPlayer : public PlayerScript
    {
    public:
        GuildTaxPlayer() : PlayerScript("GuildTaxPlayer", { PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_SAVE, PLAYERHOOK_ON_DELETE }) { }

        // Fired at the end of Player::RewardQuest; the money is worked out the way it was there: the
        // level-scaled reward, plus the experience turned into money at the level cap.
        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            int32 money = 0;
            if (player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) || sScriptMgr->OnPlayerShouldBeRewardedWithMoneyInsteadOfExp(player))
                money = quest->GetRewMoneyMaxLevel();
            money += quest->GetRewOrReqMoney(player->GetLevel());

            if (money > 0)
                Accrue(player, Awarded(player, uint32(money)));
        }

        // Autosave and logout, before the character row is written: the deposit and the gold it
        // came out of reach the database together.
        void OnPlayerSave(Player* player) override
        {
            Settle(player);
        }

        // A deleted character's ledger rows have nothing left to show up under.
        void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
        {
            CharacterDatabase.Execute("DELETE FROM guild_member_ledger WHERE guid = {}", guid.GetCounter());
        }
    };

    class GuildTaxGuild : public GuildScript
    {
    public:
        GuildTaxGuild() : GuildScript("GuildTaxGuild", { GUILDHOOK_ON_REMOVE_MEMBER }) { }

        // Leaving or kicked, fired while the character is still a member: what was accrued as a
        // member still goes in. A disbanding guild loses its bank, so nothing is deposited there.
        // An offline member settled at its logout save.
        void OnRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool /*isKicked*/) override
        {
            if (!player)
                return;

            if (Tab* tab = player->CustomData.Get<Tab>(TAB_KEY))
            {
                if (!isDisbanding && tab->guildId == guild->GetId())
                    Deposit(player, guild, *tab);
                tab->owed = 0;
            }
        }
    };

    // Running total per guild and member of the money that went through the bank
    // (guild_member_ledger, in copper): what the member deposited, withdrew (repairs included) and
    // paid in tax. The bank's own money log is capped (Guild.BankEventLogRecordsCount, 25 by
    // default), this is not. Rows outlive a membership: a member who leaves and comes back finds
    // their numbers again; a disbanded guild's rows go with it.

    // The ledger's column a money log event goes to, or nullptr for the item events. The log does
    // not say which deposits were the module's; the seed below asks for a guess (isTax), the live
    // booking knows (depositingTax).
    char const* LedgerColumn(uint8 eventType, bool isTax)
    {
        switch (eventType)
        {
            case GUILD_BANK_LOG_DEPOSIT_MONEY:  return isTax ? "tax" : "deposited";
            case GUILD_BANK_LOG_WITHDRAW_MONEY:
            case GUILD_BANK_LOG_REPAIR_MONEY:   return "withdrawn";
            default:                            return nullptr;
        }
    }

    // Adds to one column of a member's row, creating the row on first use. Runs on whichever thread
    // moved the money; the database queue is the only shared state.
    void Book(uint32 guildId, ObjectGuid::LowType guid, char const* column, uint32 amount)
    {
        CharacterDatabase.Execute("INSERT INTO guild_member_ledger (guildid, guid, {0}) VALUES ({1}, {2}, {3}) ON DUPLICATE KEY UPDATE {0} = {0} + {3}",
            column, guildId, guid, amount);
    }

    // Booked from the bank event the core logs once the money has actually moved, so a refused
    // deposit or withdrawal never shows.
    class GuildTaxLedger : public GuildScript
    {
    public:
        GuildTaxLedger() : GuildScript("GuildTaxLedger", { GUILDHOOK_ON_BANK_EVENT, GUILDHOOK_ON_DISBAND }) { }

        void OnBankEvent(Guild* guild, uint8 eventType, uint8 /*tabId*/, ObjectGuid::LowType playerGuid, uint32 itemOrMoney, uint16 /*itemStackCount*/, uint8 /*destTabId*/) override
        {
            if (char const* column = LedgerColumn(eventType, depositingTax))
                Book(guild->GetId(), playerGuid, column, itemOrMoney);
        }

        // Guild ids are reused, so a disbanded guild's rows must not wait for the next one.
        void OnDisband(Guild* guild) override
        {
            CharacterDatabase.Execute("DELETE FROM guild_member_ledger WHERE guildid = {}", guild->GetId());
        }
    };

    // The first time the module starts with an empty ledger, it is built from what is still in the
    // banks' money logs (the last Guild.BankEventLogRecordsCount entries of each guild), so a guild
    // that predates the module does not start from zero. The log does not say which deposits were
    // the module's: in a guild with a tax rate, a deposit with a silver or copper part is taken to
    // be tax (the module deposits the tax accrued so far, an odd amount) and a whole number of gold
    // the member's own (what one types into the bank window); without a rate, all are the member's.
    class GuildTaxLedgerSeed : public WorldScript
    {
    public:
        GuildTaxLedgerSeed() : WorldScript("GuildTaxLedgerSeed", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            if (CharacterDatabase.Query("SELECT 1 FROM guild_member_ledger LIMIT 1"))
                return;

            QueryResult result = CharacterDatabase.Query("SELECT guildid, PlayerGuid, EventType, ItemOrMoney FROM guild_bank_eventlog WHERE TabId = {}", uint32(GUILD_BANK_MONEY_LOGS_TAB));
            if (!result)
                return;

            bool const taxed = settings.GetConfigValue<bool>(GuildTaxConfig::ENABLE);
            uint32 entries = 0;
            do
            {
                Field* fields = result->Fetch();
                uint32 guildId = fields[0].Get<uint32>();
                ObjectGuid::LowType guid = fields[1].Get<uint32>();
                uint8 eventType = fields[2].Get<uint8>();
                uint32 amount = fields[3].Get<uint32>();

                Guild const* guild = sGuildMgr->GetGuildById(guildId);
                if (!guild)
                    continue;

                bool isTax = taxed && TaxPercent(guild) && amount % GOLD;
                if (char const* column = LedgerColumn(eventType, isTax))
                {
                    Book(guildId, guid, column, amount);
                    ++entries;
                }
            } while (result->NextRow());

            LOG_INFO("module", "mod-guild-tax: guild ledger built from {} guild bank money log entries", entries);
        }
    };

    // "12g 34s 56c" in the client's coin colours, zero parts left out; "-" in front of a negative.
    std::string Coins(int64 copper)
    {
        uint64 const total = copper < 0 ? -copper : copper;
        uint64 const gold = total / GOLD, silver = total % GOLD / SILVER, rest = total % SILVER;

        std::string out = copper < 0 ? "-" : "";
        if (gold)
            out += Acore::StringFormat("{}|cffffd700g|r", gold);
        if (silver)
            out += Acore::StringFormat("{}{}|cffc7c7cfs|r", gold ? " " : "", silver);
        if (rest || !total)
            out += Acore::StringFormat("{}{}|cffeda55fc|r", gold || silver ? " " : "", rest);
        return out;
    }

    // The client's class colours (the ones the guild roster and group frames use).
    char const* ClassColor(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:      return "|cffC79C6E";
            case CLASS_PALADIN:      return "|cffF58CBA";
            case CLASS_HUNTER:       return "|cffABD473";
            case CLASS_ROGUE:        return "|cffFFF569";
            case CLASS_PRIEST:       return "|cffFFFFFF";
            case CLASS_DEATH_KNIGHT: return "|cffC41F3B";
            case CLASS_SHAMAN:       return "|cff0070DE";
            case CLASS_MAGE:         return "|cff69CCF0";
            case CLASS_WARLOCK:      return "|cff9482C9";
            case CLASS_DRUID:        return "|cffFF7D0A";
            default:                 return "|cffFFFFFF";
        }
    }

    // ".guild ledger": the guild's members with their rank and what each deposited, withdrew and paid
    // in tax, and the balance of the three (deposited + tax - withdrawn). Every member can run it,
    // as every member can read the bank's money log. The XorWoW client addon sends it for /guildinfo.
    class GuildTaxCommands : public CommandScript
    {
    public:
        GuildTaxCommands() : CommandScript("GuildTaxCommands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable guildCommandTable =
            {
                { "ledger", HandleLedgerCommand, SEC_PLAYER, Console::No }
            };
            static ChatCommandTable commandTable =
            {
                { "guild", guildCommandTable }   // merged into the core's ".guild" tree
            };
            return commandTable;
        }

        static bool HandleLedgerCommand(ChatHandler* handler)
        {
            Player* player = handler->GetPlayer();
            if (!player)
                return false;

            Guild* guild = player->GetGuild();
            if (!guild)
            {
                handler->SendSysMessage("You are not in a guild.");
                return true;
            }

            // The member list and rank names are the guild's tables (kept in step with the guild in
            // memory); one query joins the ledger to them. Offline members included, guild master first.
            QueryResult result = CharacterDatabase.Query(
                "SELECT c.name, c.class, r.rname, l.deposited, l.withdrawn, l.tax "
                "FROM guild_member m "
                "JOIN characters c ON c.guid = m.guid "
                "LEFT JOIN guild_rank r ON r.guildid = m.guildid AND r.rid = m.`rank` "
                "LEFT JOIN guild_member_ledger l ON l.guildid = m.guildid AND l.guid = m.guid "
                "WHERE m.guildid = {} ORDER BY m.`rank`, c.name", guild->GetId());

            std::string header = Acore::StringFormat("{}: {} members, guild bank {}", guild->GetName(), guild->GetMemberCount(), Coins(guild->GetTotalBankMoney()));
            if (settings.GetConfigValue<bool>(GuildTaxConfig::ENABLE))
                if (uint32 percent = TaxPercent(guild))
                    header += Acore::StringFormat(", tax {}%", percent);
            handler->SendSysMessage(header);

            if (!result)
                return true;

            do
            {
                Field* fields = result->Fetch();
                std::string name = fields[0].Get<std::string>();
                uint8 classId = fields[1].Get<uint8>();
                std::string rank = fields[2].Get<std::string>();
                uint64 deposited = fields[3].Get<uint64>();   // NULL (no row yet) reads as 0
                uint64 withdrawn = fields[4].Get<uint64>();
                uint64 tax = fields[5].Get<uint64>();

                handler->PSendSysMessage("{}{}|r ({}): deposited {}, withdrawn {}, tax {}, balance {}",
                    ClassColor(classId), name, rank, Coins(deposited), Coins(withdrawn), Coins(tax), Coins(int64(deposited + tax) - int64(withdrawn)));
            } while (result->NextRow());

            return true;
        }
    };
}

void AddGuildTaxScripts()
{
    new GuildTaxWorld();
    new GuildTaxLoot();
    new GuildTaxPlayer();
    new GuildTaxGuild();
    new GuildTaxLedger();
    new GuildTaxLedgerSeed();
    new GuildTaxCommands();
}
