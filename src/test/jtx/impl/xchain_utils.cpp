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
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/XChainAttestations.h>

#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>
#include <test/jtx/xchain_utils.h>

#include <optional>
#include <string>
#include <vector>

namespace ripple {
namespace test {
namespace jtx {

XChainBridgeObjects::XChainBridgeObjects()
    : mcDoor("mcDoor")
    , mcAlice("mcAlice")
    , mcBob("mcBob")
    , mcGw("mcGw")
    , scDoor("scDoor")
    , scAlice("scAlice")
    , scBob("scBob")
    , scGw("scGw")
    , scAttester("scAttester")
    , scReward("scReward")
    , mcUSD(mcGw["USD"])
    , scUSD(scGw["USD"])
    , reward(XRP(1))
    , jvXRPBridge(bridge(mcDoor, xrpIssue(), scDoor, xrpIssue()))
    , features(supported_amendments() | FeatureBitset{featureXChainBridge})
    , signers([] {
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
    }())
    , rewardAccountsMisc([&] {
        std::vector<Account> r;
        r.reserve(signers.size());
        for (int i = 0, e = signers.size(); i != e; ++i)
        {
            using namespace std::literals;
            auto const a = Account("reward_"s + std::to_string(i));
            r.push_back(a);
        }
        return r;
    }())
    , rewardAccountsScReward([&] {
        std::vector<Account> r;
        r.reserve(signers.size());
        for (int i = 0, e = signers.size(); i != e; ++i)
        {
            r.push_back(scReward);
        }
        return r;
    }())
    , quorum(static_cast<std::uint32_t>(signers.size()) - 1)
{
}

void
XChainBridgeObjects::createBridgeObjects(Env& mcEnv, Env& scEnv)
{
    PrettyAmount xrp_funds{XRP(10000)};
    mcEnv.fund(xrp_funds, mcDoor, mcAlice, mcBob, mcGw);
    scEnv.fund(xrp_funds, scDoor, scAlice, scBob, scGw, scAttester, scReward);

    // Signer's list must match the attestation signers
    mcEnv(jtx::signers(mcDoor, signers.size(), signers));
    scEnv(jtx::signers(scDoor, signers.size(), signers));

    // create XRP bridges in both direction
    auto const reward = XRP(1);
    STAmount const minCreate = XRP(20);

    mcEnv(bridge_create(mcDoor, jvXRPBridge, reward, minCreate));
    scEnv(bridge_create(scDoor, jvXRPBridge, reward, minCreate));
    mcEnv.close();
    scEnv.close();
}

Json::Value
XChainBridgeObjects::attestationClaimBatch(
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
XChainBridgeObjects::attestationCreateAccountBatch(
    Json::Value const& jvBridge,
    jtx::Account const& sendingAccount,
    jtx::AnyAmount const& sendingAmount,
    jtx::AnyAmount const& rewardAmount,
    std::vector<jtx::Account> const& rewardAccounts,
    bool wasLockingChainSend,
    std::uint64_t createCount,
    jtx::Account const& dst,
    std::vector<jtx::signer> const& signers,
    size_t num_signers /* = 0 */)
{
    assert(rewardAccounts.size() == signers.size());
    if (num_signers == 0)
        num_signers = signers.size();

    STXChainBridge const stBridge(jvBridge);
    std::vector<AttestationBatch::AttestationCreateAccount> atts;
    atts.reserve(num_signers);

    for (int i = 0; i != num_signers; ++i)
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

} // namespace jtx
} // namespace test
} // namespace ripple
