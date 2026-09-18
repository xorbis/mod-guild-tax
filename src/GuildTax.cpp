/*
 * This file is part of mod-guild-tax, a module for AzerothCore, released under the
 * GNU GPL v2 license: https://github.com/xorbis/mod-guild-tax/blob/main/LICENSE
 */

// A share of the money guild members loot and earn from quests goes to their guild bank.
// The rate is a "[tax: N%]" tag in the guild MOTD, so the guild's own officers set it and
// every member sees it at login. The deposits go through the same path as the bank window
// (Guild::HandleMemberDepositMoney), so they show up in the bank's money log.

#include "Chat.h"
#include "ConfigValueCache.h"
#include "Creature.h"
#include "Group.h"
#include "Guild.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

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
    struct Ledger : public DataMap::Base
    {
        uint32 guildId = 0;
        uint32 owed = 0;    // copper
    };

    constexpr char LEDGER_KEY[] = "mod-guild-tax";

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
    void Deposit(Player* player, Guild* guild, Ledger& ledger)
    {
        uint32 amount = std::min(ledger.owed, player->GetMoney());
        if (!amount || guild->GetTotalBankMoney() + amount > GUILD_BANK_MONEY_LIMIT)
            return;

        guild->HandleMemberDepositMoney(player->GetSession(), amount);
        ledger.owed -= amount;

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

        Ledger* ledger = player->CustomData.GetDefault<Ledger>(LEDGER_KEY);
        if (ledger->guildId != guild->GetId())
        {
            // another guild than the one the rest was accrued for (see OnRemoveMember)
            ledger->guildId = guild->GetId();
            ledger->owed = 0;
        }
        ledger->owed += tax;

        if (ledger->owed >= settings.GetConfigValue<uint32>(GuildTaxConfig::DEPOSIT_THRESHOLD))
            Deposit(player, guild, *ledger);
    }

    // Deposits whatever is owed, as long as the character is still in the guild it is owed to.
    void Settle(Player* player)
    {
        Ledger* ledger = player->CustomData.Get<Ledger>(LEDGER_KEY);
        if (!ledger || !ledger->owed)
            return;

        Guild* guild = player->GetGuild();
        if (guild && guild->GetId() == ledger->guildId)
            Deposit(player, guild, *ledger);
        else
            ledger->owed = 0;
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
        GuildTaxPlayer() : PlayerScript("GuildTaxPlayer", { PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_SAVE }) { }

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

            if (Ledger* ledger = player->CustomData.Get<Ledger>(LEDGER_KEY))
            {
                if (!isDisbanding && ledger->guildId == guild->GetId())
                    Deposit(player, guild, *ledger);
                ledger->owed = 0;
            }
        }
    };
}

void AddGuildTaxScripts()
{
    new GuildTaxWorld();
    new GuildTaxLoot();
    new GuildTaxPlayer();
    new GuildTaxGuild();
}
