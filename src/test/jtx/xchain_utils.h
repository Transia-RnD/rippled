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

#ifndef RIPPLE_TEST_JTX_XCHAINUTILS_H_INCLUDED
#define RIPPLE_TEST_JTX_XCHAINUTILS_H_INCLUDED

#include <test/jtx/xchain_bridge.h>

namespace ripple {
namespace test {
namespace jtx {

struct XChainBridgeObjects
{
    const FeatureBitset features;

    const Account mcDoor;
    const Account mcAlice;
    const Account mcBob;
    const Account mcGw;
    const Account scDoor;
    const Account scAlice;
    const Account scBob;
    const Account scGw;
    const Account scAttester;
    const Account scReward;

    const IOU mcUSD;
    const IOU scUSD;

    const PrettyAmount reward;

    const Json::Value jvXRPBridge;

    const std::vector<signer> signers;
    const std::vector<Account> rewardAccountsScReward;
    const std::vector<Account> rewardAccountsMisc;
    const std::uint32_t quorum;

    static constexpr int drop_per_xrp = 1000000;

    XChainBridgeObjects();

    void
    createBridgeObjects(Env& mcEnv, Env& scEnv);

    Json::Value
    attestationClaimBatch(
        Json::Value const& jvBridge,
        jtx::Account const& sendingAccount,
        jtx::AnyAmount const& sendingAmount,
        std::vector<jtx::Account> const& rewardAccounts,
        bool wasLockingChainSend,
        std::uint64_t claimID,
        std::optional<jtx::Account> const& dst,
        std::vector<jtx::signer> const& signers);

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
        std::vector<jtx::signer> const& signers,
        size_t num_signers = 0);
};

} // namespace jtx
} // namespace test
} // namespace ripple

#endif // RIPPLE_TEST_JTX_XCHAINUTILS_H_INCLUDED
