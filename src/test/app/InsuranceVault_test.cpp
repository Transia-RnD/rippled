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

struct InsuranceVault_test : public beast::unit_test::suite
{
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

    void
    testCreateInsuranceVault(FeatureBitset features)
    {
        testcase("Create Insurance Vault");

        using namespace test::jtx;
        Env env{*this, features};

        auto const gw = Account("gateway");
        auto const GME = gw["GME"];
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw);
        env.close();

        // Create OptionPair first (prerequisite)
        env(optionPairCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Create insurance vault for the pair
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

    void
    testDuplicateVaultRejected(FeatureBitset features)
    {
        testcase("Duplicate Insurance Vault Rejected");

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

        // First vault creation succeeds
        env(insuranceVaultCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tesSUCCESS));
        env.close();

        // Second vault creation for same pair should fail
        env(insuranceVaultCreate(
                gw,
                STIssue(sfAsset, GME.issue()),
                STIssue(sfAsset2, USD.issue())),
            fee(env.current()->fees().increment),
            ter(tecDUPLICATE));
        env.close();
    }

    void
    testDepositToVault(FeatureBitset features)
    {
        testcase("Deposit to Insurance Vault");

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

        // Create OptionPair + InsuranceVault
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

        // Deposit 1000 USD
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

    void
    testWithdrawFromVault(FeatureBitset features)
    {
        testcase("Withdraw from Insurance Vault");

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

        // Deposit first
        env(insuranceDeposit(alice, vaultID, USD(1000)),
            ter(tesSUCCESS));
        env.close();

        // Withdraw 500 USD
        env(insuranceWithdraw(alice, vaultID, USD(500)),
            ter(tesSUCCESS));
        env.close();
    }

    void
    testWithdrawExceedsBalance(FeatureBitset features)
    {
        testcase("Withdraw Exceeds Insurance Balance");

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

        // Deposit 100
        env(insuranceDeposit(alice, vaultID, USD(100)),
            ter(tesSUCCESS));
        env.close();

        // Try to withdraw 500 (more than deposited) → should fail
        env(insuranceWithdraw(alice, vaultID, USD(500)),
            ter(tecUNFUNDED_PAYMENT));
        env.close();
    }

    void
    testDepositNonexistentVault(FeatureBitset features)
    {
        testcase("Deposit to Nonexistent Vault");

        using namespace test::jtx;
        Env env{*this, features};

        auto const alice = Account("alice");
        auto const gw = Account("gateway");
        auto const USD = gw["USD"];

        env.fund(XRP(100000), gw, alice);
        env.close();

        env.trust(USD(100000), alice);
        env.close();

        env(pay(gw, alice, USD(10000)));
        env.close();

        // Try to deposit to a vault that doesn't exist
        uint256 const fakeVaultID(5678);
        env(insuranceDeposit(alice, fakeVaultID, USD(100)),
            ter(tecNO_ENTRY));
        env.close();
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{testable_amendments()};

        testCreateInsuranceVault(all);
        testDuplicateVaultRejected(all);
        testDepositToVault(all);
        testWithdrawFromVault(all);
        testWithdrawExceedsBalance(all);
        testDepositNonexistentVault(all);
    }
};

BEAST_DEFINE_TESTSUITE(InsuranceVault, app, ripple);

}  // namespace test
}  // namespace xrpl
