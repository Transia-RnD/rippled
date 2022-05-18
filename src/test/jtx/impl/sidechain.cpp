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

#include "ripple/protocol/SField.h"
#include <test/jtx/sidechain.h>

#include <ripple/protocol/Issue.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
sidechain(
    Account const& srcChainDoor,
    Issue const& srcChainIssue,
    Account const& dstChainDoor,
    Issue const& dstChainIssue)
{
    Json::Value jv;
    jv[jss::src_chain_door] = srcChainDoor.human();
    jv[jss::src_chain_issue] = to_json(srcChainIssue);
    jv[jss::dst_chain_door] = dstChainDoor.human();
    jv[jss::dst_chain_issue] = to_json(dstChainIssue);
    return jv;
}

Json::Value
sidechain_claim_proof(
    Json::Value const& sidechain,
    AnyAmount const& amt,
    std::uint32_t xchainSeq,
    std::vector<std::pair<PublicKey, Buffer>> const& sigs)
{
    Json::Value jv;
    jv[jss::sidechain] = sidechain;
    jv[jss::amount] = amt.value.getJson(JsonOptions::none);
    jv[jss::xchain_seq] = xchainSeq;
    Json::Value signers{Json::arrayValue};
    for (auto const& [pk, sig] : sigs)
    {
        Json::Value s;
        // TODO: Do we want to use AccountPublic for token type?
        s[jss::signing_key] = toBase58(TokenType::AccountPublic, pk);
        s[jss::signature] = strHex(sig);
        signers.append(s);
    }
    jv[jss::signatures] = signers;
    return jv;
}

Json::Value
sidechain_create(
    Account const& acc,
    Json::Value const& sidechain,
    std::uint32_t quorum,
    std::vector<signer> const& v)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfSidechain.getJsonName()] = sidechain;
    jv[sfSignerQuorum.getJsonName()] = quorum;

    // TODO: Don't have the extra object name in the array
    //       and store public keys, not account addresses
    auto& ja = jv[sfSignerEntries.getJsonName()];
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        auto const& e = v[i];
        auto& je = ja[i][sfSignerEntry.getJsonName()];
        je[jss::Account] = e.account.human();
        je[sfSignerWeight.getJsonName()] = e.weight;
    }

    jv[jss::TransactionType] = jss::SidechainCreate;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_seq_num_create(
    Account const& acc,
    Json::Value const& sidechain)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfSidechain.getJsonName()] = sidechain;

    jv[jss::TransactionType] = jss::SidechainXChainSeqNumCreate;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_transfer(
    Account const& acc,
    Json::Value const& sidechain,
    std::uint32_t xchainSeq,
    AnyAmount const& amt)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfSidechain.getJsonName()] = sidechain;
    jv[sfXChainSequence.getJsonName()] = xchainSeq;
    jv[jss::Amount] = amt.value.getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::SidechainXChainTransfer;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_claim(
    Account const& acc,
    Json::Value const& claimProof,
    Account const& dst)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfXChainClaimProof.getJsonName()] = claimProof;
    jv[jss::Destination] = dst.human();

    jv[jss::TransactionType] = jss::SidechainXChainClaim;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_account_create(
    Account const& acc,
    Json::Value const& sidechain,
    Account const& dst,
    AnyAmount const& amt,
    AnyAmount const& xChainFee)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfSidechain.getJsonName()] = sidechain;
    jv[jss::Destination] = dst.human();
    jv[jss::Amount] = amt.value.getJson(JsonOptions::none);
    jv[sfXChainFee.getJsonName()] = xChainFee.value.getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::SidechainXChainAccountCreate;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

Json::Value
sidechain_xchain_account_claim(
    Account const& acc,
    Json::Value const& sidechain,
    Account const& dst,
    AnyAmount const& amt)
{
    Json::Value jv;

    jv[jss::Account] = acc.human();
    jv[sfSidechain.getJsonName()] = sidechain;
    jv[jss::Destination] = dst.human();
    jv[jss::Amount] = amt.value.getJson(JsonOptions::none);

    jv[jss::TransactionType] = jss::SidechainXChainAccountClaim;
    jv[jss::Flags] = tfUniversal;
    return jv;
}

}  // namespace jtx
}  // namespace test
}  // namespace ripple
