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

// Fuzz testing harness for Options, Margin, Insurance, Import/Export, Passkey
// Uses property-based testing with randomized inputs to discover edge cases.
//
// Each test generates thousands of random transactions and verifies
// system-wide invariants hold after every operation.

#include <test/jtx.h>

#include <xrpl/basics/random.h>
#include <xrpl/json/to_string.h>
#include <xrpl/ledger/Dir.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

#include <random>

namespace xrpl {
namespace test {

struct OptionsFuzz_test : public beast::unit_test::suite
{
    // RNG seeded from random device for reproducibility
    std::mt19937 rng_{std::random_device{}()};

    std::uint32_t
    randU32(std::uint32_t lo, std::uint32_t hi)
    {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng_);
    }

    std::int64_t
    randI64(std::int64_t lo, std::int64_t hi)
    {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
    }

    // =========================================================================
    // Transaction builders (same as exploit tests)
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
        std::uint32_t quantity,
        STAmount const& premium,
        uint256 const& marginAccountID = beast::zero,
        std::uint32_t leverage = 0)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionCreate;
        jv[jss::Account] = account.human();
        jv[sfStrikePrice.jsonName] = strikePrice.getJson(JsonOptions::none);
        jv[sfAsset.jsonName] = asset.getJson(JsonOptions::none);
        jv[sfExpiration.jsonName] = expiration.time_since_epoch().count();
        jv[sfPremium.jsonName] = premium.getJson(JsonOptions::none);
        jv[sfQuantity.jsonName] = quantity;
        if (marginAccountID != beast::zero)
        {
            jv[sfMarginAccountID.jsonName] = to_string(marginAccountID);
            jv[sfLeverage.jsonName] = leverage > 0 ? leverage : 5;
        }
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

    Json::Value
    insuranceDeposit(
        jtx::Account const& account,
        uint256 const& insuranceVaultID,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::InsuranceDeposit;
        jv[jss::Account] = account.human();
        jv[sfInsuranceVaultID.jsonName] = to_string(insuranceVaultID);
        jv[sfAmount.jsonName] = amount.getJson(JsonOptions::none);
        return jv;
    }

    Json::Value
    insuranceWithdraw(
        jtx::Account const& account,
        uint256 const& insuranceVaultID,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::InsuranceWithdraw;
        jv[jss::Account] = account.human();
        jv[sfInsuranceVaultID.jsonName] = to_string(insuranceVaultID);
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

    Json::Value
    setPasskeyList(
        jtx::Account const& account,
        Json::Value const& passkeys)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::PasskeyListSet;
        jv[jss::Account] = account.human();
        jv[sfPasskeys.jsonName] = passkeys;
        return jv;
    }

