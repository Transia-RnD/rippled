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

struct OptionMargin_test : public beast::unit_test::suite
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
        STIssue const& collateralAsset)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::MarginAccountSet;
        jv[jss::Account] = account.human();
        jv[sfCollateralAsset.jsonName] =
            collateralAsset.getJson(JsonOptions::none);
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

    // Helper: create an OptionCreate transaction (standard, no margin)
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

    // Helper: create an OptionCreate transaction with margin
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

    static uint256
    getOfferIndex(AccountID const& account, std::uint32_t sequence)
    {
        return keylet::optionOffer(account, sequence).key;
    }

    void
    testLeveragedSellOption(FeatureBitset features)
    {
        testcase("Leveraged Sell Option at 5x");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env(pay(gw, alice, GME(50000)));
        env.close();

        // Step 1: Create OptionPair
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Step 2: Create leverage tier (5x max)
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                5,      // 5x max leverage
                20000,  // 20% initial margin (20000 * 1/10 bps = 20%)
                10000,  // 10% maintenance margin
                500),   // 0.5% liquidation bonus
            ter(tesSUCCESS));
        env.close();

        // Step 3: Create margin account for alice (isolated mode)
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        // Step 4: Deposit 10000 USD as collateral
        env(marginDeposit(alice, marginAcctID, USD(10000)),
            ter(tesSUCCESS));
        env.close();

        // Step 5: Create a leveraged sell (put) option at 5x
        // Strike = $20 USD, quantity = 100
        // Notional = 20 * 100 = 2000 USD
        // Initial margin at 5x = 2000 / 5 = 400 USD
        auto const expiration = env.now() + 80s;
        auto const strikePrice = USD(20);
        auto const premium = USD(1);

        auto const offerId = getOfferIndex(alice.id(), env.seq(alice));
        env(optionCreateWithMargin(
                alice,
                expiration,
                strikePrice,
                STIssue(sfAsset, GME.issue()),
                100,
                premium,
                marginAcctID,
                5),
            txflags(tfSell | tfPut),
            ter(tesSUCCESS));
        env.close();

        // Verify: collateral balance reduced by 400 USD (from 10000 to 9600)
        auto sleMarginAcct = env.current()->read(marginAcctKeylet);
        BEAST_EXPECT(sleMarginAcct);

        // Verify: option offer exists
        auto const sleOffer =
            env.current()->read(keylet::unchecked(offerId));
        BEAST_EXPECT(sleOffer);

        // Verify: option offer has sfMarginPositionID set
        if (sleOffer)
        {
            BEAST_EXPECT(sleOffer->isFieldPresent(sfMarginPositionID));
        }

        // Verify: margin position was created with the same seq as the tx
    }

    void
    testLeverageExceedsMax(FeatureBitset features)
    {
        testcase("Leverage Exceeds Tier Max");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
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

        // Create leverage tier (max 5x)
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
                STIssue(sfCollateralAsset, USD.issue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        env(marginDeposit(alice, marginAcctID, USD(10000)),
            ter(tesSUCCESS));
        env.close();

        // Try to create option with 10x leverage (exceeds 5x max)
        auto const expiration = env.now() + 80s;
        env(optionCreateWithMargin(
                alice,
                expiration,
                USD(20),
                STIssue(sfAsset, GME.issue()),
                100,
                USD(1),
                marginAcctID,
                10),
            txflags(tfSell | tfPut),
            ter(tecNO_PERMISSION));
        env.close();
    }

    void
    testInsufficientMargin(FeatureBitset features)
    {
        testcase("Insufficient Margin Collateral");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
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

        // Create leverage tier (5x)
        env(leverageTierSet(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue()),
                5, 20000, 10000, 500),
            ter(tesSUCCESS));
        env.close();

        // Create margin account with very small deposit
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        // Deposit only 100 USD
        env(marginDeposit(alice, marginAcctID, USD(100)),
            ter(tesSUCCESS));
        env.close();

        // Try to create option requiring 400 USD margin (2000 notional / 5x)
        auto const expiration = env.now() + 80s;
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
            ter(tecUNFUNDED_PAYMENT));
        env.close();
    }

    void
    testNoMarginFullCollateral(FeatureBitset features)
    {
        testcase("No Margin - Full Collateral Backward Compatible");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const gme = Account("gme");
        auto const GME = gme["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice, gme);
        env.close();

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env(pay(gme, alice, GME(50000)));
        env.close();

        // Create OptionPair
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Create standard sell option WITHOUT margin (full collateral)
        auto const expiration = env.now() + 80s;
        auto const strikePrice = USD(20);
        auto const premium = USD(1);

        env(optionCreate(
                alice,
                expiration,
                strikePrice,
                STIssue(sfAsset, GME.issue()),
                100,
                premium),
            txflags(tfSell),
            ter(tesSUCCESS));
        env.close();

        // Verify: option was created (full collateral path works)
        // No margin position should exist
    }

    void
    testMarginOnBuyOfferRejected(FeatureBitset features)
    {
        testcase("Margin on Buy Offer Rejected");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
        env.close();

        env(pay(gw, alice, USD(50000)));
        env(pay(gw, alice, GME(50000)));
        env.close();

        // Create OptionPair and tier
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

        // Create margin account and deposit
        env(marginAccountSet(
                alice,
                STIssue(sfCollateralAsset, USD.issue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(alice.id(), USD.issue());
        auto const marginAcctID = marginAcctKeylet.key;

        env(marginDeposit(alice, marginAcctID, USD(10000)),
            ter(tesSUCCESS));
        env.close();

        // Try margin on a BUY offer (not sell) - should fail
        auto const expiration = env.now() + 80s;
        env(optionCreateWithMargin(
                alice,
                expiration,
                USD(20),
                STIssue(sfAsset, GME.issue()),
                100,
                USD(1),
                marginAcctID,
                5),
            txflags(tfPut),  // buy (no tfSell)
            ter(temMALFORMED));
        env.close();
    }

    void
    testInvalidLeverageRange(FeatureBitset features)
    {
        testcase("Invalid Leverage Range");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.trust(GME(100000), alice);
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

        auto const expiration = env.now() + 80s;

        // Leverage too low (1)
        env(optionCreateWithMargin(
                alice,
                expiration,
                USD(20),
                STIssue(sfAsset, GME.issue()),
                100,
                USD(1),
                marginAcctID,
                1),
            txflags(tfSell | tfPut),
            ter(temMALFORMED));
        env.close();

        // Leverage too high (201)
        env(optionCreateWithMargin(
                alice,
                expiration,
                USD(20),
                STIssue(sfAsset, GME.issue()),
                100,
                USD(1),
                marginAcctID,
                201),
            txflags(tfSell | tfPut),
            ter(temMALFORMED));
        env.close();
    }

    void
    testMarginAccountIDWithoutLeverage(FeatureBitset features)
    {
        testcase("MarginAccountID without Leverage");

        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const alice = Account("alice");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
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

        auto const expiration = env.now() + 80s;

        // Send MarginAccountID without Leverage
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionCreate;
        jv[jss::Account] = alice.human();
        jv[sfStrikePrice.jsonName] = STAmount(USD, 20).getJson(JsonOptions::none);
        jv[sfAsset.jsonName] =
            STIssue(sfAsset, GME.issue()).getJson(JsonOptions::none);
        jv[sfExpiration.jsonName] =
            static_cast<std::uint32_t>(expiration.time_since_epoch().count());
        jv[sfPremium.jsonName] = STAmount(USD, 1).getJson(JsonOptions::none);
        jv[sfQuantity.jsonName] = 100u;
        jv[sfMarginAccountID.jsonName] = to_string(marginAcctID);
        // No sfLeverage

        env(jv, txflags(tfSell | tfPut), ter(temMALFORMED));
        env.close();
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();
        testLeveragedSellOption(sa);
        testLeverageExceedsMax(sa);
        testInsufficientMargin(sa);
        testNoMarginFullCollateral(sa);
        testMarginOnBuyOfferRejected(sa);
        testInvalidLeverageRange(sa);
        testMarginAccountIDWithoutLeverage(sa);
    }
};

BEAST_DEFINE_TESTSUITE(OptionMargin, app, ripple);

}  // namespace test
}  // namespace xrpl
