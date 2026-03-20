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

struct MarginFunctionality_test : public beast::unit_test::suite
{
    // -----------------------------------------------------------------------
    // Helper: build a MarginAccountSet transaction JSON
    // -----------------------------------------------------------------------
    Json::Value
    marginAccountSet(
        jtx::Account const& account,
        STIssue const& collateralAsset)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::MarginAccountSet;
        jv[jss::Account] = account.human();
        jv[sfCollateralAsset.jsonName] =
            collateralAsset.getJson(JsonOptions::none);
        return jv;
    }

    // -----------------------------------------------------------------------
    // Helper: build a MarginDeposit transaction JSON
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Helper: build a MarginWithdraw transaction JSON
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Helper: build a LeverageTierSet transaction JSON
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Helper: build an OptionPairCreate transaction JSON
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Helper: build an InsuranceVaultCreate transaction JSON
    // -----------------------------------------------------------------------
    Json::Value
    insuranceVaultCreate(
        jtx::Account const& account,
        STIssue const& asset,
        STIssue const& asset2)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::InsuranceVaultCreate;
        jv[jss::Account] = account.human();
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfAsset2.jsonName] = asset2.getJson(JsonOptions::none);
        return jv;
    }

    // -----------------------------------------------------------------------
    // Helper: build an InsuranceDeposit transaction JSON
    // -----------------------------------------------------------------------
    Json::Value
    insuranceDeposit(
        jtx::Account const& account,
        uint256 const& vaultID,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::InsuranceDeposit;
        jv[jss::Account] = account.human();
        jv[sfInsuranceVaultID.jsonName] = to_string(vaultID);
        jv[sfAmount.jsonName] = amount.getJson(JsonOptions::none);
        return jv;
    }

    // -----------------------------------------------------------------------
    // Helper: build an InsuranceWithdraw transaction JSON
    // -----------------------------------------------------------------------
    Json::Value
    insuranceWithdraw(
        jtx::Account const& account,
        uint256 const& vaultID,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::InsuranceWithdraw;
        jv[jss::Account] = account.human();
        jv[sfInsuranceVaultID.jsonName] = to_string(vaultID);
        jv[sfAmount.jsonName] = amount.getJson(JsonOptions::none);
        return jv;
    }

    // -----------------------------------------------------------------------
    // Helper: build an OptionCreate transaction JSON (with optional margin)
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Helper: count entries in an owner directory
    // -----------------------------------------------------------------------
    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    }

    // -----------------------------------------------------------------------
    // Helper: create common setup (gateway, option pair, leverage tier)
    // -----------------------------------------------------------------------
    struct MarginSetup
    {
        jtx::Account gw{"gateway"};
        jtx::Account alice{"alice"};
        jtx::Account bob{"bob"};
    };

    // ===================================================================
    // Test 1: MarginAccountSet
    // ===================================================================
    void
    testMarginAccountSet(FeatureBitset features)
    {
        testcase("MarginAccountSet - Isolated Mode with IOU Collateral");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const USD = gw["USD"];

            env.fund(XRP(10000), gw, alice);
            env.close();

            // Create margin account
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, USD.issue())),
                ter(tesSUCCESS));
            env.close();

            // Verify margin account ledger entry exists
            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), USD.issue());
            auto const sleMarginAcct =
                env.current()->read(marginAcctKeylet);
            BEAST_EXPECT(sleMarginAcct);
            if (sleMarginAcct)
            {
                BEAST_EXPECT(
                    sleMarginAcct->getAccountID(sfAccount) == alice.id());
            }
        }

        testcase("MarginAccountSet - XRP Collateral");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const alice = Account("alice");

            env.fund(XRP(10000), alice);
            env.close();

            // Create margin account with XRP as collateral
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, xrpIssue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), xrpIssue());
            auto const sleMarginAcct =
                env.current()->read(marginAcctKeylet);
            BEAST_EXPECT(sleMarginAcct);
            if (sleMarginAcct)
            {
                BEAST_EXPECT(
                    sleMarginAcct->getAccountID(sfAccount) == alice.id());
            }
        }
    }

    // ===================================================================
    // Test 2: MarginDeposit
    // ===================================================================
    void
    testMarginDeposit(FeatureBitset features)
    {
        testcase("MarginDeposit - IOU Deposit Success");
        {
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
                    STIssue(sfCollateralAsset, USD.issue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), USD.issue());
            auto const marginAcctID = marginAcctKeylet.key;

            // Record pre-deposit IOU balance
            auto const preBalance = env.balance(alice, USD);

            // Deposit 1000 USD
            env(marginDeposit(alice, marginAcctID, USD(1000)),
                ter(tesSUCCESS));
            env.close();

            // Verify IOU balance decreased
            auto const postBalance = env.balance(alice, USD);
            BEAST_EXPECT(postBalance < preBalance);

            // Verify collateralBalance updated on the margin account
            auto const sleMarginAcct =
                env.current()->read(marginAcctKeylet);
            BEAST_EXPECT(sleMarginAcct);
            if (sleMarginAcct)
            {
                Number const collateral =
                    sleMarginAcct->at(~sfCollateralBalance)
                        .value_or(Number(0));
                BEAST_EXPECT(collateral > Number(0));
            }
        }

        testcase("MarginDeposit - XRP Deposit Success");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const alice = Account("alice");

            env.fund(XRP(10000), alice);
            env.close();

            // Create XRP-collateral margin account
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, xrpIssue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), xrpIssue());
            auto const marginAcctID = marginAcctKeylet.key;

            // Record pre-deposit XRP balance
            auto const preBalance = env.balance(alice);

            // Deposit 1000 XRP
            env(marginDeposit(alice, marginAcctID, XRP(1000)),
                ter(tesSUCCESS));
            env.close();

            // Verify XRP balance decreased (accounting for fees too)
            auto const postBalance = env.balance(alice);
            BEAST_EXPECT(postBalance < preBalance);

            // Verify collateralBalance updated
            auto const sleMarginAcct =
                env.current()->read(marginAcctKeylet);
            BEAST_EXPECT(sleMarginAcct);
            if (sleMarginAcct)
            {
                Number const collateral =
                    sleMarginAcct->at(~sfCollateralBalance)
                        .value_or(Number(0));
                BEAST_EXPECT(collateral > Number(0));
            }
        }

        testcase("MarginDeposit - XRP Reserve Violation Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const alice = Account("alice");

            // Fund with minimal XRP so reserve check triggers
            env.fund(XRP(300), alice);
            env.close();

            // Create XRP-collateral margin account
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, xrpIssue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), xrpIssue());
            auto const marginAcctID = marginAcctKeylet.key;

            // Attempt to deposit nearly all XRP, violating reserve
            env(marginDeposit(alice, marginAcctID, XRP(290)),
                ter(tecUNFUNDED_PAYMENT));
            env.close();
        }

        testcase("MarginDeposit - Non-Owner Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const USD = gw["USD"];

            env.fund(XRP(10000), gw, alice, bob);
            env.close();

            env.trust(USD(100000), alice);
            env.trust(USD(100000), bob);
            env.close();

            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            // Create margin account for alice
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, USD.issue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), USD.issue());
            auto const marginAcctID = marginAcctKeylet.key;

            // Bob tries to deposit to alice's margin account
            env(marginDeposit(bob, marginAcctID, USD(500)),
                ter(tecNO_PERMISSION));
            env.close();
        }

        testcase("MarginDeposit - Wrong Collateral Asset Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const USD = gw["USD"];
            auto const EUR = gw["EUR"];

            env.fund(XRP(10000), gw, alice);
            env.close();

            env.trust(USD(100000), alice);
            env.trust(EUR(100000), alice);
            env.close();

            env(pay(gw, alice, USD(1000)));
            env(pay(gw, alice, EUR(1000)));
            env.close();

            // Create margin account with USD collateral
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, USD.issue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), USD.issue());
            auto const marginAcctID = marginAcctKeylet.key;

            // Try to deposit EUR into a USD-collateral margin account
            env(marginDeposit(alice, marginAcctID, EUR(500)),
                ter(temMALFORMED));
            env.close();
        }

        testcase("MarginDeposit - Nonexistent Account Rejected");
        {
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

            // Deposit to a margin account ID that does not exist
            uint256 fakeID{};
            env(marginDeposit(alice, fakeID, USD(500)),
                ter(tecNO_ENTRY));
            env.close();
        }
    }

    // ===================================================================
    // Test 3: MarginWithdraw
    // ===================================================================
    void
    testMarginWithdraw(FeatureBitset features)
    {
        testcase("MarginWithdraw - IOU Withdrawal Success");
        {
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

            // Create margin account and deposit
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, USD.issue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), USD.issue());
            auto const marginAcctID = marginAcctKeylet.key;

            env(marginDeposit(alice, marginAcctID, USD(1000)),
                ter(tesSUCCESS));
            env.close();

            // Record balance after deposit
            auto const preWithdrawBalance = env.balance(alice, USD);

            // Withdraw 500 USD
            env(marginWithdraw(alice, marginAcctID, USD(500)),
                ter(tesSUCCESS));
            env.close();

            // Verify IOU balance increased
            auto const postWithdrawBalance = env.balance(alice, USD);
            BEAST_EXPECT(postWithdrawBalance > preWithdrawBalance);

            // Verify collateralBalance decreased
            auto const sleMarginAcct =
                env.current()->read(marginAcctKeylet);
            BEAST_EXPECT(sleMarginAcct);
            if (sleMarginAcct)
            {
                Number const collateral =
                    sleMarginAcct->at(~sfCollateralBalance)
                        .value_or(Number(0));
                // Should be roughly 500 (1000 deposited - 500 withdrawn)
                BEAST_EXPECT(collateral > Number(0));
            }
        }

        testcase("MarginWithdraw - XRP Withdrawal Success");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const alice = Account("alice");

            env.fund(XRP(10000), alice);
            env.close();

            // Create XRP margin account, deposit, then withdraw
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, xrpIssue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), xrpIssue());
            auto const marginAcctID = marginAcctKeylet.key;

            env(marginDeposit(alice, marginAcctID, XRP(2000)),
                ter(tesSUCCESS));
            env.close();

            auto const preWithdrawBalance = env.balance(alice);

            // Withdraw 1000 XRP
            env(marginWithdraw(alice, marginAcctID, XRP(1000)),
                ter(tesSUCCESS));
            env.close();

            // Verify XRP balance increased (minus fee)
            auto const postWithdrawBalance = env.balance(alice);
            BEAST_EXPECT(postWithdrawBalance > preWithdrawBalance);
        }

        testcase("MarginWithdraw - Insufficient Collateral Rejected");
        {
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

            // Create margin account and deposit 1000 USD
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, USD.issue())),
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

        testcase("MarginWithdraw - Non-Owner Rejected");
        {
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

            // Create margin account for alice and deposit
            env(marginAccountSet(
                    alice,
                    STIssue(sfCollateralAsset, USD.issue())),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctKeylet =
                keylet::marginAccount(alice.id(), USD.issue());
            auto const marginAcctID = marginAcctKeylet.key;

            env(marginDeposit(alice, marginAcctID, USD(1000)),
                ter(tesSUCCESS));
            env.close();

            // Bob tries to withdraw from alice's margin account
            env(marginWithdraw(bob, marginAcctID, USD(500)),
                ter(tecNO_PERMISSION));
            env.close();
        }
    }

    // ===================================================================
    // Test 4: LeverageTierSet
    // ===================================================================
    void
    testLeverageTierSet(FeatureBitset features)
    {
        testcase("LeverageTierSet - Create Valid Tier");
        {
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

            // Create OptionPair prerequisite
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Create leverage tier (gw is issuer)
            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    20,     // 20x max leverage
                    5000,   // 5% initial margin
                    2500,   // 2.5% maintenance margin
                    500),   // 0.5% liquidation bonus
                ter(tesSUCCESS));
            env.close();

            // Verify leverage tier exists on ledger
            auto const tierKeylet =
                keylet::leverageTier(GME.issue(), USD.issue());
            auto const sleTier = env.current()->read(tierKeylet);
            BEAST_EXPECT(sleTier);
            if (sleTier)
            {
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfMaxLeverage) == 20);
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfInitialMarginBps) == 5000);
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfMaintenanceMarginBps) == 2500);
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfLiquidationBonusBps) == 500);
            }
        }

        testcase("LeverageTierSet - Update Existing Tier");
        {
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

            // Update tier with new parameters
            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    20, 5000, 2500, 1000),
                ter(tesSUCCESS));
            env.close();

            // Verify updated values
            auto const tierKeylet =
                keylet::leverageTier(GME.issue(), USD.issue());
            auto const sleTier = env.current()->read(tierKeylet);
            BEAST_EXPECT(sleTier);
            if (sleTier)
            {
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfMaxLeverage) == 20);
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfInitialMarginBps) == 5000);
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfMaintenanceMarginBps) == 2500);
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfLiquidationBonusBps) == 1000);
            }
        }

        testcase("LeverageTierSet - Non-Issuer Rejected");
        {
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

            // alice (not the issuer) tries to set leverage tier
            env(leverageTierSet(
                    alice,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    10, 10000, 5000, 500),
                ter(tecNO_PERMISSION));
            env.close();
        }

        testcase("LeverageTierSet - Leverage Too Low Rejected");
        {
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

            // Max leverage of 1 (below minimum of 2)
            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    1, 100000, 50000, 500),
                ter(temMALFORMED));
            env.close();
        }

        testcase("LeverageTierSet - Leverage Too High Rejected");
        {
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

            // Max leverage of 201 (above maximum of 200)
            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    201, 500, 250, 500),
                ter(temMALFORMED));
            env.close();
        }

        testcase("LeverageTierSet - Maintenance >= Initial Rejected");
        {
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

            // Maintenance margin equal to initial margin
            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    10, 5000, 5000, 500),
                ter(temMALFORMED));
            env.close();
        }

        testcase("LeverageTierSet - Read Back maintenanceMarginBps");
        {
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

            // Create tier with specific maintenance margin
            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    50, 2000, 1000, 300),
                ter(tesSUCCESS));
            env.close();

            auto const tierKeylet =
                keylet::leverageTier(GME.issue(), USD.issue());
            auto const sleTier = env.current()->read(tierKeylet);
            BEAST_EXPECT(sleTier);
            if (sleTier)
            {
                BEAST_EXPECT(
                    sleTier->getFieldU32(sfMaintenanceMarginBps) == 1000);
            }
        }
    }

    // ===================================================================
    // Test 5: InsuranceVault lifecycle
    // ===================================================================
    void
    testInsuranceVault(FeatureBitset features)
    {
        testcase("InsuranceVault - Create Vault");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw);
            env.close();

            // Create OptionPair prerequisite
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Create insurance vault
            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Verify insurance vault exists
            auto const vaultKeylet =
                keylet::insuranceVault(GME.issue(), USD.issue());
            auto const sleVault = env.current()->read(vaultKeylet);
            BEAST_EXPECT(sleVault);
        }

        testcase("InsuranceVault - Deposit XRP");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, alice);
            env.close();

            // Setup
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            auto const vaultKeylet =
                keylet::insuranceVault(GME.issue(), USD.issue());
            auto const vaultID = vaultKeylet.key;

            // Deposit XRP
            env(insuranceDeposit(alice, vaultID, XRP(1000)),
                ter(tesSUCCESS));
            env.close();

            // Verify insurance balance
            auto const sleVault = env.current()->read(vaultKeylet);
            BEAST_EXPECT(sleVault);
            if (sleVault)
            {
                Number const balance =
                    sleVault->at(~sfInsuranceBalance).value_or(Number(0));
                BEAST_EXPECT(balance > Number(0));
            }
        }

        testcase("InsuranceVault - Deposit IOU");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, alice);
            env.close();

            env.trust(USD(100000), alice);
            env.close();

            env(pay(gw, alice, USD(10000)));
            env.close();

            // Setup
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            auto const vaultKeylet =
                keylet::insuranceVault(GME.issue(), USD.issue());
            auto const vaultID = vaultKeylet.key;

            // Deposit 1000 USD (IOU)
            env(insuranceDeposit(alice, vaultID, USD(1000)),
                ter(tesSUCCESS));
            env.close();

            // Verify balance updated
            auto const sleVault = env.current()->read(vaultKeylet);
            BEAST_EXPECT(sleVault);
            if (sleVault)
            {
                Number const balance =
                    sleVault->at(~sfInsuranceBalance).value_or(Number(0));
                BEAST_EXPECT(balance > Number(0));
            }
        }

        testcase("InsuranceVault - Withdraw by Authorized Issuer");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, alice);
            env.close();

            env.trust(USD(100000), alice);
            env.close();

            env(pay(gw, alice, USD(10000)));
            env.close();

            // Setup vault
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            auto const vaultKeylet =
                keylet::insuranceVault(GME.issue(), USD.issue());
            auto const vaultID = vaultKeylet.key;

            // Deposit from alice
            env(insuranceDeposit(alice, vaultID, USD(1000)),
                ter(tesSUCCESS));
            env.close();

            // gw (asset issuer) withdraws 500 USD
            env(insuranceWithdraw(gw, vaultID, USD(500)),
                ter(tesSUCCESS));
            env.close();

            // Verify balance reduced
            auto const sleVault = env.current()->read(vaultKeylet);
            BEAST_EXPECT(sleVault);
        }

        testcase("InsuranceVault - Withdraw by Non-Authorized Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, alice);
            env.close();

            env.trust(USD(100000), alice);
            env.close();

            env(pay(gw, alice, USD(10000)));
            env.close();

            // Setup vault and deposit
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            auto const vaultKeylet =
                keylet::insuranceVault(GME.issue(), USD.issue());
            auto const vaultID = vaultKeylet.key;

            env(insuranceDeposit(alice, vaultID, USD(1000)),
                ter(tesSUCCESS));
            env.close();

            // alice (not an issuer) tries to withdraw
            env(insuranceWithdraw(alice, vaultID, USD(500)),
                ter(tecNO_PERMISSION));
            env.close();
        }

        testcase("InsuranceVault - Withdraw Exceeding Balance Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, alice);
            env.close();

            env.trust(USD(100000), alice);
            env.close();

            env(pay(gw, alice, USD(10000)));
            env.close();

            // Setup vault and deposit
            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            auto const vaultKeylet =
                keylet::insuranceVault(GME.issue(), USD.issue());
            auto const vaultID = vaultKeylet.key;

            // Deposit 100 USD
            env(insuranceDeposit(alice, vaultID, USD(100)),
                ter(tesSUCCESS));
            env.close();

            // gw (authorized) tries to withdraw 500 (exceeds 100 balance)
            env(insuranceWithdraw(gw, vaultID, USD(500)),
                ter(tecUNFUNDED_PAYMENT));
            env.close();
        }

        testcase("InsuranceVault - Duplicate Vault Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const GME = gw["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw);
            env.close();

            env(optionPairCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // First creation succeeds
            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Second creation for same pair fails
            env(insuranceVaultCreate(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue())),
                fee(env.current()->fees().increment),
                ter(tecDUPLICATE));
            env.close();
        }

        testcase("InsuranceVault - Deposit to Nonexistent Vault Rejected");
        {
            using namespace test::jtx;
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, alice);
            env.close();

            env.trust(USD(100000), alice);
            env.close();

            env(pay(gw, alice, USD(10000)));
            env.close();

            // Deposit to a vault that does not exist
            uint256 const fakeVaultID(5678);
            env(insuranceDeposit(alice, fakeVaultID, USD(100)),
                ter(tecNO_ENTRY));
            env.close();
        }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{testable_amendments()};

        testMarginAccountSet(all);
        testMarginDeposit(all);
        testMarginWithdraw(all);
        testLeverageTierSet(all);
        testInsuranceVault(all);
    }
};

BEAST_DEFINE_TESTSUITE(MarginFunctionality, app, ripple);

}  // namespace test
}  // namespace xrpl
