//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2022 Ripple Labs Inc.

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

#include <ripple/beast/unit_test/suite.hpp>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/XChainAttestations.h>

#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>
#include <test/jtx/multisign.h>
#include <test/jtx/xchain_bridge.h>

#include <optional>
#include <string>
#include <vector>

namespace ripple {
namespace test {

struct XChainBridge_test : public beast::unit_test::suite
{
    void
    testBridgeCreate()
    {
        testcase("Bridge Create");

        using namespace jtx;
        auto const features =
            supported_amendments() | FeatureBitset{featureXChainBridge};
        auto const mcDoor = Account("mcDoor");
        auto const scDoor = Account("scDoor");
        auto const alice = Account("alice");
        auto const mcGw = Account("mcGw");
        auto const scGw = Account("scGw");
        auto const mcUSD = mcGw["USD"];
        auto const scUSD = scGw["USD"];

        for (auto withMinCreate : {true, false})
        {
            // Simple create
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;
            if (withMinCreate)
                minCreate.emplace(XRP(5));

            env(bridge_create(
                mcDoor,
                bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                reward,
                minCreate));
        }
        {
            // Bridge must be owned by one of the door accounts
            Env env(*this, features);
            env.fund(XRP(10000), alice, mcDoor);
            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;
            env(bridge_create(
                    alice,
                    bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    reward,
                    minCreate),
                ter(temSIDECHAIN_NONDOOR_OWNER));
        }
        for (auto const mcIsXRP : {true, false})
        {
            for (auto const scIsXRP : {true, false})
            {
                Env env(*this, features);
                env.fund(XRP(10000), alice, mcDoor, mcGw);
                auto const reward = XRP(1);
                std::optional<STAmount> minCreate;
                // issue must be both xrp or both iou
                TER const expectedTer = (mcIsXRP != scIsXRP)
                    ? TER{temSIDECHAIN_BAD_ISSUES}
                    : TER{tesSUCCESS};

                Issue const mcIssue = mcIsXRP ? xrpIssue() : mcUSD;
                Issue const scIssue = scIsXRP ? xrpIssue() : scUSD;

                env(bridge_create(
                        mcDoor,
                        bridge(mcDoor, mcIssue, scDoor, scIssue),
                        reward,
                        minCreate),
                    ter(expectedTer));
            }
        }

        {
            // cannot have the same door account on both chains
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;
            env(bridge_create(
                    mcDoor,
                    bridge(mcDoor, xrpIssue(), mcDoor, xrpIssue()),
                    reward,
                    minCreate),
                ter(temEQUAL_DOOR_ACCOUNTS));
        }

        {
            // can't create the same sidechain twice, but can create different
            // sidechains on the same account.
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor, mcGw);
            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;
            env(bridge_create(
                mcDoor,
                bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                reward,
                minCreate));

            // Can't create the same sidechain twice
            env(bridge_create(
                    mcDoor,
                    bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    reward,
                    minCreate),
                ter(tecDUPLICATE));

            // But can create a different sidechain on the same account
            env(bridge_create(
                mcDoor,
                bridge(mcDoor, mcUSD, scDoor, scUSD),
                reward,
                minCreate));
        }

        {
            // check that issuer for this chain exists on this chain
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;
            // Issuer doesn't exist. Should fail.
            env(bridge_create(
                    mcDoor,
                    bridge(mcDoor, mcUSD, scDoor, scUSD),
                    reward,
                    minCreate),
                ter(tecNO_ISSUER));
            env.close();
            env.fund(XRP(10000), mcGw);
            env.close();
            // Issuer now exists. Should succeed.
            env(bridge_create(
                mcDoor,
                bridge(mcDoor, mcUSD, scDoor, scUSD),
                reward,
                minCreate));
        }
        {
            // Simple modify
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;

            env(bridge_create(
                mcDoor,
                bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                reward,
                minCreate));

            auto const newReward = XRP(2);
            auto const newMinCreate = XRP(10);
            env(bridge_modify(
                mcDoor,
                bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                newReward,
                newMinCreate));
        }
        {
            // TODO
            // check reserves
        }
    }

