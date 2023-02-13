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

#ifndef RIPPLE_PROTOCOL_STXATTESTATION_BATCH_H_INCLUDED
#define RIPPLE_PROTOCOL_STXATTESTATION_BATCH_H_INCLUDED

#include <ripple/basics/Buffer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>

#include <type_traits>
#include <vector>

namespace ripple {

namespace Attestations {

struct AttestationBase
{
    // Public key from the witness server attesting to the event
    PublicKey publicKey;
    // Signature from the witness server attesting to the event
    Buffer signature;
    // Account on the sending chain that triggered the event (sent the
    // transaction)
    AccountID sendingAccount;
    // Amount transfered on the sending chain
    STAmount sendingAmount;
    // Account on the destination chain that collects a share of the attestation
    // reward
    AccountID rewardAccount;
    // Amount was transfered on the locking chain
    bool wasLockingChainSend;

    explicit AttestationBase(
        PublicKey const& publicKey_,
        Buffer signature_,
        AccountID const& sendingAccount_,
        STAmount const& sendingAmount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_);

    AttestationBase(AttestationBase const&) = default;

    virtual ~AttestationBase() = default;

    AttestationBase&
    operator=(AttestationBase const&) = default;

    // verify that the signature attests to the data.
    bool
    verify(STXChainBridge const& bridge) const;

protected:
    explicit AttestationBase(STObject const& o);
    explicit AttestationBase(Json::Value const& v);

    [[nodiscard]] static bool
    equalHelper(AttestationBase const& lhs, AttestationBase const& rhs);

    [[nodiscard]] static bool
    sameEventHelper(AttestationBase const& lhs, AttestationBase const& rhs);

    void
    addHelper(STObject& o) const;

private:
    [[nodiscard]] virtual std::vector<std::uint8_t>
    message(STXChainBridge const& bridge) const = 0;
};

// Attest to a regular cross-chain transfer
struct AttestationClaim : AttestationBase
{
    std::uint64_t claimID;
    std::optional<AccountID> dst;

    explicit AttestationClaim(
        PublicKey const& publicKey_,
        Buffer signature_,
        AccountID const& sendingAccount_,
        STAmount const& sendingAmount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_,
        std::uint64_t claimID_,
        std::optional<AccountID> const& dst_);

    explicit AttestationClaim(
        STXChainBridge const& bridge,
        PublicKey const& publicKey_,
        SecretKey const& secretKey_,
        AccountID const& sendingAccount_,
        STAmount const& sendingAmount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_,
        std::uint64_t claimID_,
        std::optional<AccountID> const& dst_);

    explicit AttestationClaim(STObject const& o);
    explicit AttestationClaim(Json::Value const& v);

    [[nodiscard]] STObject
    toSTObject() const;

    // return true if the two attestations attest to the same thing
    [[nodiscard]] bool
    sameEvent(AttestationClaim const& rhs) const;

    [[nodiscard]] static std::vector<std::uint8_t>
    message(
        STXChainBridge const& bridge,
        AccountID const& sendingAccount,
        STAmount const& sendingAmount,
        AccountID const& rewardAccount,
        bool wasLockingChainSend,
        std::uint64_t claimID,
        std::optional<AccountID> const& dst);

    [[nodiscard]] bool
    validAmounts() const;

private:
    [[nodiscard]] std::vector<std::uint8_t>
    message(STXChainBridge const& bridge) const override;

    friend bool
    operator==(AttestationClaim const& lhs, AttestationClaim const& rhs);
};

struct CmpByClaimID
{
    bool
    operator()(AttestationClaim const& lhs, AttestationClaim const& rhs) const
    {
        return lhs.claimID < rhs.claimID;
    }
};

// Attest to a cross-chain transfer that creates an account
struct AttestationCreateAccount : AttestationBase
{
    // createCount on the sending chain. This is the value of the `CreateCount`
    // field of the bridge on the sending chain when the transaction was
    // executed.
    std::uint64_t createCount;
    // Account to create on the destination chain
    AccountID toCreate;
    // Total amount of the reward pool
    STAmount rewardAmount;

    explicit AttestationCreateAccount(STObject const& o);

    explicit AttestationCreateAccount(Json::Value const& v);

    explicit AttestationCreateAccount(
        PublicKey const& publicKey_,
        Buffer signature_,
        AccountID const& sendingAccount_,
        STAmount const& sendingAmount_,
        STAmount const& rewardAmount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_,
        std::uint64_t createCount_,
        AccountID const& toCreate_);

    explicit AttestationCreateAccount(
        STXChainBridge const& bridge,
        PublicKey const& publicKey_,
        SecretKey const& secretKey_,
        AccountID const& sendingAccount_,
        STAmount const& sendingAmount_,
        STAmount const& rewardAmount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_,
        std::uint64_t createCount_,
        AccountID const& toCreate_);

    [[nodiscard]] STObject
    toSTObject() const;

    // return true if the two attestations attest to the same thing
    [[nodiscard]] bool
    sameEvent(AttestationCreateAccount const& rhs) const;

    friend bool
    operator==(
        AttestationCreateAccount const& lhs,
        AttestationCreateAccount const& rhs);

    [[nodiscard]] static std::vector<std::uint8_t>
    message(
        STXChainBridge const& bridge,
        AccountID const& sendingAccount,
        STAmount const& sendingAmount,
        STAmount const& rewardAmount,
        AccountID const& rewardAccount,
        bool wasLockingChainSend,
        std::uint64_t createCount,
        AccountID const& dst);

    [[nodiscard]] bool
    validAmounts() const;

private:
    [[nodiscard]] std::vector<std::uint8_t>
    message(STXChainBridge const& bridge) const override;
};

struct CmpByCreateCount
{
    bool
    operator()(
        AttestationCreateAccount const& lhs,
        AttestationCreateAccount const& rhs) const
    {
        return lhs.createCount < rhs.createCount;
    }
};

};  // namespace Attestations

}  // namespace ripple

#endif
