//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>

#include <xrpl/ledger/Dir.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {
namespace test {

struct OptionLiquidate_test : public beast::unit_test::suite
{
    // Helper: create an OptionPairCreate transaction
    Json::Value
    optionPairCreate(
        jtx::Account const& account,
        STIssue const& asset,
        STIssue const& asset2)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionPairCreate;
        jv[jss::Account] = account.human();
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfAsset2.jsonName] = asset2.getJson(JsonOptions::none);
        return jv;
    }

    Json::Value
    leverageTierSet(
        jtx::Account const& account,
        STIssue const& asset,
        STIssue const& asset2,
        std::uint32_t maxLeverage,
        std::uint32_t initialMarginBps,
        std::uint32_t maintenanceMarginBps,
        std::uint32_t liquidationBonusBps)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::LeverageTierSet;
        jv[jss::Account] = account.human();
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfAsset2.jsonName] = asset2.getJson(JsonOptions::none);
        jv[sfMaxLeverage.jsonName] = maxLeverage;
        jv[sfInitialMarginBps.jsonName] = initialMarginBps;
        jv[sfMaintenanceMarginBps.jsonName] = maintenanceMarginBps;
        jv[sfLiquidationBonusBps.jsonName] = liquidationBonusBps;
        return jv;
    }

    Json::Value
    marginAccountSet(
        jtx::Account const& account,
        STIssue const& collateralAsset,
        std::uint32_t marginMode)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::MarginAccountSet;
        jv[jss::Account] = account.human();
        jv[sfCollateralAsset.jsonName] =
            collateralAsset.getJson(JsonOptions::none);
        jv[sfMarginMode.jsonName] = marginMode;
        return jv;
    }

    Json::Value
    marginDeposit(
        jtx::Account const& account,
        uint256 const& marginAccountID,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::MarginDeposit;
        jv[jss::Account] = account.human();
        jv[sfMarginAccountID.jsonName] = to_string(marginAccountID);
        jv[sfAmount.jsonName] = amount.getJson(JsonOptions::none);
        return jv;
    }

    Json::Value
    optionCreate(
        jtx::Account const& account,
        NetClock::time_point expiration,
        STAmount const& strikePrice,
        STIssue const& asset,
        std::uint32_t quantity,
        STAmount const& premium)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionCreate;
        jv[jss::Account] = account.human();
        jv[sfStrikePrice.jsonName] = strikePrice.getJson(JsonOptions::none);
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfExpiration.jsonName] = expiration.time_since_epoch().count();
        jv[sfPremium.jsonName] = premium.getJson(JsonOptions::none);
        jv[sfQuantity.jsonName] = quantity;
        return jv;
    }

    Json::Value
    optionCreateWithMargin(
        jtx::Account const& account,
        NetClock::time_point expiration,
        STAmount const& strikePrice,
        STIssue const& asset,
        std::uint32_t quantity,
        STAmount const& premium,
        uint256 const& marginAccountID,
        std::uint32_t leverage)
    {
        Json::Value jv = optionCreate(
            account, expiration, strikePrice, asset, quantity, premium);
        jv[sfMarginAccountID.jsonName] = to_string(marginAccountID);
        jv[sfLeverage.jsonName] = leverage;
        return jv;
    }

    Json::Value
    optionLiquidate(
        jtx::Account const& account,
        uint256 const& marginPositionID)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionLiquidate;
        jv[jss::Account] = account.human();
        jv[sfMarginPositionID.jsonName] = to_string(marginPositionID);
        return jv;
    }

    void
    testLiquidateHealthyPosition(FeatureBitset features)
    {
        testcase("Liquidate Healthy Position - Should Fail");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice, bob);
        env.close();

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env(pay(gw, alice, GME(50000)));
        env.close();

        // Create OptionPair
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Create leverage tier (5x max)
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                5, 20000, 10000, 500),
            ter(tesSUCCESS));
        env.close();

        // Create margin account and deposit
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                0),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        env(marginDeposit(alice, marginAcctID, USD(10000)),
            ter(tesSUCCESS));
        env.close();

        // Create a leveraged position
        auto const expiration = env.now() + 3600s;
        env(optionCreateWithMargin(
                alice,
                expiration,
                USD(20),
                STIssue(sfAsset, GME.issue()),
                100,
                USD(1),
                marginAcctID,
                5),
            txflags(tfSell | tfPut),
            ter(tesSUCCESS));
        env.close();

        // Find the margin position in alice's owner directory
        uint256 marginPositionID;
        {
            Dir const ownerDir(
                *env.current(), keylet::ownerDir(alice.id()));
            for (auto const& sle : ownerDir)
            {
                if (sle->getType() == ltMARGIN_POSITION)
                {
                    marginPositionID = sle->key();
                    break;
                }
            }
        }
        BEAST_EXPECT(marginPositionID != beast::zero);

        // Bob tries to liquidate alice's healthy position → should fail
        env(optionLiquidate(bob, marginPositionID),
            ter(tecCANT_LIQUIDATE));
        env.close();
    }

    void
    testPermissionlessLiquidation(FeatureBitset features)
    {
        testcase("Permissionless Liquidation - Any Account Can Submit");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");

        env.fund(XRP(100000), gw, alice, bob, carol);
        env.close();

        // Verify that multiple accounts can attempt liquidation
        // (the actual liquidation will fail because the position is healthy,
        // but the important thing is no permission error)

        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env(pay(gw, alice, GME(50000)));
        env.close();

        // Setup: OptionPair + LeverageTier + MarginAccount + Position
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                5, 20000, 10000, 500),
            ter(tesSUCCESS));
        env.close();

        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                0),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        env(marginDeposit(alice, marginAcctID, USD(10000)),
            ter(tesSUCCESS));
        env.close();

        auto const expiration = env.now() + 3600s;
        env(optionCreateWithMargin(
                alice,
                expiration,
                USD(20),
                STIssue(sfAsset, GME.issue()),
                100,
                USD(1),
                marginAcctID,
                5),
            txflags(tfSell | tfPut),
            ter(tesSUCCESS));
        env.close();

        // Find the margin position
        uint256 marginPositionID;
        {
            Dir const ownerDir(
                *env.current(), keylet::ownerDir(alice.id()));
            for (auto const& sle : ownerDir)
            {
                if (sle->getType() == ltMARGIN_POSITION)
                {
                    marginPositionID = sle->key();
                    break;
                }
            }
        }
        BEAST_EXPECT(marginPositionID != beast::zero);

        // Bob attempts liquidation → tecCANT_LIQUIDATE (healthy)
        env(optionLiquidate(bob, marginPositionID),
            ter(tecCANT_LIQUIDATE));
        env.close();

        // Carol also attempts → same result (permissionless)
        env(optionLiquidate(carol, marginPositionID),
            ter(tecCANT_LIQUIDATE));
        env.close();
    }

    void
    testLiquidateNonexistentPosition(FeatureBitset features)
    {
        testcase("Liquidate Nonexistent Position");

        using namespace test::jtx;
        Env env{*this, features};

        auto const alice = Account("alice");
        env.fund(XRP(100000), alice);
        env.close();

        // Try to liquidate a position that doesn't exist
        uint256 const fakeID(1234);
        env(optionLiquidate(alice, fakeID),
            ter(tecNO_ENTRY));
        env.close();
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{testable_amendments()};

        testLiquidateHealthyPosition(all);
        testPermissionlessLiquidation(all);
        testLiquidateNonexistentPosition(all);
    }
};

BEAST_DEFINE_TESTSUITE(OptionLiquidate, app, ripple);

}  // namespace test
}  // namespace xrpl
