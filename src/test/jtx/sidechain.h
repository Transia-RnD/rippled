//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_TEST_JTX_SIDECHAIN_H_INCLUDED
#define RIPPLE_TEST_JTX_SIDECHAIN_H_INCLUDED

#include <ripple/json/json_value.h>
#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/multisign.h>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
sidechain(
    Account const& srcChainDoor,
    Issue const& sideChainIssue,
    Account const& dstChainDoor,
    Issue const& dstChainIssue);

Json::Value
sidechain_claim_proof(
    Json::Value const& sidechain,
    AnyAmount const& amt,
    std::uint32_t xchainSeq,
    std::vector<std::pair<PublicKey, Buffer>> const& sigs);

Json::Value
sidechain_create(
    Account const& acc,
    Json::Value const& sidechain,
    std::uint32_t quorum,
    std::vector<signer> const& v);

Json::Value
sidechain_xchain_seq_num_create(
    Account const& acc,
    Json::Value const& sidechain);

Json::Value
sidechain_xchain_transfer(
    Account const& acc,
    Json::Value const& sidechain,
    std::uint32_t xchainSeq,
    AnyAmount const& amt);

Json::Value
sidechain_xchain_claim(
    Account const& acc,
    Json::Value const& claimProof,
    Account const& dst);

Json::Value
sidechain_xchain_account_create(
    Account const& acc,
    Json::Value const& sidechain,
    Account const& dst,
    AnyAmount const& amt,
    AnyAmount const& xChainFee);

Json::Value
sidechain_xchain_account_claim(
    Account const& acc,
    Json::Value const& sidechain,
    Account const& dst,
    AnyAmount const& amt);

}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif
