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

#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STSidechain.h>
#include <ripple/protocol/TER.h>
#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>
#include <test/jtx/multisign.h>
#include <test/jtx/sidechain.h>

#include <string>
#include <vector>

namespace ripple {
namespace test {

struct Sidechain_test : public beast::unit_test::suite
{
    void
    testSidechainCreate()
    {
        testcase("Sidechain Create");

        using namespace jtx;
        auto const features =
            supported_amendments() | FeatureBitset{featureSidechains};
        auto const mcDoor = Account("mcDoor");
        auto const scDoor = Account("scDoor");
        auto const alice = Account("alice");
        auto const mcGw = Account("mcGw");
        auto const scGw = Account("scGw");
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

        {
            // Simple create
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            std::uint32_t const quorum = signers.size() - 1;
            env(sidechain_create(
                mcDoor,
                sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                quorum,
                signers));
        }
        {
            // Sidechain must be owned by one of the door accounts
            Env env(*this, features);
            env.fund(XRP(10000), alice, mcDoor);
            std::uint32_t const quorum = signers.size() - 1;
            env(sidechain_create(
                    alice,
                    sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    quorum,
                    signers),
                ter(temSIDECHAIN_NONDOOR_OWNER));
        }
        for (auto const mcIsXRP : {true, false})
        {
            for (auto const scIsXRP : {true, false})
            {
                Env env(*this, features);
                env.fund(XRP(10000), alice, mcDoor, mcGw);
                // issue must be both xrp or both iou
                TER const expectedTer = (mcIsXRP != scIsXRP)
                    ? TER{temSIDECHAIN_BAD_ISSUES}
                    : TER{tesSUCCESS};

                Issue const mcIssue = mcIsXRP ? xrpIssue() : mcUSD;
                Issue const scIssue = scIsXRP ? xrpIssue() : scUSD;

                std::uint32_t const quorum = signers.size() - 1;
                env(sidechain_create(
                        mcDoor,
                        sidechain(mcDoor, mcIssue, scDoor, scIssue),
                        quorum,
                        signers),
                    ter(expectedTer));
            }
        }

        {
            // cannot have the same door account on both chains
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            std::uint32_t const quorum = signers.size() - 1;
            env(sidechain_create(
                    mcDoor,
                    sidechain(mcDoor, xrpIssue(), mcDoor, xrpIssue()),
                    quorum,
                    signers),
                ter(temEQUAL_DOOR_ACCOUNTS));
        }

        {
            // quorum must be positive
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            std::uint32_t const quorum = 0;
            env(sidechain_create(
                    mcDoor,
                    sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    quorum,
                    signers),
                ter(temBAD_QUORUM));
        }

        for (int delta = -2; delta <= 2; ++delta)
        {
            // quorum must be specifiable
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            // quorum must be less than or equal to the sum of the signing
            // weights
            TER const expectedTer =
                (delta > 0) ? TER{temBAD_QUORUM} : TER{tesSUCCESS};
            std::uint32_t const quorum = signers.size() + delta;
            env(sidechain_create(
                    mcDoor,
                    sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    quorum,
                    signers),
                ter(expectedTer));
        }

        {
            // check that all the signing weights are positive
            std::vector<signer> zeroWeightSigner{signers};
            zeroWeightSigner[signers.size() / 2].weight = 0;
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            std::uint32_t const quorum = 1;
            env(sidechain_create(
                    mcDoor,
                    sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    quorum,
                    zeroWeightSigner),
                ter(temBAD_WEIGHT));
        }

        {
            // can't create the same sidechain twice, but can create different
            // sidechains on the same account.
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor, mcGw);
            std::uint32_t const quorum = signers.size() - 1;
            env(sidechain_create(
                mcDoor,
                sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                quorum,
                signers));

            // Can't create the same sidechain twice
            env(sidechain_create(
                    mcDoor,
                    sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue()),
                    quorum,
                    signers),
                ter(tecDUPLICATE));

