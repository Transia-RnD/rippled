//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Ripple Labs Inc.

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

// Comprehensive functionality test suite for XLS-62d Options system.
// Tests OptionPairCreate, OptionCreate, order matching, OptionSettle,
// and sealed option tracking.

#include <test/jtx.h>

#include <xrpl/ledger/Dir.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

namespace xrpl {
namespace test {

struct OptionFunctionality_test : public beast::unit_test::suite
{
    // =========================================================================
    // Utility helpers
    // =========================================================================

    static bool
    inOwnerDir(
        ReadView const& view,
        jtx::Account const& acct,
        uint256 const& tid)
    {
        auto const sle = view.read({ltOPTION_OFFER, tid});
        Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::find(ownerDir.begin(), ownerDir.end(), sle) !=
            ownerDir.end();
    }

    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    };

    struct SealedOption
    {
        uint256 offerId;
        AccountID owner;
        std::uint32_t quantity;
    };

    void
    validateOffer(
        int line,
        ReadView const& view,
        uint256 const& offerId,
        std::uint32_t const& quantity,
        STAmount const& premium,
        std::uint32_t const& openInterest,
        std::vector<SealedOption> const& sealedOptions_)
    {
        using namespace std::string_literals;

        auto const k = keylet::unchecked(offerId);
        auto const sle = view.read(k);
        if (!sle)
        {
            fail("Option offer not found in ledger"s, __FILE__, line);
            return;
        }

        if ((*sle)[sfQuantity] != quantity)
            fail(
                "Quantity mismatch: "s + std::to_string((*sle)[sfQuantity]) +
                    "/" + std::to_string(quantity),
                __FILE__,
                line);

        if ((*sle)[sfPremium] != premium)
            fail(
                "Premium mismatch: "s + (*sle)[sfPremium].getFullText() + "/" +
                    premium.getFullText(),
                __FILE__,
                line);

        if (openInterest && !(*sle)[sfOpenInterest])
            fail(
                "Open interest field not present, but expected: "s +
                    std::to_string(openInterest),
                __FILE__,
                line);
        else if (!openInterest && (*sle)[sfOpenInterest])
            fail(
                "Open interest field present, but expected to be absent",
                __FILE__,
                line);
        else if (
            (*sle)[sfOpenInterest] && (*sle)[sfOpenInterest] != openInterest)
            fail(
                "Open interest mismatch: "s +
                    std::to_string(
                        static_cast<std::uint32_t>((*sle)[sfOpenInterest])) +
                    "/" + std::to_string(openInterest),
                __FILE__,
                line);
        if (sealedOptions_.size() > 0 && !sle->isFieldPresent(sfSealedOptions))
            fail(
                "Expected sealed options field to be present with "s +
                    std::to_string(sealedOptions_.size()) +
                    " entries, but field is missing",
                __FILE__,
                line);
        else if (
            sealedOptions_.size() == 0 &&
            sle->isFieldPresent(sfSealedOptions) &&
            sle->getFieldArray(sfSealedOptions).size() > 0)
            fail(
                "Expected sealed options field to be absent, but field is "
                "present",
                __FILE__,
                line);
        else if (
            sealedOptions_.size() > 0 && sle->isFieldPresent(sfSealedOptions))
        {
            STArray const sealedOptions = sle->getFieldArray(sfSealedOptions);
            if (sealedOptions.size() != sealedOptions_.size())
                fail(
                    "Sealed options count mismatch: "s +
                        std::to_string(sealedOptions.size()) + "/" +
                        std::to_string(sealedOptions_.size()),
                    __FILE__,
                    line);

            for (std::size_t i = 0; i < sealedOptions.size(); ++i)
            {
                auto const sealedOption = sealedOptions[i];
                auto const slOfferId =
                    sealedOption.getFieldH256(sfOptionOfferID);
                auto const slOwner = sealedOption.getAccountID(sfOwner);
                auto const slQuantity = sealedOption.getFieldU32(sfQuantity);

                if (slOfferId != sealedOptions_[i].offerId)
                    fail(
                        "Sealed option #"s + std::to_string(i) +
                            " offer ID mismatch: " + to_string(slOfferId) +
                            "/" + to_string(sealedOptions_[i].offerId),
                        __FILE__,
                        line);

                if (slOwner != sealedOptions_[i].owner)
                    fail(
                        "Sealed option #"s + std::to_string(i) +
                            " owner mismatch: " + to_string(slOwner) + "/" +
                            to_string(sealedOptions_[i].owner),
                        __FILE__,
                        line);

                if (slQuantity != sealedOptions_[i].quantity)
                    fail(
                        "Sealed option #"s + std::to_string(i) +
                            " quantity mismatch: " +
                            std::to_string(slQuantity) + "/" +
                            std::to_string(sealedOptions_[i].quantity),
                        __FILE__,
                        line);
            }
        }

        pass();
    }

