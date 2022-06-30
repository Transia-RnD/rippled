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
#include "ripple/protocol/SField.h"
#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/multisign.h>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
bridge(
    Account const& lockingChainDoor,
    Issue const& lockingChainIssue,
    Account const& issuingChainDoor,
    Issue const& issuingChainIssue);

Json::Value
bridge_create(
    Account const& acc,
    Json::Value const& sidechain,
    STAmount const& reward,
    std::optional<STAmount> const& minAccountCreate = std::nullopt);

Json::Value
xchain_create_claim_id(
    Account const& acc,
    Json::Value const& sidechain,
    STAmount const& reward,
    Account const& otherChainAccount);

Json::Value
xchain_commit(
    Account const& acc,
    Json::Value const& sidechain,
    std::uint32_t xchainSeq,
    AnyAmount const& amt);

Json::Value
xchain_claim(
    Account const& acc,
    Json::Value const& claimProof,
    Account const& dst);

Json::Value
xchain_add_attestation_batch(Account const& acc, Json::Value const& batch);

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
