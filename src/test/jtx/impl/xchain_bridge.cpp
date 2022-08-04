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

#include <ripple/json/json_value.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/xchain_bridge.h>

namespace ripple {
namespace test {
namespace jtx {

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
xchain_create_claim_id(
    Account const& acc,
    Json::Value const& bridge,
    STAmount const& reward,
    Account const& otherChainAccount)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainBridge.getJsonName()] = bridge;
    jv[sfSignatureReward.getJsonName()] = reward.getJson(JsonOptions::none);
    jv[sfOtherChainAccount.getJsonName()] = otherChainAccount.human();

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
        jv[sfOtherChainAccount.getJsonName()] = dst->human();

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

}  // namespace jtx
}  // namespace test
}  // namespace ripple
