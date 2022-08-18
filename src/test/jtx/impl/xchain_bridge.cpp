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

#include <test/jtx/xchain_bridge.h>

#include <ripple/json/json_value.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>

namespace ripple {
namespace test {
namespace jtx {

// use this for creating a bridge for a transaction
Json::Value
bridge(
    Account const& lockingChainDoor,
    Issue const& lockingChainIssue,
    Account const& issuingChainDoor,
    Issue const& issuingChainIssue)
{
    Json::Value jv;
    jv[sfLockingChainDoor.getJsonName()] = lockingChainDoor.human();
    jv[sfLockingChainIssue.getJsonName()] = to_json(lockingChainIssue);
    jv[sfIssuingChainDoor.getJsonName()] = issuingChainDoor.human();
    jv[sfIssuingChainIssue.getJsonName()] = to_json(issuingChainIssue);
    return jv;
}

// use this for creating a bridge for a rpc query
Json::Value
bridge_rpc(
    Account const& lockingChainDoor,
    Issue const& lockingChainIssue,
    Account const& issuingChainDoor,
    Issue const& issuingChainIssue)
{
    Json::Value jv;
    jv[jss::locking_chain_door] = lockingChainDoor.human();
    jv[jss::locking_chain_issue] = to_json(lockingChainIssue);
    jv[jss::issuing_chain_door] = issuingChainDoor.human();
    jv[jss::issuing_chain_issue] = to_json(issuingChainIssue);
    return jv;
}

Json::Value
bridge_create(
    Account const& acc,
    Json::Value const& bridge,
    STAmount const& reward,
    std::optional<STAmount> const& minAccountCreate)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[sfSignatureReward.getJsonName()] = reward.getJson(JsonOptions::none);
    if (minAccountCreate)
        jv[sfMinAccountCreateAmount.getJsonName()] =
            minAccountCreate->getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::XChainCreateBridge;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
bridge_modify(
    Account const& acc,
    Json::Value const& bridge,
    std::optional<STAmount> const& reward,
    std::optional<STAmount> const& minAccountCreate)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    if (reward)
        jv[sfSignatureReward.getJsonName()] =
            reward->getJson(JsonOptions::none);
    if (minAccountCreate)
        jv[sfMinAccountCreateAmount.getJsonName()] =
            minAccountCreate->getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::XChainModifyBridge;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
xchain_create_claim_id(
    Account const& acc,
    Json::Value const& bridge,
    STAmount const& reward,
    Account const& otherChainSource)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[sfSignatureReward.getJsonName()] = reward.getJson(JsonOptions::none);
    jv[sfOtherChainSource.getJsonName()] = otherChainSource.human();

    jv[jss::TransactionType] = jss::XChainCreateClaimID;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
xchain_commit(
    Account const& acc,
    Json::Value const& bridge,
    std::uint32_t xchainSeq,
    AnyAmount const& amt,
    std::optional<Account> const& dst)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[sfXChainClaimID.getJsonName()] = xchainSeq;
    jv[jss::Amount] = amt.value.getJson(JsonOptions::none);
    if (dst)
        jv[sfOtherChainDestination.getJsonName()] = dst->human();

    jv[jss::TransactionType] = jss::XChainCommit;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
xchain_claim(
    Account const& acc,
    Json::Value const& bridge,
    std::uint32_t claimID,
    AnyAmount const& amt,
    Account const& dst)
{
    Json::Value jv;

    jv[sfAccount.getJsonName()] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[sfXChainClaimID.getJsonName()] = claimID;
    jv[sfDestination.getJsonName()] = dst.human();
    jv[sfAmount.getJsonName()] = amt.value.getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::XChainClaim;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_account_create(
    Account const& acc,
    Json::Value const& bridge,
    Account const& dst,
    AnyAmount const& amt,
    AnyAmount const& reward)
{
    Json::Value jv;

    jv[sfAccount.getJsonName()] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[sfDestination.getJsonName()] = dst.human();
    jv[sfAmount.getJsonName()] = amt.value.getJson(JsonOptions::none);
    jv[sfSignatureReward.getJsonName()] =
        reward.value.getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::SidechainXChainAccountCreate;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_account_claim(
    Account const& acc,
    Json::Value const& bridge,
    Account const& dst,
    AnyAmount const& amt)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[jss::Destination] = dst.human();
    jv[jss::Amount] = amt.value.getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::SidechainXChainAccountClaim;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
xchain_add_attestation_batch(Account const& acc, Json::Value const& batch)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainAttestationBatch.getJsonName()] = batch;

    jv[jss::TransactionType] = jss::XChainAddAttestation;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
attestation_claim_batch(
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
attestation_create_account_batch(
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
    , mcuDoor("mcuDoor")
    , mcuAlice("mcuAlice")
    , mcuBob("mcuBob")
    , mcuGw("mcuGw")
    , scuDoor("scuDoor")
    , scuAlice("scuAlice")
    , scuBob("scuBob")
    , scuGw("scuGw")
    , mcUSD(mcGw["USD"])
    , scUSD(scGw["USD"])
    , reward(XRP(1))
    , jvXRPBridgeRPC(
          bridge_rpc(mcDoor, xrpIssue(), Account::master, xrpIssue()))
    , jvb(bridge(mcDoor, xrpIssue(), Account::master, xrpIssue()))
    , jvub(bridge(mcuDoor, xrpIssue(), Account::master, xrpIssue()))
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
    , rewardAccountsScReward([&] {
        std::vector<Account> r;
        r.reserve(signers.size());
        for (int i = 0, e = signers.size(); i != e; ++i)
        {
            r.push_back(scReward);
        }
        return r;
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

    mcEnv(bridge_create(mcDoor, jvb, reward, minCreate));
    scEnv(bridge_create(scDoor, jvb, reward, minCreate));
    mcEnv.close();
    scEnv.close();
}
}  // namespace jtx
}  // namespace test
}  // namespace ripple