    Json::Value
    attestationClaimBatch(
        Json::Value const& jvBridge,
        jtx::Account const& sendingAccount,
        jtx::AnyAmount const& sendingAmount,
        std::vector<jtx::Account> const& rewardAccounts,
        bool wasLockingChainSend,
        std::uint64_t claimID,
        std::optional<jtx::Account> const& dst,
        std::vector<jtx::signer> const& signers)
    {
        assert(rewardAccounts.size() == signers.size());

        STXChainBridge const stBridge(jvBridge);
        std::vector<AttestationBatch::AttestationClaim> claims;
        claims.reserve(signers.size());

        for (int i = 0, e = signers.size(); i != e; ++i)
        {
            auto const& s = signers[i];
            auto const& pk = s.account.pk();
            auto const& sk = s.account.sk();
            auto const sig = jtx::sign_claim_attestation(
                pk,
                sk,
                stBridge,
                sendingAccount,
                sendingAmount.value,
                rewardAccounts[i],
                wasLockingChainSend,
                claimID,
                dst);

            claims.emplace_back(
                pk,
                std::move(sig),
                sendingAccount.id(),
                sendingAmount.value,
                rewardAccounts[i].id(),
                wasLockingChainSend,
                claimID,
                dst ? std::optional{dst->id()} : std::nullopt);
        }

        STXChainAttestationBatch batch{stBridge, claims.begin(), claims.end()};

        return batch.getJson(JsonOptions::none);
    }

    Json::Value
    attestationCreateAccountBatch(
        Json::Value const& jvBridge,
        jtx::Account const& sendingAccount,
        jtx::AnyAmount const& sendingAmount,
        jtx::AnyAmount const& rewardAmount,
        std::vector<jtx::Account> const& rewardAccounts,
        bool wasLockingChainSend,
        std::uint64_t createCount,
        jtx::Account const& dst,
        std::vector<jtx::signer> const& signers)
    {
        assert(rewardAccounts.size() == signers.size());

        STXChainBridge const stBridge(jvBridge);
        std::vector<AttestationBatch::AttestationCreateAccount> atts;
        atts.reserve(signers.size());

        for (int i = 0, e = signers.size(); i != e; ++i)
        {
            auto const& s = signers[i];
            auto const& pk = s.account.pk();
            auto const& sk = s.account.sk();
            auto const sig = jtx::sign_create_account_attestation(
                pk,
                sk,
                stBridge,
                sendingAccount,
                sendingAmount.value,
                rewardAmount.value,
                rewardAccounts[i],
                wasLockingChainSend,
                createCount,
                dst);

            atts.emplace_back(
                pk,
                std::move(sig),
                sendingAccount.id(),
                sendingAmount.value,
                rewardAmount.value,
                rewardAccounts[i].id(),
                wasLockingChainSend,
                createCount,
                dst);
        }

        AttestationBatch::AttestationClaim* nullClaimRange = nullptr;
        STXChainAttestationBatch batch{
            stBridge, nullClaimRange, nullClaimRange, atts.begin(), atts.end()};

        return batch.getJson(JsonOptions::none);
    }

    void
    testSerializers()
    {
        testcase("XChain serializers");

        using namespace jtx;
        auto const mcDoor = Account("mcDoor");
        auto const mcAlice = Account("mcAlice");
        auto const mcBob = Account("mcBob");
        auto const mcGw = Account("mcGw");
        auto const scDoor = Account("scDoor");
        auto const scAlice = Account("scAlice");
        auto const scBob = Account("scBob");
        auto const scGw = Account("scGw");
        auto const mcUSD = mcGw["USD"];
        auto const scUSD = scGw["USD"];

        // Simple xchain txn
        std::vector<signer> const signers = [] {
            constexpr int numSigners = 5;
            std::vector<signer> result;
            result.reserve(numSigners);
            for (int i = 0; i < numSigners; ++i)
            {
                using namespace std::literals;
                auto const a = Account("signer_"s + std::to_string(i));
                result.emplace_back(a);
            }
            return result;
        }();

        auto const reward = XRP(1);
        std::optional<STAmount> minCreate;
        auto const bridgeSpec = bridge(mcDoor, xrpIssue(), scDoor, xrpIssue());
        std::uint32_t const chaimID = 1;
        auto const amt = XRP(1000);
        std::vector<Account> const rewardAccounts = [&] {
            std::vector<Account> r;
            r.reserve(signers.size());
            for (int i = 0, e = signers.size(); i != e; ++i)
            {
                r.push_back(scDoor);
            }
            return r;
        }();
        std::optional<Account> dst{scBob};
        Json::Value batch = attestationClaimBatch(
            bridgeSpec,
            mcAlice,
            amt,
            rewardAccounts,
            /*wasLockingChainSend*/ true,
            chaimID,
            dst,
            signers);
        {
            Serializer s;
            STXChainAttestationBatch org{sfXChainAttestationBatch, batch};
            org.add(s);
            SerialIter si{s.data(), s.size()};
            STXChainAttestationBatch read{si, sfXChainAttestationBatch};
            BEAST_EXPECT(org == read);
        }
        {
            Serializer s;
            STXChainBridge org{sfXChainBridge, bridgeSpec};
            org.add(s);
            SerialIter si{s.data(), s.size()};
            STXChainBridge read{si, sfXChainBridge};
            BEAST_EXPECT(org == read);
        }
        {
            Serializer s;
            XChainClaimAttestations const attestations = [&] {
                std::vector<XChainClaimAttestation> toAdd;
                STXChainAttestationBatch b{sfXChainAttestationBatch, batch};
                for (auto const& c : b.claims())
                    toAdd.emplace_back(c);
                return XChainClaimAttestations{std::move(toAdd)};
            }();
            STObject org{sfXChainClaimID};
            org[sfAccount] = STAccount{sfAccount, mcAlice.id()};
            org[sfXChainBridge] = STXChainBridge{sfXChainBridge, bridgeSpec};
            org.setFieldArray(
                sfXChainClaimAttestations, attestations.toSTArray());
            org.add(s);
            SerialIter si{s.data(), s.size()};
            STObject read{si, sfXChainClaimID};
            BEAST_EXPECT(org == read);
        }
    }