            // But can create a different sidechain on the same account
            env(sidechain_create(
                mcDoor,
                sidechain(mcDoor, mcUSD, scDoor, scUSD),
                quorum,
                signers));
        }

        {
            // check that issuer for this chain exists on this chain
            Env env(*this, features);
            env.fund(XRP(10000), mcDoor);
            std::uint32_t const quorum = signers.size() - 1;
            // Issuer doesn't exist. Should fail.
            env(sidechain_create(
                    mcDoor,
                    sidechain(mcDoor, mcUSD, scDoor, scUSD),
                    quorum,
                    signers),
                ter(tecNO_ISSUER));
            env.close();
            env.fund(XRP(10000), mcGw);
            env.close();
            // Issuer now exists. Should succeed.
            env(sidechain_create(
                mcDoor,
                sidechain(mcDoor, mcUSD, scDoor, scUSD),
                quorum,
                signers));
        }

        {
            // TODO
            // check reserves
        }
    }

    Json::Value
    collectClaimProof(
        Json::Value const& jvSidechain,
        jtx::AnyAmount const& amt,
        std::uint32_t xchainSeq,
        bool wasSrcSend,
        std::vector<jtx::signer> const& signers)
    {
        STSidechain const stsc = STSidechainFromJson(sfSidechain, jvSidechain);
        std::vector<std::pair<PublicKey, Buffer>> sigs;
        sigs.reserve(signers.size());

        for (auto const& s : signers)
        {
            auto const& pk = s.account.pk();
            auto const& sk = s.account.sk();
            auto const sig = jtx::sign_attestation(
                pk, sk, stsc, amt.value, xchainSeq, wasSrcSend);
            sigs.emplace_back(pk, std::move(sig));
        }

        return sidechain_claim_proof(
            jvSidechain, amt, xchainSeq, wasSrcSend, sigs);
    }

    void
    testXChainTxn()
    {
        testcase("Sidechain XChain Txn");

        using namespace jtx;
        auto const features =
            supported_amendments() | FeatureBitset{featureSidechains};
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

        {
            // Simple xchain txn
            Env mcEnv(*this, features);
            Env scEnv(*this, envconfig(port_increment, 3), features);
            mcEnv.fund(XRP(10000), mcDoor, mcAlice);
            scEnv.fund(XRP(10000), scDoor, scAlice, scBob);
            std::uint32_t const quorum = signers.size() - 1;
            auto const sidechainSpec =
                sidechain(mcDoor, xrpIssue(), scDoor, xrpIssue());
            mcEnv(sidechain_create(mcDoor, sidechainSpec, quorum, signers));
            scEnv(sidechain_create(scDoor, sidechainSpec, quorum, signers));
            mcEnv.close();
            scEnv.close();

            // Alice creates xchain sequence number on the sidechain
            // Initiate a cross chain transaction from alice on the mainchain
            // Collect signatures from the attesters
            // Alice claims the XRP on the sidechain and sends it to bob

            scEnv(sidechain_xchain_seq_num_create(scAlice, sidechainSpec));
            scEnv.close();
            // TODO: Get the sequence number from metadata
            // RPC command to get owned sequence numbers?
            std::uint32_t const xchainSeq = 1;
            auto const amt = XRP(1000);
            mcEnv(sidechain_xchain_transfer(
                mcAlice, sidechainSpec, xchainSeq, amt));
            mcEnv.close();
            Json::Value claimProof = collectClaimProof(
                sidechainSpec, amt, xchainSeq, /*wasSrcSend*/ true, signers);
            scEnv(sidechain_xchain_claim(scAlice, claimProof, scBob));
            scEnv.close();
        };
    }

    void
    run() override
    {
        testSidechainCreate();
        testXChainTxn();
    }
};

BEAST_DEFINE_TESTSUITE(Sidechain, app, ripple);

}  // namespace test
}  // namespace ripple
