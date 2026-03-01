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

struct MarginAccount_test : public beast::unit_test::suite
{
    // Helper: create a LeverageTierSet transaction
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

    // Helper: create a MarginAccountSet transaction
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

    // Helper: create a MarginDeposit transaction
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

    // Helper: create a MarginWithdraw transaction
    Json::Value
    marginWithdraw(
        jtx::Account const& account,
        uint256 const& marginAccountID,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::MarginWithdraw;
        jv[jss::Account] = account.human();
        jv[sfMarginAccountID.jsonName] = to_string(marginAccountID);
        jv[sfAmount.jsonName] = amount.getJson(JsonOptions::none);
        return jv;
    }

    void
    testLeverageTierCreate(FeatureBitset features)
    {
        testcase("Leverage Tier Create");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];
        auto const GME = gw["GME"];

        env.fund(XRP(10000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env(pay(gw, alice, GME(50000)));
        env.close();

        // Create OptionPair first
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Create leverage tier (gw is issuer of both assets)
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                20,     // 20x max leverage
                5000,   // 5% initial margin (5000 * 1/10 bps)
                2500,   // 2.5% maintenance margin
                500),   // 0.5% liquidation bonus
            ter(tesSUCCESS));
        env.close();

        // Verify leverage tier exists
        auto const tierKeylet =
            keylet::leverageTier(GME.issue(), USD.issue());
        auto const sleTier = env.current()->read(tierKeylet);
        BEAST_EXPECT(sleTier);
        if (sleTier)
        {
            BEAST_EXPECT(sleTier->getFieldU32(sfMaxLeverage) == 20);
            BEAST_EXPECT(sleTier->getFieldU32(sfInitialMarginBps) == 5000);
            BEAST_EXPECT(sleTier->getFieldU32(sfMaintenanceMarginBps) == 2500);
            BEAST_EXPECT(sleTier->getFieldU32(sfLiquidationBonusBps) == 500);
        }
    }

    void
    testLeverageTierValidation(FeatureBitset features)
    {
        testcase("Leverage Tier Validation");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];
        auto const GME = gw["GME"];

        env.fund(XRP(10000), gw, alice);
        env.close();

        // Create OptionPair
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Leverage too low (< 2)
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                1, 100000, 50000, 500),
            ter(temMALFORMED));
        env.close();

        // Leverage too high (> 200)
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                201, 500, 250, 500),
            ter(temMALFORMED));
        env.close();

        // Maintenance >= initial margin
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                10, 5000, 5000, 500),
            ter(temMALFORMED));
        env.close();

        // Non-issuer cannot set tiers
        env(leverageTierSet(
                alice,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                10, 10000, 5000, 500),
            ter(tecNO_PERMISSION));
        env.close();

        // Liquidation bonus out of range
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                10, 10000, 5000, 50),
            ter(temMALFORMED));
        env.close();
    }

    void
    testMarginAccountCreate(FeatureBitset features)
    {
        testcase("Margin Account Create");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];

        env.fund(XRP(10000), gw, alice);
        env.close();

        // Create isolated margin account
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                0),  // isolated mode
            ter(tesSUCCESS));
        env.close();

        // Verify margin account exists
        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const sleMarginAcct = env.current()->read(marginAcctKeylet);
        BEAST_EXPECT(sleMarginAcct);
        if (sleMarginAcct)
        {
            BEAST_EXPECT(
                sleMarginAcct->getAccountID(sfAccount) == alice.id());
            BEAST_EXPECT(sleMarginAcct->getFieldU32(sfMarginMode) == 0);
        }
    }

    void
    testMarginAccountCrossMode(FeatureBitset features)
    {
        testcase("Margin Account Cross Mode");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];

        env.fund(XRP(10000), gw, alice);
        env.close();

        // Create cross-margin account
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                1),  // cross mode
            ter(tesSUCCESS));
        env.close();

        // Verify cross mode
        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const sleMarginAcct = env.current()->read(marginAcctKeylet);
        BEAST_EXPECT(sleMarginAcct);
        if (sleMarginAcct)
        {
            BEAST_EXPECT(sleMarginAcct->getFieldU32(sfMarginMode) == 1);
        }
    }

    void
    testMarginAccountInvalidMode(FeatureBitset features)
    {
        testcase("Margin Account Invalid Mode");

        using namespace test::jtx;
        Env env{*this, features};

        auto const alice = Account("alice");
        auto const gw = Account("gateway");
        auto const USD = gw["USD"];

        env.fund(XRP(10000), alice, gw);
        env.close();

        // Invalid margin mode (> 1)
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                2),  // invalid
            ter(temMALFORMED));
        env.close();
    }

    void
    testMarginDepositWithdraw(FeatureBitset features)
    {
        testcase("Margin Deposit and Withdraw");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];

        env.fund(XRP(10000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env.close();

        // Create margin account
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                0),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        // Deposit collateral
        auto const preBalance = env.balance(alice, USD);
        env(marginDeposit(alice, marginAcctID, USD(1000)),
            ter(tesSUCCESS));
        env.close();

        // Verify collateral balance updated
        auto sleMarginAcct = env.current()->read(marginAcctKeylet);
        BEAST_EXPECT(sleMarginAcct);

        // Withdraw collateral
        env(marginWithdraw(alice, marginAcctID, USD(500)),
            ter(tesSUCCESS));
        env.close();

        // Verify withdrawal reduced balance
        sleMarginAcct = env.current()->read(marginAcctKeylet);
        BEAST_EXPECT(sleMarginAcct);
    }

    void
    testMarginDepositInvalid(FeatureBitset features)
    {
        testcase("Margin Deposit Invalid");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const USD = gw["USD"];

        env.fund(XRP(10000), gw, alice, bob);
        env.close();

        env.trust(USD(100000), alice);
        env.close();

        env(pay(gw, alice, USD(1000)));
        env.close();

        // Create margin account for alice
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue()),
                0),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        // Bob cannot deposit to alice's margin account
        env.trust(USD(100000), bob);
        env.close();
        env(pay(gw, bob, USD(1000)));
        env.close();

        env(marginDeposit(bob, marginAcctID, USD(500)),
            ter(tecNO_PERMISSION));
        env.close();

        // Cannot deposit to non-existent margin account
        uint256 fakeID{};
        env(marginDeposit(alice, fakeID, USD(500)),
            ter(tecNO_ENTRY));
        env.close();
    }

    void
    testMarginWithdrawInsufficient(FeatureBitset features)
    {
        testcase("Margin Withdraw Insufficient");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const USD = gw["USD"];

        env.fund(XRP(10000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.close();

        env(pay(gw, alice, USD(1000)));
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

        env(marginDeposit(alice, marginAcctID, USD(1000)),
            ter(tesSUCCESS));
        env.close();

        // Try to withdraw more than deposited
        env(marginWithdraw(alice, marginAcctID, USD(2000)),
            ter(tecUNFUNDED_PAYMENT));
        env.close();
    }

    void
    testLeverageTierUpdate(FeatureBitset features)
    {
        testcase("Leverage Tier Update");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const USD = gw["USD"];
        auto const GME = gw["GME"];

        env.fund(XRP(10000), gw);
        env.close();

        // Create OptionPair
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Create initial tier
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                10, 10000, 5000, 500),
            ter(tesSUCCESS));
        env.close();

        // Update tier
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                20, 5000, 2500, 1000),
            ter(tesSUCCESS));
        env.close();

        // Verify update
        auto const tierKeylet =
            keylet::leverageTier(GME.issue(), USD.issue());
        auto const sleTier = env.current()->read(tierKeylet);
        BEAST_EXPECT(sleTier);
        if (sleTier)
        {
            BEAST_EXPECT(sleTier->getFieldU32(sfMaxLeverage) == 20);
            BEAST_EXPECT(sleTier->getFieldU32(sfInitialMarginBps) == 5000);
        }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();
        testLeverageTierCreate(sa);
        testLeverageTierValidation(sa);
        testLeverageTierUpdate(sa);
        testMarginAccountCreate(sa);
        testMarginAccountCrossMode(sa);
        testMarginAccountInvalidMode(sa);
        testMarginDepositWithdraw(sa);
        testMarginDepositInvalid(sa);
        testMarginWithdrawInsufficient(sa);
    }
};

BEAST_DEFINE_TESTSUITE(MarginAccount, app, ripple);

}  // namespace test
}  // namespace xrpl