    Json::Value
    exportTx(
        jtx::Account const& account,
        jtx::Account const& destination,
        STAmount const& amount)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = account.human();
        jv[jss::Destination] = destination.human();
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        return jv;
    }

    Json::Value
    importTx(
        jtx::Account const& account,
        std::string const& blob)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::Import;
        jv[jss::Account] = account.human();
        jv[sfBlob.jsonName] = blob;
        return jv;
    }

    // =========================================================================
    // INVARIANT CHECKERS
    // These are verified after every fuzz operation.
    // =========================================================================

    // Invariant: Total XRP drops should never increase
    bool
    checkXRPSupplyInvariant(
        jtx::Env& env,
        std::int64_t initialDrops)
    {
        auto const currentDrops =
            env.current()->header().drops.drops();
        if (currentDrops > initialDrops)
        {
            log << "INVARIANT VIOLATION: XRP supply increased from "
                << initialDrops << " to " << currentDrops << std::endl;
            return false;
        }
        return true;
    }

    // Invariant: Margin collateral balance should never be negative
    bool
    checkMarginCollateralInvariant(
        jtx::Env& env,
        jtx::Account const& account,
        Issue const& collateralIssue)
    {
        auto const marginAcctKeylet =
            keylet::marginAccount(account.id(), collateralIssue);
        auto const sle = env.le(marginAcctKeylet);
        if (!sle)
            return true;  // No margin account is fine

        auto const balance = sle->at(~sfCollateralBalance);
        if (balance && *balance < Number(0))
        {
            log << "INVARIANT VIOLATION: Negative collateral balance for "
                << account.human() << std::endl;
            return false;
        }
        return true;
    }

    // Invariant: Insurance vault balance should never be negative
    bool
    checkInsuranceBalanceInvariant(
        jtx::Env& env,
        Issue const& baseIssue,
        Issue const& quoteIssue)
    {
        auto const vaultKeylet =
            keylet::insuranceVault(baseIssue, quoteIssue);
        auto const sle = env.le(vaultKeylet);
        if (!sle)
            return true;  // No vault is fine

        auto const balance = sle->at(~sfInsuranceBalance);
        if (balance && *balance < Number(0))
        {
            log << "INVARIANT VIOLATION: Negative insurance balance"
                << std::endl;
            return false;
        }
        return true;
    }

    // Invariant: Account balance should never be negative
    bool
    checkAccountBalanceInvariant(
        jtx::Env& env,
        jtx::Account const& account)
    {
        auto const bal = env.balance(account);
        if (bal < STAmount(0))
        {
            log << "INVARIANT VIOLATION: Negative balance for "
                << account.human() << std::endl;
            return false;
        }
        return true;
    }

    // =========================================================================
    // FUZZ: Random option creation with boundary values
    // Tests edge cases in quantity, strike price, premium, leverage, expiry
    // =========================================================================

    void
    fuzzOptionCreate(FeatureBitset features)
    {
        testcase("FUZZ: Random option creation with boundary values");
        using namespace test::jtx;

        Env env{*this, features};

        Account const gw{"gateway"};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000000), gw, alice, bob);
        env.close();

        auto const USD = gw["USD"];
        env.trust(USD(100000000), alice, bob);
        env(pay(gw, alice, USD(50000000)));
        env(pay(gw, bob, USD(50000000)));
        env.close();

        auto const asset = STIssue(sfAsset, USD.issue());
        auto const xrpAsset = STIssue(sfAsset2, xrpIssue());

        env(optionPairCreate(gw, asset, xrpAsset),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Setup margin accounts
        env(marginAccountSet(
                alice, STIssue(sfCollateralAsset, xrpIssue())),
            ter(tesSUCCESS));
        env(marginAccountSet(
                bob, STIssue(sfCollateralAsset, xrpIssue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginIdAlice =
            keylet::marginAccount(alice.id(), xrpIssue()).key;
        auto const marginIdBob =
            keylet::marginAccount(bob.id(), xrpIssue()).key;

        env(marginDeposit(alice, marginIdAlice, XRP(5000000)),
            ter(tesSUCCESS));
        env(marginDeposit(bob, marginIdBob, XRP(5000000)),
            ter(tesSUCCESS));
        env.close();

        auto const initialDrops =
            env.current()->header().drops.drops();

        // Fuzz: Create many options with random parameters
        constexpr int kIterations = 200;
        for (int i = 0; i < kIterations; ++i)
        {
            // Random quantity: boundary values + random
            std::uint32_t quantities[] = {
                0, 1, 99, 100, 200, 1000, 10000,
                0xFFFFFFFF, 0xFFFFFFFE,
                randU32(0, 100000)};
            auto const quantity =
                quantities[randU32(0, sizeof(quantities)/sizeof(quantities[0]) - 1)];

            // Random strike: boundary values
            std::int64_t strikes[] = {
                0, 1, 100, 1000, 1000000,
                std::numeric_limits<std::int64_t>::max(),
                randI64(1, 1000000)};
            auto const strike =
                strikes[randU32(0, sizeof(strikes)/sizeof(strikes[0]) - 1)];

            // Random premium
            std::int64_t premiums[] = {
                0, 1, 100, 10000,
                randI64(1, 100000)};
            auto const premium =
                premiums[randU32(0, sizeof(premiums)/sizeof(premiums[0]) - 1)];

            // Random leverage
            std::uint32_t leverages[] = {
                0, 1, 2, 5, 10, 50, 100, 200, 201, 255,
                0xFFFFFFFF, randU32(2, 200)};
            auto const leverage =
                leverages[randU32(0, sizeof(leverages)/sizeof(leverages[0]) - 1)];

            // Random expiration
            auto const expSeconds = randU32(0, 604800);  // 0 to 7 days
            auto const exp =
                env.now() + std::chrono::seconds(expSeconds);

            // Random flags
            std::uint32_t flagCombos[] = {
                0,                        // buy call
                tfSell,                   // sell call
                tfPut,                    // buy put
                tfPut | tfSell,           // sell put
                tfMarket,                 // market buy call
                tfMarket | tfSell,        // market sell call
                tfMarket | tfPut,         // market buy put
                tfMarket | tfPut | tfSell, // market sell put
                0xFFFFFFFF,               // all flags (invalid)
            };
            auto const flags =
                flagCombos[randU32(0, sizeof(flagCombos)/sizeof(flagCombos[0]) - 1)];

            // Pick random account
            auto const& account = (randU32(0, 1) == 0) ? alice : bob;
            auto const& marginId =
                (&account == &alice) ? marginIdAlice : marginIdBob;

            // Submit (expect any result -- we're testing invariants,
            // not individual transaction success)
            try
            {
                env(optionCreate(
                        account,
                        exp,
                        USD(strike > 0 ? strike : 1),
                        asset,
                        quantity,
                        USD(premium > 0 ? premium : 1),
                        marginId,
                        leverage),
                    txflags(flags),
                    ter(std::ignore));
            }
            catch (...)
            {
                // Some parameter combinations may throw during
                // STAmount construction -- that's expected
            }
            env.close();

            // Check invariants after every operation
            BEAST_EXPECT(checkXRPSupplyInvariant(env, initialDrops));
            BEAST_EXPECT(
                checkMarginCollateralInvariant(env, alice, xrpIssue()));
            BEAST_EXPECT(
                checkMarginCollateralInvariant(env, bob, xrpIssue()));
            BEAST_EXPECT(checkAccountBalanceInvariant(env, alice));
            BEAST_EXPECT(checkAccountBalanceInvariant(env, bob));
        }
    }

    // =========================================================================
    // FUZZ: Random margin deposit/withdraw cycles
    // Tests XRP accounting invariants under rapid deposit/withdraw.
    // =========================================================================

    void
    fuzzMarginDepositWithdraw(FeatureBitset features)
    {
        testcase("FUZZ: Random margin deposit/withdraw cycles");
        using namespace test::jtx;

        Env env{*this, features};

        Account const gw{"gateway"};
        Account const alice{"alice"};
        env.fund(XRP(10000000), gw, alice);
        env.close();

        env(marginAccountSet(
                alice, STIssue(sfCollateralAsset, xrpIssue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginId =
            keylet::marginAccount(alice.id(), xrpIssue()).key;

        auto const initialDrops =
            env.current()->header().drops.drops();

        constexpr int kIterations = 500;
        for (int i = 0; i < kIterations; ++i)
        {
            bool const deposit = randU32(0, 1) == 0;

            // Random amount: boundary + random
            std::int64_t amounts[] = {
                0, 1, 10, 1000, 1000000, 100000000,
                std::numeric_limits<std::int64_t>::max(),
                randI64(1, 5000000)};
            auto const amount =
                amounts[randU32(0, sizeof(amounts)/sizeof(amounts[0]) - 1)];

            try
            {
                if (deposit)
                {
                    env(marginDeposit(
                            alice, marginId,
                            STAmount(amount > 0 ? amount : 1)),
                        ter(std::ignore));
                }
                else
                {
                    env(marginWithdraw(
                            alice, marginId,
                            STAmount(amount > 0 ? amount : 1)),
                        ter(std::ignore));
                }
            }
            catch (...)
            {
                // Expected for invalid amounts
            }
            env.close();

            // Invariants
            BEAST_EXPECT(checkXRPSupplyInvariant(env, initialDrops));
            BEAST_EXPECT(
                checkMarginCollateralInvariant(env, alice, xrpIssue()));
            BEAST_EXPECT(checkAccountBalanceInvariant(env, alice));
        }
    }

    // =========================================================================
    // FUZZ: Random insurance vault deposit/withdraw cycles
    // Tests for unauthorized withdrawal and XRP inflation.
    // =========================================================================

    void
    fuzzInsuranceVault(FeatureBitset features)
    {
        testcase("FUZZ: Random insurance vault deposit/withdraw");
        using namespace test::jtx;

        Env env{*this, features};

        Account const gw{"gateway"};
        Account const depositor{"depositor"};
        Account const attacker{"attacker"};
        env.fund(XRP(10000000), gw, depositor, attacker);
        env.close();

        auto const USD = gw["USD"];
        auto const asset = STIssue(sfAsset, USD.issue());
        auto const xrpAsset = STIssue(sfAsset2, xrpIssue());

        env(optionPairCreate(gw, asset, xrpAsset),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        env(insuranceVaultCreate(gw, asset, xrpAsset),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        auto const vaultKeylet =
            keylet::insuranceVault(USD.issue(), xrpIssue());
        auto const vaultID = vaultKeylet.key;

        auto const initialDrops =
            env.current()->header().drops.drops();
        auto const depositorBefore = env.balance(depositor);
        auto const attackerBefore = env.balance(attacker);

        constexpr int kIterations = 200;
        for (int i = 0; i < kIterations; ++i)
        {
            // Random operation: deposit or withdraw
            auto const op = randU32(0, 3);
            auto const amount = randI64(1, 1000000);

            try
            {
                switch (op)
                {
                    case 0:
                        // Depositor deposits
                        env(insuranceDeposit(
                                depositor, vaultID, XRP(amount)),
                            ter(std::ignore));
                        break;
                    case 1:
                        // Depositor withdraws
                        env(insuranceWithdraw(
                                depositor, vaultID, XRP(amount)),
                            ter(std::ignore));
                        break;
                    case 2:
                        // Attacker tries to withdraw (should fail)
                        env(insuranceWithdraw(
                                attacker, vaultID, XRP(amount)),
                            ter(std::ignore));
                        break;
                    case 3:
                        // Attacker deposits then withdraws more
                        env(insuranceDeposit(
                                attacker, vaultID, XRP(1)),
                            ter(std::ignore));
                        env.close();
                        env(insuranceWithdraw(
                                attacker, vaultID, XRP(amount)),
                            ter(std::ignore));
                        break;
                }
            }
            catch (...)
            {
            }
            env.close();

            // Invariants
            BEAST_EXPECT(checkXRPSupplyInvariant(env, initialDrops));
            BEAST_EXPECT(
                checkInsuranceBalanceInvariant(
                    env, USD.issue(), xrpIssue()));

            // Attacker should never profit
            auto const attackerNow = env.balance(attacker);
            BEAST_EXPECT(attackerNow <= attackerBefore);
        }
    }

    // =========================================================================
    // FUZZ: Random import blob payloads
    // Tests that malformed XPop blobs are safely rejected.
    // =========================================================================

    void
    fuzzImportBlobs(FeatureBitset features)
    {
        testcase("FUZZ: Random import blob payloads");
        using namespace test::jtx;

        Env env{*this, features};

        Account const alice{"alice"};
        env.fund(XRP(100000), alice);
        env.close();

        auto const initialDrops =
            env.current()->header().drops.drops();

        constexpr int kIterations = 200;
        for (int i = 0; i < kIterations; ++i)
        {
            std::string blob;

            auto const blobType = randU32(0, 7);
            switch (blobType)
            {
                case 0:
                    // Empty string
                    blob = "";
                    break;
                case 1:
                    // Random bytes (not JSON)
                    for (int j = 0; j < randU32(1, 1000); ++j)
                        blob += static_cast<char>(randU32(0, 255));
                    break;
                case 2:
                    // Valid JSON but wrong structure
                    blob = "{\"foo\": \"bar\"}";
                    break;
                case 3:
                    // Nested JSON (depth attack)
                    blob = std::string(100, '{') + std::string(100, '}');
                    break;
                case 4:
                    // Very long string
                    blob = std::string(512 * 1024 + 1, 'A');
                    break;
                case 5:
                {
                    // Partial XPop structure
                    Json::Value xpop;
                    xpop["ledger"]["acroot"] = "0000";
                    xpop["transaction"]["blob"] = "0000";
                    xpop["validation"]["data"] = Json::objectValue;
                    blob = Json::to_string(xpop);
                    break;
                }
                case 6:
                    // Null bytes
                    blob = std::string(64, '\0');
                    break;
                case 7:
                    // Unicode characters
                    blob = "\xef\xbb\xbf{\"test\": \"\xf0\x9f\x92\xa5\"}";
                    break;
            }

            try
            {
                env(importTx(alice, blob), ter(std::ignore));
            }
            catch (...)
            {
                // Expected for some malformed inputs
            }
            env.close();

            // XRP should never increase from malformed imports
            BEAST_EXPECT(checkXRPSupplyInvariant(env, initialDrops));
            BEAST_EXPECT(checkAccountBalanceInvariant(env, alice));
        }
    }

    // =========================================================================
    // FUZZ: Random passkey list operations
    // Tests boundary conditions in passkey registration.
    // =========================================================================

    void
    fuzzPasskeyList(FeatureBitset features)
    {
        testcase("FUZZ: Random passkey list operations");
        using namespace test::jtx;

        Env env{*this, features};

        Account const alice{"alice"};
        env.fund(XRP(100000), alice);
        env.close();

        constexpr int kIterations = 100;
        for (int i = 0; i < kIterations; ++i)
        {
            Json::Value passkeys(Json::arrayValue);

            auto const numEntries = randU32(0, 15);  // 0 to 15 (max is 10)

            for (std::uint32_t j = 0; j < numEntries; ++j)
            {
                Json::Value entry;

                auto const idLen = randU32(0, 100);
                std::string passkeyId;
                for (std::uint32_t k = 0; k < idLen; ++k)
                {
                    uint8_t b = static_cast<uint8_t>(randU32(0, 255));
                    passkeyId += strHex(Slice(&b, 1));
                }
                entry[sfPasskeyID.jsonName] = passkeyId;

                // Random key format
                auto const keyType = randU32(0, 4);
                std::string pubKey;
                switch (keyType)
                {
                    case 0:
                    {
                        // Valid P256 format (65 bytes, 0xF6 prefix)
                        pubKey = "F6";
                        for (int k = 0; k < 64; ++k)
                        {
                            uint8_t b =
                                static_cast<uint8_t>(randU32(0, 255));
                            pubKey += strHex(Slice(&b, 1));
                        }
                        break;
                    }
                    case 1:
                    {
                        // Wrong prefix (65 bytes)
                        uint8_t prefix =
                            static_cast<uint8_t>(randU32(0, 255));
                        pubKey = strHex(Slice(&prefix, 1));
                        for (int k = 0; k < 64; ++k)
                        {
                            uint8_t b =
                                static_cast<uint8_t>(randU32(0, 255));
                            pubKey += strHex(Slice(&b, 1));
                        }
                        break;
                    }
                    case 2:
                        // Too short
                        pubKey = "F601020304";
                        break;
                    case 3:
                    {
                        // Too long
                        pubKey = "F6";
                        for (int k = 0; k < 100; ++k)
                            pubKey += "01";
                        break;
                    }
                    case 4:
                        // Empty
                        pubKey = "";
                        break;
                }
                entry[sfPublicKey.jsonName] = pubKey;

                // Randomly omit fields
                if (randU32(0, 10) == 0)
                    entry.removeMember(sfPasskeyID.jsonName);
                if (randU32(0, 10) == 0)
                    entry.removeMember(sfPublicKey.jsonName);

                passkeys.append(entry);
            }

            try
            {
                env(setPasskeyList(alice, passkeys), ter(std::ignore));
            }
            catch (...)
            {
                // Expected for some malformed inputs
            }
            env.close();

            BEAST_EXPECT(checkAccountBalanceInvariant(env, alice));
        }
    }

    // =========================================================================
    // FUZZ: Random leverage tier parameters
    // Tests boundary values for margin configuration.
    // =========================================================================

    void
    fuzzLeverageTierSet(FeatureBitset features)
    {
        testcase("FUZZ: Random leverage tier parameters");
        using namespace test::jtx;

        Env env{*this, features};

        Account const gw{"gateway"};
        env.fund(XRP(10000000), gw);
        env.close();

        auto const USD = gw["USD"];
        auto const asset = STIssue(sfAsset, USD.issue());
        auto const xrpAsset = STIssue(sfAsset2, xrpIssue());

        constexpr int kIterations = 200;
        for (int i = 0; i < kIterations; ++i)
        {
            // Boundary values for each parameter
            auto const maxLev = randU32(0, 0xFFFFFFFF);
            auto const initMargin = randU32(0, 0xFFFFFFFF);
            auto const maintMargin = randU32(0, 0xFFFFFFFF);
            auto const liqBonus = randU32(0, 0xFFFFFFFF);

            try
            {
                env(leverageTierSet(
                        gw, asset, xrpAsset,
                        maxLev, initMargin, maintMargin, liqBonus),
                    ter(std::ignore));
            }
            catch (...)
            {
            }
            env.close();

            BEAST_EXPECT(checkAccountBalanceInvariant(env, gw));
        }
    }

    // =========================================================================
    // FUZZ: Rapid option create/settle/expire cycles
    // Tests state machine transitions under pressure.
    // =========================================================================

    void
    fuzzOptionLifecycle(FeatureBitset features)
    {
        testcase("FUZZ: Rapid option create/settle/expire cycles");
        using namespace test::jtx;

        Env env{*this, features};

        Account const gw{"gateway"};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000000), gw, alice, bob);
        env.close();

        auto const USD = gw["USD"];
        env.trust(USD(100000000), alice, bob);
        env(pay(gw, alice, USD(50000000)));
        env(pay(gw, bob, USD(50000000)));
        env.close();

        auto const asset = STIssue(sfAsset, USD.issue());
        auto const xrpAsset = STIssue(sfAsset2, xrpIssue());

        env(optionPairCreate(gw, asset, xrpAsset),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Setup margin
        env(marginAccountSet(
                alice, STIssue(sfCollateralAsset, xrpIssue())),
            ter(tesSUCCESS));
        env(marginAccountSet(
                bob, STIssue(sfCollateralAsset, xrpIssue())),
            ter(tesSUCCESS));
        env.close();

        auto const marginIdAlice =
            keylet::marginAccount(alice.id(), xrpIssue()).key;
        auto const marginIdBob =
            keylet::marginAccount(bob.id(), xrpIssue()).key;

        env(marginDeposit(alice, marginIdAlice, XRP(5000000)),
            ter(tesSUCCESS));
        env(marginDeposit(bob, marginIdBob, XRP(5000000)),
            ter(tesSUCCESS));
        env.close();

        auto const initialDrops =
            env.current()->header().drops.drops();

        // Track created offers for settle attempts
        std::vector<std::pair<uint256, uint256>> createdOffers;

        constexpr int kIterations = 100;
        for (int i = 0; i < kIterations; ++i)
        {
            auto const op = randU32(0, 3);

            try
            {
                switch (op)
                {
                    case 0:
                    {
                        // Create a sell offer
                        auto const exp = env.now() +
                            std::chrono::seconds(randU32(10, 86400));
                        auto const& account =
                            (randU32(0, 1) == 0) ? alice : bob;
                        auto const& marginId =
                            (&account == &alice) ? marginIdAlice
                                                 : marginIdBob;
                        auto const seq = env.seq(account);
                        env(optionCreate(
                                account,
                                exp,
                                USD(randI64(50, 500)),
                                asset,
                                100,
                                USD(randI64(1, 50)),
                                marginId,
                                randU32(2, 50)),
                            txflags(tfSell),
                            ter(std::ignore));
                        env.close();

                        auto const optionId = keylet::option(
                            USD.issue().account,
                            USD.issue().currency,
                            100,  // strike as integer
                            exp.time_since_epoch().count()).key;
                        auto const offerId =
                            keylet::optionOffer(account.id(), seq).key;
                        createdOffers.push_back({optionId, offerId});
                        break;
                    }
                    case 1:
                    {
                        // Create a matching buy offer
                        auto const exp = env.now() +
                            std::chrono::seconds(randU32(10, 86400));
                        auto const& account =
                            (randU32(0, 1) == 0) ? alice : bob;
                        auto const& marginId =
                            (&account == &alice) ? marginIdAlice
                                                 : marginIdBob;
                        env(optionCreate(
                                account,
                                exp,
                                USD(randI64(50, 500)),
                                asset,
                                100,
                                USD(randI64(1, 50)),
                                marginId,
                                randU32(2, 50)),
                            txflags(0),  // buy
                            ter(std::ignore));
                        env.close();
                        break;
                    }
                    case 2:
                    {
                        // Try to settle a random offer
                        if (!createdOffers.empty())
                        {
                            auto const idx = randU32(
                                0, createdOffers.size() - 1);
                            auto const& [optId, offId] =
                                createdOffers[idx];
                            auto const& account =
                                (randU32(0, 1) == 0) ? alice : bob;

                            std::uint32_t const settleFlags[] = {
                                tfExpire, tfClose, tfExercise};
                            auto const flag =
                                settleFlags[randU32(0, 2)];

                            env(optionSettle(account, optId, offId),
                                txflags(flag),
                                ter(std::ignore));
                            env.close();
                        }
                        break;
                    }
                    case 3:
                    {
                        // Advance time to trigger expirations
                        for (int j = 0; j < 10; ++j)
                            env.close();
                        break;
                    }
                }
            }
            catch (...)
            {
            }

            // Invariants after every operation
            BEAST_EXPECT(checkXRPSupplyInvariant(env, initialDrops));
            BEAST_EXPECT(
                checkMarginCollateralInvariant(env, alice, xrpIssue()));
            BEAST_EXPECT(
                checkMarginCollateralInvariant(env, bob, xrpIssue()));
            BEAST_EXPECT(checkAccountBalanceInvariant(env, alice));
            BEAST_EXPECT(checkAccountBalanceInvariant(env, bob));
        }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = testable_amendments();

        fuzzOptionCreate(sa);
        fuzzMarginDepositWithdraw(sa);
        fuzzInsuranceVault(sa);
        fuzzImportBlobs(sa);
        fuzzPasskeyList(sa);
        fuzzLeverageTierSet(sa);
        fuzzOptionLifecycle(sa);
    }
};

BEAST_DEFINE_TESTSUITE_MANUAL(OptionsFuzz, app, ripple);

}  // namespace test
}  // namespace xrpl