    void
    testXChainTxn()
    {
        testcase("Bridge XChain Txn");

        using namespace jtx;
        auto const features =
            supported_amendments() | FeatureBitset{featureXChainBridge};
        auto const mcDoor = Account("mcDoor");
        auto const mcAlice = Account("mcAlice");
        auto const mcBob = Account("mcBob");
        auto const mcGw = Account("mcGw");
        auto const scDoor = Account("scDoor");
        auto const scAlice = Account("scAlice");
        auto const scBob = Account("scBob");
        auto const scGw = Account("scGw");
        auto const scReward = Account("scReward");
        auto const mcUSD = mcGw["USD"];
        auto const scUSD = scGw["USD"];

        std::vector<signer> const signers = [] {
            constexpr int numSigners = 5;
            std::vector<signer> result;
            result.reserve(numSigners);
            for (int i = 0; i < numSigners; ++i)
            {
                using namespace std::literals;
                auto const a = Account("signer_"s + std::to_string(i));
                result.emplace_back(a);
            }
            return result;
        }();

        for (auto withClaim : {false, true})
        {
            Env mcEnv(*this, features);
            Env scEnv(*this, envconfig(port_increment, 3), features);
            mcEnv.fund(XRP(10000), mcDoor, mcAlice);
            scEnv.fund(XRP(10000), scDoor, scAlice, scBob, scReward);

            // Signer's list must match the attestation signers
            mcEnv(jtx::signers(mcDoor, signers.size(), signers));
            scEnv(jtx::signers(scDoor, signers.size(), signers));

            auto const reward = XRP(1);
            std::optional<STAmount> minCreate;
            auto const bridgeSpec =
                bridge(mcDoor, xrpIssue(), scDoor, xrpIssue());
            mcEnv(bridge_create(mcDoor, bridgeSpec, reward, minCreate));
            scEnv(bridge_create(scDoor, bridgeSpec, reward, minCreate));
            mcEnv.close();
            scEnv.close();

            // Alice creates xchain sequence number on the sidechain
            // Initiate a cross chain transaction from alice on the mainchain
            // Collect signatures from the attesters
            // Alice claims the XRP on the sidechain and sends it to bob

            scEnv(xchain_create_claim_id(scAlice, bridgeSpec, reward, mcAlice));
            scEnv.close();
            // TODO: Get the sequence number from metadata
            // RPC command to get owned sequence numbers?
            std::uint32_t const claimID = 1;
            auto const amt = XRP(1000);

            std::optional<Account> dst;
            if (!withClaim)
                dst.emplace(scBob);

            mcEnv(xchain_commit(mcAlice, bridgeSpec, claimID, amt, dst));
            mcEnv.close();
            // TODO: reward accounts
            std::vector<Account> const rewardAccounts = [&] {
                std::vector<Account> r;
                r.reserve(signers.size());
                for (int i = 0, e = signers.size(); i != e; ++i)
                {
                    // TODO: Specify different reward accounts so can check
                    r.push_back(scReward);
                }
                return r;
            }();

            auto const bobPre = scEnv.balance(scBob);
            auto const doorPre = scEnv.balance(scDoor);
            auto const rewardPre = scEnv.balance(scReward);

            Json::Value batch = attestationClaimBatch(
                bridgeSpec,
                mcAlice,
                amt,
                rewardAccounts,
                /*wasLockingChainSend*/ true,
                claimID,
                dst,
                signers);

            scEnv(xchain_add_attestation_batch(scAlice, batch));
            scEnv.close();

            if (withClaim)
            {
                auto const bobPost = scEnv.balance(scBob);
                auto const doorPost = scEnv.balance(scDoor);
                auto const rewardPost = scEnv.balance(scReward);
                BEAST_EXPECT(bobPost == bobPre);
                BEAST_EXPECT(doorPre == doorPost);
                BEAST_EXPECT(rewardPost == rewardPre);

                // need to submit a claim transactions
                scEnv(xchain_claim(scAlice, bridgeSpec, claimID, amt, scBob));
                scEnv.close();
            }

            auto const bobPost = scEnv.balance(scBob);
            auto const doorPost = scEnv.balance(scDoor);
            auto const rewardPost = scEnv.balance(scReward);
            BEAST_EXPECT(bobPost - bobPre == amt);
            BEAST_EXPECT(doorPre - doorPost == amt);
            BEAST_EXPECT(rewardPost - rewardPre == reward);
        };
    }