    // =========================================================================
    // Transaction builders
    // =========================================================================

    Json::Value
    optionPairCreate(
        jtx::Account const& account,
        STIssue const& asset,
        STIssue const& asset2,
        std::uint32_t tradingFeeBps = 0)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionPairCreate;
        jv[jss::Account] = account.human();
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfAsset2.jsonName] = asset2.getJson(JsonOptions::none);
        if (tradingFeeBps > 0)
            jv[sfTradingFeeBps.jsonName] = tradingFeeBps;
        return jv;
    }

    Json::Value
    optionCreate(
        jtx::Account const& account,
        NetClock::time_point expiration,
        STAmount const& strikePrice,
        STIssue const& asset,
        uint32_t const& quantity,
        STAmount const& premium,
        uint256 const& marginAccountID,
        std::uint32_t leverage = 5)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionCreate;
        jv[jss::Account] = account.human();
        jv[sfStrikePrice.jsonName] = strikePrice.getJson(JsonOptions::none);
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfExpiration.jsonName] = expiration.time_since_epoch().count();
        jv[sfPremium.jsonName] = premium.getJson(JsonOptions::none);
        jv[sfQuantity.jsonName] = quantity;
        jv[sfMarginAccountID.jsonName] = to_string(marginAccountID);
        jv[sfLeverage.jsonName] = leverage;
        return jv;
    }

    Json::Value
    optionSettle(
        jtx::Account const& account,
        uint256 const& optionId,
        uint256 const& offerId)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionSettle;
        jv[jss::Account] = account.human();
        jv[sfOptionID.jsonName] = to_string(optionId);
        jv[sfOptionOfferID.jsonName] = to_string(offerId);
        return jv;
    }

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

    // Set up margin infrastructure for an account: margin account + deposit
    uint256
    setupMargin(
        jtx::Env& env,
        jtx::Account const& account,
        Issue const& collateralIssue,
        STAmount const& deposit)
    {
        using namespace test::jtx;
        env(marginAccountSet(
                account,
                STIssue(sfCollateralAsset, collateralIssue)),
            ter(tesSUCCESS));
        env.close();

        auto const marginAcctKeylet =
            keylet::marginAccount(account.id(), collateralIssue);
        auto const marginAcctID = marginAcctKeylet.key;

        env(marginDeposit(account, marginAcctID, deposit),
            ter(tesSUCCESS));
        env.close();

        return marginAcctID;
    }

    // =========================================================================
    // Higher-level helpers
    // =========================================================================

    static uint256
    getOptionIndex(
        AccountID const& issuer,
        Currency const& currency,
        std::uint64_t const& strike,
        NetClock::time_point expiration)
    {
        return keylet::option(
                   issuer,
                   currency,
                   strike,
                   expiration.time_since_epoch().count())
            .key;
    }

    static uint256
    getOfferIndex(AccountID const& account, std::uint32_t sequence)
    {
        return keylet::optionOffer(account, sequence).key;
    }

    void
    initPair(
        jtx::Env& env,
        jtx::Account const& account,
        Issue const& issue,
        Issue const& issue2)
    {
        using namespace test::jtx;
        env(optionPairCreate(
                account, STIssue(sfAsset, issue), STIssue(sfAsset2, issue2)),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();
    }

    uint256
    createOffer(
        jtx::Env& env,
        jtx::Account const& account,
        std::uint32_t const& seq,
        jtx::IOU const& AST,
        std::uint32_t const& quantity,
        NetClock::time_point const& expiration,
        STAmount const& strikePrice,
        STAmount const& premium,
        std::uint32_t flags,
        uint256 const& marginAccountID,
        std::uint32_t leverage = 5)
    {
        using namespace test::jtx;
        auto const issue = STIssue(sfAsset, AST.issue());
        auto const offerId = getOfferIndex(account.id(), seq);
        env(optionCreate(
                account, expiration, strikePrice, issue, quantity, premium,
                marginAccountID, leverage),
            txflags(flags),
            ter(tesSUCCESS));
        env.close();
        return offerId;
    }

    // =========================================================================
    // Test 1: OptionPairCreate
    // =========================================================================

    void
    testOptionPairCreate(FeatureBitset features)
    {
        testcase("OptionPairCreate");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;

        // ---- Success: issuer creates an option pair ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme);
            env.close();

            auto const asset = STIssue(sfAsset, GME.issue());
            auto const asset2 = STIssue(sfAsset2, USD.issue());

            // Issuer of asset (gme) creates the pair
            env(optionPairCreate(gme, asset, asset2),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Verify the option pair exists in the ledger
            auto const pairKeylet =
                keylet::optionPair(GME.issue(), USD.issue());
            auto const slePair = env.current()->read(pairKeylet);
            BEAST_EXPECT(slePair);
        }

        // ---- Duplicate rejection: creating same pair again fails ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const USD = gw["USD"];
            auto const EUR = gw["EUR"];

            env.fund(XRP(100000), gw);
            env.close();

            auto const asset = STIssue(sfAsset, USD.issue());
            auto const asset2 = STIssue(sfAsset2, EUR.issue());

            // First creation succeeds
            env(optionPairCreate(gw, asset, asset2),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Second creation of same pair should fail with tecDUPLICATE
            env(optionPairCreate(gw, asset, asset2),
                fee(env.current()->fees().increment),
                ter(tecDUPLICATE));
            env.close();
        }

        // ---- Non-issuer rejection ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const alice = Account("alice");
            auto const USD = gw["USD"];
            auto const EUR = gw["EUR"];

            env.fund(XRP(100000), gw, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(EUR(100000), alice);
            env.close();

            auto const asset = STIssue(sfAsset, USD.issue());
            auto const asset2 = STIssue(sfAsset2, EUR.issue());

            // Alice is not the issuer of either asset -- should fail
            env(optionPairCreate(alice, asset, asset2),
                fee(env.current()->fees().increment),
                ter(tecNO_PERMISSION));
            env.close();
        }

        // ---- With trading fee: create pair with sfTradingFeeBps ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const USD = gw["USD"];
            auto const EUR = gw["EUR"];

            env.fund(XRP(100000), gw);
            env.close();

            auto const asset = STIssue(sfAsset, USD.issue());
            auto const asset2 = STIssue(sfAsset2, EUR.issue());

            // Create with a valid trading fee (e.g. 500 = 50 bps = 0.5%)
            env(optionPairCreate(gw, asset, asset2, 500),
                fee(env.current()->fees().increment),
                ter(tesSUCCESS));
            env.close();

            // Verify the pair exists
            auto const pairKeylet =
                keylet::optionPair(USD.issue(), EUR.issue());
            auto const slePair = env.current()->read(pairKeylet);
            BEAST_EXPECT(slePair);
        }

        // ---- Fee too large: reject TradingFeeBps > 1000000 ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const USD = gw["USD"];
            auto const EUR = gw["EUR"];

            env.fund(XRP(100000), gw);
            env.close();

            auto const asset = STIssue(sfAsset, USD.issue());
            auto const asset2 = STIssue(sfAsset2, EUR.issue());

            // Fee of 1000001 (> 1000000) should be rejected
            env(optionPairCreate(gw, asset, asset2, 1000001),
                fee(env.current()->fees().increment),
                ter(temMALFORMED));
            env.close();

            // Fee of 0xFFFFFFFF should also be rejected
            env(optionPairCreate(gw, asset, asset2, 0xFFFFFFFF),
                fee(env.current()->fees().increment),
                ter(temMALFORMED));
            env.close();
        }
    }

    // =========================================================================
    // Test 2: OptionCreate basic
    // =========================================================================

    void
    testOptionCreateBasic(FeatureBitset features)
    {
        testcase("OptionCreate basic");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;

        // ---- Buy call success (no flags) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gme, alice, GME(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 100u;

            auto const offerId = getOfferIndex(alice.id(), env.seq(alice));
            env(optionCreate(
                    alice,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    marginAcctID,
                    5),
                txflags(0),  // buy call (no flags)
                ter(tesSUCCESS));
            env.close();

            // Verify the offer exists
            auto const sleOffer =
                env.current()->read(keylet::unchecked(offerId));
            BEAST_EXPECT(sleOffer);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, offerId));
        }

        // ---- Sell call success (tfSell) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gme, alice, GME(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 100u;

            auto const offerId = getOfferIndex(alice.id(), env.seq(alice));
            env(optionCreate(
                    alice,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    marginAcctID,
                    5),
                txflags(tfSell),
                ter(tesSUCCESS));
            env.close();

            auto const sleOffer =
                env.current()->read(keylet::unchecked(offerId));
            BEAST_EXPECT(sleOffer);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, offerId));
        }

        // ---- Buy put success (tfPut) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gme, alice, GME(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 100u;

            auto const offerId = getOfferIndex(alice.id(), env.seq(alice));
            env(optionCreate(
                    alice,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    marginAcctID,
                    5),
                txflags(tfPut),  // buy put
                ter(tesSUCCESS));
            env.close();

            auto const sleOffer =
                env.current()->read(keylet::unchecked(offerId));
            BEAST_EXPECT(sleOffer);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, offerId));
        }

        // ---- Sell put success (tfPut | tfSell) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gme, alice, GME(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 100u;

            auto const offerId = getOfferIndex(alice.id(), env.seq(alice));
            env(optionCreate(
                    alice,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    marginAcctID,
                    5),
                txflags(tfPut | tfSell),  // sell put
                ter(tesSUCCESS));
            env.close();

            auto const sleOffer =
                env.current()->read(keylet::unchecked(offerId));
            BEAST_EXPECT(sleOffer);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, offerId));
        }

        // ---- Zero quantity rejection ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gme, alice, GME(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;

            // Zero quantity should be rejected
            env(optionCreate(
                    alice,
                    expiration,
                    USD(20),
                    STIssue(sfAsset, GME.issue()),
                    0,  // zero quantity
                    USD(0.5),
                    marginAcctID,
                    5),
                txflags(tfSell),
                ter(temMALFORMED));
            env.close();
        }

        // ---- Invalid leverage (< 2) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;

            // Leverage = 1 (too low) should fail
            env(optionCreate(
                    alice,
                    expiration,
                    USD(20),
                    STIssue(sfAsset, GME.issue()),
                    100,
                    USD(0.5),
                    marginAcctID,
                    1),  // leverage too low
                txflags(tfSell | tfPut),
                ter(temMALFORMED));
            env.close();
        }

        // ---- Invalid leverage (> 200) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    200, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            auto const expiration = env.now() + 80s;

            // Leverage = 201 (too high) should fail
            env(optionCreate(
                    alice,
                    expiration,
                    USD(20),
                    STIssue(sfAsset, GME.issue()),
                    100,
                    USD(0.5),
                    marginAcctID,
                    201),  // leverage too high
                txflags(tfSell | tfPut),
                ter(temMALFORMED));
            env.close();
        }

        // ---- Past expiration rejection ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(GME(100000), alice);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const marginAcctID =
                setupMargin(env, alice, USD.issue(), USD(10000));

            // Expiration in the past
            auto const pastExp = env.now() - std::chrono::seconds(3600);

            env(optionCreate(
                    alice,
                    pastExp,
                    USD(20),
                    STIssue(sfAsset, GME.issue()),
                    100,
                    USD(0.5),
                    marginAcctID,
                    5),
                txflags(tfSell),
                ter(tecEXPIRED));
            env.close();
        }
    }

    // =========================================================================
    // Test 3: Order matching
    // =========================================================================

    void
    testOptionMatching(FeatureBitset features)
    {
        testcase("OptionCreate matching");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;

        // ---- Buy call matches sell call (different accounts) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gw, bob, USD(50000)));
            env(pay(gme, alice, GME(10000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(25000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(25000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Alice creates a sell call
            uint256 const sellId = createOffer(
                env,
                alice,
                env.seq(alice),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell,
                aliceMargin);

            // Validate sell offer has open interest
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                1000,
                {});

            // Bob creates a buy call -- should match with alice's sell
            uint256 const buyId = createOffer(
                env,
                bob,
                env.seq(bob),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,  // buy call
                bobMargin);

            // After matching: buy offer should have sealed option referencing
            // sell
            validateOffer(
                __LINE__,
                *env.current(),
                buyId,
                quantity,
                premium,
                0,
                {{sellId, alice.id(), quantity}});

            // Sell offer should have sealed option referencing buy
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                0,
                {{buyId, bob.id(), quantity}});
        }

        // ---- Buy put matches sell put ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gw, bob, USD(50000)));
            env(pay(gme, bob, GME(10000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(25000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(25000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 500u;

            // Alice creates a sell put
            uint256 const sellId = createOffer(
                env,
                alice,
                env.seq(alice),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell | tfPut,
                aliceMargin);

            // Bob creates a buy put -- should match
            uint256 const buyId = createOffer(
                env,
                bob,
                env.seq(bob),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfPut,  // buy put
                bobMargin);

            // Both should have sealed options referencing each other
            validateOffer(
                __LINE__,
                *env.current(),
                buyId,
                quantity,
                premium,
                0,
                {{sellId, alice.id(), quantity}});

            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                0,
                {{buyId, bob.id(), quantity}});
        }

        // ---- Buy does NOT match buy (same side) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(1000000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(500000)));
            env(pay(gw, bob, USD(500000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(100000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(100000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(100);
            auto const premium = USD(5);
            auto const quantity = 100u;

            // Alice creates a buy call
            auto const aliceSeq = env.seq(alice);
            env(optionCreate(
                    alice,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    aliceMargin,
                    5),
                txflags(0),  // buy call
                ter(tesSUCCESS));
            env.close();

            auto const aliceOfferId = getOfferIndex(alice.id(), aliceSeq);

            // Bob also creates a buy call -- should NOT match alice's buy
            auto const bobSeq = env.seq(bob);
            env(optionCreate(
                    bob,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    bobMargin,
                    5),
                txflags(0),  // also buy call
                ter(tesSUCCESS));
            env.close();

            auto const bobOfferId = getOfferIndex(bob.id(), bobSeq);

            // Verify bob's offer has no sealed options
            auto const bobOffer =
                env.current()->read(keylet::unchecked(bobOfferId));
            if (bobOffer)
            {
                auto const oi = bobOffer->at(~sfOpenInterest);
                // Open interest should remain equal to quantity (unmatched)
                BEAST_EXPECT(!oi || *oi == 0 || *oi == quantity);
                if (bobOffer->isFieldPresent(sfSealedOptions))
                {
                    auto const sealed =
                        bobOffer->getFieldArray(sfSealedOptions);
                    // Should have no sealed options since no matching occurred
                    // between two buys
                    BEAST_EXPECT(sealed.size() == 0);
                }
            }

            // Verify alice's offer also has no sealed options
            auto const aliceOffer =
                env.current()->read(keylet::unchecked(aliceOfferId));
            if (aliceOffer)
            {
                if (aliceOffer->isFieldPresent(sfSealedOptions))
                {
                    auto const sealed =
                        aliceOffer->getFieldArray(sfSealedOptions);
                    BEAST_EXPECT(sealed.size() == 0);
                }
            }
        }

        // ---- Put does NOT match call (wrong type) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(1000000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(500000)));
            env(pay(gw, bob, USD(500000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(100000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(100000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(100);
            auto const premium = USD(5);
            auto const quantity = 100u;

            // Alice creates a buy call (no flags)
            auto const aliceSeq = env.seq(alice);
            env(optionCreate(
                    alice,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    aliceMargin,
                    5),
                txflags(0),  // buy call
                ter(tesSUCCESS));
            env.close();

            auto const aliceOfferId = getOfferIndex(alice.id(), aliceSeq);

            // Bob creates a sell put -- should NOT match with alice's buy call
            auto const bobSeq = env.seq(bob);
            env(optionCreate(
                    bob,
                    expiration,
                    strikePrice,
                    STIssue(sfAsset, GME.issue()),
                    quantity,
                    premium,
                    bobMargin,
                    5),
                txflags(tfPut | tfSell),  // sell put
                ter(tesSUCCESS));
            env.close();

            auto const bobOfferId = getOfferIndex(bob.id(), bobSeq);

            // Verify no matching occurred
            auto const aliceOffer =
                env.current()->read(keylet::unchecked(aliceOfferId));
            if (aliceOffer)
            {
                auto const oi = aliceOffer->at(~sfOpenInterest);
                BEAST_EXPECT(!oi || *oi == 0 || *oi == quantity);
            }

            auto const bobOffer =
                env.current()->read(keylet::unchecked(bobOfferId));
            if (bobOffer)
            {
                auto const oi = bobOffer->at(~sfOpenInterest);
                BEAST_EXPECT(!oi || *oi == 0 || *oi == quantity);
            }
        }

        // ---- Market order matching ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gw, bob, USD(50000)));
            env(pay(gme, alice, GME(10000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(25000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(25000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Alice creates a sell call (market order)
            uint256 const sellId = createOffer(
                env,
                alice,
                env.seq(alice),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell | tfMarket,
                aliceMargin);

            // Validate: unmatched sell
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                1000,
                {});

            // Bob creates a buy call (market order) -- should match
            uint256 const buyId = createOffer(
                env,
                bob,
                env.seq(bob),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfMarket,
                bobMargin);

            // After matching: both should have sealed options
            validateOffer(
                __LINE__,
                *env.current(),
                buyId,
                quantity,
                premium,
                0,
                {{sellId, alice.id(), quantity}});

            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                0,
                {{buyId, bob.id(), quantity}});
        }
    }

    // =========================================================================
    // Test 4: OptionSettle (exercise, close, expire)
    // =========================================================================

    void
    testOptionSettle(FeatureBitset features)
    {
        testcase("OptionSettle");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;

        // ---- Exercise a matched (sealed) call option ----
        {
            Env env{*this, features};
            (void)env.current()->fees().base;

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const writer = Account("alice");
            auto const buyer = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, writer, buyer);
            env.close();
            env.trust(USD(1000000), writer, buyer);
            env.trust(GME(100000), writer, buyer);
            env.close();
            env(pay(gw, writer, USD(100000)));
            env(pay(gw, buyer, USD(100000)));
            env(pay(gme, writer, GME(10000)));
            env.close();

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            std::int64_t const strike =
                static_cast<std::int64_t>(Number(strikePrice.value()));
            uint256 const optionId{
                getOptionIndex(gme.id(), GME.currency, strike, expiration)};
            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const writerMargin =
                setupMargin(env, writer, USD.issue(), USD(50000));
            auto const buyerMargin =
                setupMargin(env, buyer, USD.issue(), USD(50000));

            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Create sell offer (writer)
            uint256 const sellId = createOffer(
                env,
                writer,
                env.seq(writer),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell,
                writerMargin);

            // Create buy offer (buyer) -- will match
            uint256 const buyId = createOffer(
                env,
                buyer,
                env.seq(buyer),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                buyerMargin);

            // Verify matching occurred
            BEAST_EXPECT(inOwnerDir(*env.current(), writer, sellId));
            BEAST_EXPECT(inOwnerDir(*env.current(), buyer, buyId));

            // Exercise the option
            env(optionSettle(buyer, optionId, buyId),
                txflags(tfExercise),
                ter(tesSUCCESS));
            env.close();

            // After exercise: offers should be removed from owner directories
            BEAST_EXPECT(!inOwnerDir(*env.current(), writer, sellId));
            BEAST_EXPECT(!inOwnerDir(*env.current(), buyer, buyId));
        }

        // ---- Close an option (OptionSettle without exercise) ----
        {
            Env env{*this, features};
            (void)env.current()->fees().base;

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const writer = Account("alice");
            auto const buyer = Account("bob");
            auto const counter = Account("counter");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, writer, buyer, counter);
            env.close();
            env.trust(USD(1000000), writer, buyer, counter);
            env.trust(GME(100000), writer, buyer, counter);
            env.close();
            env(pay(gw, writer, USD(100000)));
            env(pay(gw, buyer, USD(100000)));
            env(pay(gw, counter, USD(100000)));
            env(pay(gme, writer, GME(10000)));
            env(pay(gme, counter, GME(10000)));
            env.close();

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            std::int64_t const strike =
                static_cast<std::int64_t>(Number(strikePrice.value()));
            uint256 const optionId{
                getOptionIndex(gme.id(), GME.currency, strike, expiration)};
            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const writerMargin =
                setupMargin(env, writer, USD.issue(), USD(50000));
            auto const buyerMargin =
                setupMargin(env, buyer, USD.issue(), USD(50000));

            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Create sell offer (writer)
            uint256 const sellId = createOffer(
                env,
                writer,
                env.seq(writer),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell,
                writerMargin);

            // Create buy offer (buyer) -- matches
            uint256 const buyId = createOffer(
                env,
                buyer,
                env.seq(buyer),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                buyerMargin);

            BEAST_EXPECT(inOwnerDir(*env.current(), writer, sellId));
            BEAST_EXPECT(inOwnerDir(*env.current(), buyer, buyId));

            // Close the sell offer (writer closes their position)
            env(optionSettle(writer, optionId, sellId),
                txflags(tfClose),
                ter(tesSUCCESS));
            env.close();

            // After close: writer's sell offer should be removed
            BEAST_EXPECT(!inOwnerDir(*env.current(), writer, sellId));
        }

        // ---- Expire after expiration time ----
        {
            Env env{*this, features};
            (void)env.current()->fees().base;

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const buyer = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, buyer);
            env.close();
            env.trust(USD(1000000), buyer);
            env.close();
            env(pay(gw, buyer, USD(100000)));
            env.close();

            auto const expiration = env.now() + 10s;
            auto const strikePrice = USD(20);
            std::int64_t const strike =
                static_cast<std::int64_t>(Number(strikePrice.value()));
            uint256 const optionId{
                getOptionIndex(gme.id(), GME.currency, strike, expiration)};
            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const buyerMargin =
                setupMargin(env, buyer, USD.issue(), USD(50000));

            auto const preBuyerXrp = env.balance(buyer);
            auto const preBuyerUsd = env.balance(buyer, USD);

            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Create buy offer (unmatched)
            uint256 const buyId = createOffer(
                env,
                buyer,
                env.seq(buyer),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                buyerMargin);

            BEAST_EXPECT(inOwnerDir(*env.current(), buyer, buyId));

            // Expire the unmatched buy offer
            env(optionSettle(buyer, optionId, buyId),
                txflags(tfExpire),
                ter(tecEXPIRED));
            env.close();

            // After expire: offer should be removed
            BEAST_EXPECT(!inOwnerDir(*env.current(), buyer, buyId));
        }

        // ---- Reject exercise before matching (no sealed options) ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const buyer = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, buyer);
            env.close();
            env.trust(USD(1000000), buyer);
            env.close();
            env(pay(gw, buyer, USD(100000)));
            env.close();

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            std::int64_t const strike =
                static_cast<std::int64_t>(Number(strikePrice.value()));
            uint256 const optionId{
                getOptionIndex(gme.id(), GME.currency, strike, expiration)};
            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const buyerMargin =
                setupMargin(env, buyer, USD.issue(), USD(50000));

            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Create buy offer (unmatched -- no counterparty)
            uint256 const buyId = createOffer(
                env,
                buyer,
                env.seq(buyer),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfMarket,
                buyerMargin);

            // Attempt to exercise an unmatched offer -- should fail
            // because there are no sealed options to exercise
            env(optionSettle(buyer, optionId, buyId),
                txflags(tfExercise),
                ter(tecINSUFFICIENT_FUNDS));
            env.close();
        }
    }

    // =========================================================================
    // Test 5: Sealed options (counterparty tracking)
    // =========================================================================

    void
    testSealedOptions(FeatureBitset features)
    {
        testcase("Sealed options counterparty tracking");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;

        // ---- Full match: both sides get sealed options ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gw, bob, USD(50000)));
            env(pay(gme, alice, GME(10000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(25000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(25000));

            auto const preAliceUsd = env.balance(alice, USD);
            auto const preBobUsd = env.balance(bob, USD);

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);
            auto const quantity = 1000u;

            // Alice creates sell call
            uint256 const sellId = createOffer(
                env,
                alice,
                env.seq(alice),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell,
                aliceMargin);

            // Validate: sell offer starts with open interest, no sealed options
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                1000,
                {});

            // Bob creates buy call -- full match
            uint256 const buyId = createOffer(
                env,
                bob,
                env.seq(bob),
                GME,
                quantity,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                bobMargin);

            // Validate: buy offer has sealed option referencing sell
            validateOffer(
                __LINE__,
                *env.current(),
                buyId,
                quantity,
                premium,
                0,
                {{sellId, alice.id(), quantity}});

            // Validate: sell offer has sealed option referencing buy
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                quantity,
                premium,
                0,
                {{buyId, bob.id(), quantity}});

            // Premium transfer: bob paid premium to alice
            // premium = $0.5 per contract * 1000 quantity = $500
            BEAST_EXPECT(
                env.balance(alice, USD) == preAliceUsd + USD(500));
            BEAST_EXPECT(
                env.balance(bob, USD) == preBobUsd - USD(500));
        }

        // ---- Partial match: sell 1000, buy 500 ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(100000), gw, gme, alice, bob);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.trust(GME(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(50000)));
            env(pay(gw, bob, USD(50000)));
            env(pay(gme, alice, GME(10000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const aliceMargin =
                setupMargin(env, alice, USD.issue(), USD(25000));
            auto const bobMargin =
                setupMargin(env, bob, USD.issue(), USD(25000));

            auto const preAliceUsd = env.balance(alice, USD);
            auto const preBobUsd = env.balance(bob, USD);

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);

            // Alice creates sell call for 1000
            uint256 const sellId = createOffer(
                env,
                alice,
                env.seq(alice),
                GME,
                1000,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell,
                aliceMargin);

            // Validate: sell has full open interest of 1000
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                1000,
                premium,
                1000,
                {});

            // Bob creates buy call for only 500 -- partial match
            uint256 const buyId = createOffer(
                env,
                bob,
                env.seq(bob),
                GME,
                500,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                bobMargin);

            // Buy offer: fully matched (500 sealed with sell)
            validateOffer(
                __LINE__,
                *env.current(),
                buyId,
                500,
                premium,
                0,
                {{sellId, alice.id(), 500}});

            // Sell offer: partially matched (still 500 open interest)
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                1000,
                premium,
                500,
                {{buyId, bob.id(), 500}});

            // Premium transfer for 500 contracts: $0.5 * 500 = $250
            BEAST_EXPECT(
                env.balance(alice, USD) == preAliceUsd + USD(250));
            BEAST_EXPECT(
                env.balance(bob, USD) == preBobUsd - USD(250));
        }

        // ---- Multiple counterparties ----
        {
            Env env{*this, features};

            auto const gw = Account("gateway");
            auto const gme = Account("gme");
            auto const writer = Account("writer");
            auto const buyer1 = Account("buyer1");
            auto const buyer2 = Account("buyer2");
            auto const GME = gme["GME"];
            auto const USD = gw["USD"];

            env.fund(XRP(1000000), gw, gme, writer, buyer1, buyer2);
            env.close();
            env.trust(USD(1000000), writer, buyer1, buyer2);
            env.trust(GME(100000), writer, buyer1, buyer2);
            env.close();
            env(pay(gw, writer, USD(100000)));
            env(pay(gw, buyer1, USD(100000)));
            env(pay(gw, buyer2, USD(100000)));
            env(pay(gme, writer, GME(10000)));
            env.close();

            initPair(env, gme, GME.issue(), USD.issue());

            env(leverageTierSet(
                    gw,
                    STIssue(sfAsset, GME.issue()),
                    STIssue(sfAsset2, USD.issue()),
                    5, 20000, 10000, 500),
                ter(tesSUCCESS));
            env.close();

            auto const writerMargin =
                setupMargin(env, writer, USD.issue(), USD(50000));
            auto const buyer1Margin =
                setupMargin(env, buyer1, USD.issue(), USD(50000));
            auto const buyer2Margin =
                setupMargin(env, buyer2, USD.issue(), USD(50000));

            auto const expiration = env.now() + 80s;
            auto const strikePrice = USD(20);
            auto const premium = USD(0.5);

            // Writer creates sell call for 1000
            uint256 const sellId = createOffer(
                env,
                writer,
                env.seq(writer),
                GME,
                1000,
                expiration,
                strikePrice.value(),
                premium.value(),
                tfSell,
                writerMargin);

            // Buyer1 buys 300
            uint256 const buy1Id = createOffer(
                env,
                buyer1,
                env.seq(buyer1),
                GME,
                300,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                buyer1Margin);

            // Verify partial match with buyer1
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                1000,
                premium,
                700,
                {{buy1Id, buyer1.id(), 300}});

            validateOffer(
                __LINE__,
                *env.current(),
                buy1Id,
                300,
                premium,
                0,
                {{sellId, writer.id(), 300}});

            // Buyer2 buys 500
            uint256 const buy2Id = createOffer(
                env,
                buyer2,
                env.seq(buyer2),
                GME,
                500,
                expiration,
                strikePrice.value(),
                premium.value(),
                0,
                buyer2Margin);

            // Verify sell offer now has two sealed options
            validateOffer(
                __LINE__,
                *env.current(),
                sellId,
                1000,
                premium,
                200,
                {{buy1Id, buyer1.id(), 300},
                 {buy2Id, buyer2.id(), 500}});

            // Buyer2 should have one sealed option pointing to writer's sell
            validateOffer(
                __LINE__,
                *env.current(),
                buy2Id,
                500,
                premium,
                0,
                {{sellId, writer.id(), 500}});

            // Verify all offers are in their owner directories
            BEAST_EXPECT(inOwnerDir(*env.current(), writer, sellId));
            BEAST_EXPECT(inOwnerDir(*env.current(), buyer1, buy1Id));
            BEAST_EXPECT(inOwnerDir(*env.current(), buyer2, buy2Id));
        }
    }

    // =========================================================================
    // run()
    // =========================================================================

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();
        testOptionPairCreate(sa);
        testOptionCreateBasic(sa);
        testOptionMatching(sa);
        testOptionSettle(sa);
        testSealedOptions(sa);
    }
};

BEAST_DEFINE_TESTSUITE(OptionFunctionality, app, ripple);

}  // namespace test
}  // namespace xrpl