    void
    testXChainCreateAccount()
    {
        testcase("Bridge XChain Create Account");

        using namespace jtx;
        auto const features =
            supported_amendments() | FeatureBitset{featureXChainBridge};
        auto const mcDoor = Account("mcDoor");
        auto const mcAlice = Account("mcAlice");
        auto const mcBob = Account("mcBob");
        auto const mcGw = Account("mcGw");
        auto const scDoor = Account("scDoor");
        auto const scAlice = Account("scAlice");
        auto const scBob = Account("scBob");
        auto const scGw = Account("scGw");
        auto const scAttester = Account("scAttester");
        auto const scReward = Account("scReward");
        auto const mcUSD = mcGw["USD"];
        auto const scUSD = scGw["USD"];

        std::vector<signer> const signers = [] {
            constexpr int numSigners = 5;
            std::vector<signer> result;
            result.reserve(numSigners);
            for (int i = 0; i < numSigners; ++i)
            {
                using namespace std::literals;
                auto const a = Account("signer_"s + std::to_string(i));
                result.emplace_back(a);
            }
            return result;
        }();

        Env mcEnv(*this, features);
        Env scEnv(*this, envconfig(port_increment, 3), features);
        mcEnv.fund(XRP(10000), mcDoor, mcAlice);
        // Don't fund scBob - it will be created with the xchain transaction
        scEnv.fund(XRP(10000), scDoor, scAlice, scAttester, scReward);

        // Signer's list must match the attestation signers
        mcEnv(jtx::signers(mcDoor, signers.size(), signers));
        scEnv(jtx::signers(scDoor, signers.size(), signers));

        auto const reward = XRP(1);
        STAmount const minCreate = XRP(20);
        auto const bridgeSpec = bridge(mcDoor, xrpIssue(), scDoor, xrpIssue());
        mcEnv(bridge_create(mcDoor, bridgeSpec, reward, minCreate));
        scEnv(bridge_create(scDoor, bridgeSpec, reward, minCreate));
        mcEnv.close();
        scEnv.close();

        // Alice initiates a xchain create transaction on the mainchain
        // Collect signatures from the attesters
        // The scAttester account submits the attestations
        // // the scReward account collects the reward
        // A new scBob account should be created

        auto const amt = XRP(1000);
        mcEnv(sidechain_xchain_account_create(
            mcAlice, bridgeSpec, scBob, amt, reward));
        mcEnv.close();
        // TODO: Get createCount from the ledger
        std::uint64_t const createCount = 1;
        // TODO: reward accounts
        std::vector<Account> const rewardAccounts = [&] {
            std::vector<Account> r;
            r.reserve(signers.size());
            for (int i = 0, e = signers.size(); i != e; ++i)
            {
                // TODO: Specify different reward accounts so can check
                r.push_back(scReward);
            }
            return r;
        }();
        Account dst{scBob};

        auto const bobPre = XRP(0);
        auto const doorPre = scEnv.balance(scDoor);
        auto const rewardPre = scEnv.balance(scReward);

        Json::Value batch = attestationCreateAccountBatch(
            bridgeSpec,
            mcAlice,
            amt,
            reward,
            rewardAccounts,
            /*wasLockingChainSend*/ true,
            createCount,
            dst,
            signers);

        scEnv(xchain_add_attestation_batch(scAttester, batch));
        scEnv.close();

        auto const bobPost = scEnv.balance(scBob);
        auto const doorPost = scEnv.balance(scDoor);
        auto const rewardPost = scEnv.balance(scReward);
        BEAST_EXPECT(bobPost - bobPre == amt);
        BEAST_EXPECT(doorPre - doorPost == amt + reward);
        BEAST_EXPECT(rewardPost - rewardPre == reward);
    }

    void
    run() override
    {
        testBridgeCreate();
        testSerializers();
        testXChainTxn();
        testXChainCreateAccount();
    }
};

BEAST_DEFINE_TESTSUITE(XChainBridge, app, ripple);

}  // namespace test
}  // namespace ripple
