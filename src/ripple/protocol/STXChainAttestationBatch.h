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

#include <boost/container/flat_set.hpp>

#include <vector>

namespace ripple {

namespace AttestationBatch {

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

    virtual ~AttestationBase() = default;

    // verify that the signature attests to the data.
    bool
    verify(STXChainBridge const& bridge) const;

protected:
    explicit AttestationBase(STObject const& o);
    explicit AttestationBase(Json::Value const& v);

    static bool
    equalHelper(AttestationBase const& lhs, AttestationBase const& rhs);

    void
    addHelper(STObject& o) const;

private:
    virtual std::vector<std::uint8_t>
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

    explicit AttestationClaim(STObject const& o);
    explicit AttestationClaim(Json::Value const& v);

    STObject
    toSTObject() const;

    static std::vector<std::uint8_t>
    message(
        STXChainBridge const& bridge,
        AccountID const& sendingAccount,
        STAmount const& sendingAmount,
        AccountID const& rewardAccount,
        bool wasLockingChainSend,
        std::uint64_t claimID,
        std::optional<AccountID> const& dst);

private:
    std::vector<std::uint8_t>
    message(STXChainBridge const& bridge) const override;

    friend bool
    operator==(AttestationClaim const& lhs, AttestationClaim const& rhs);

    friend bool
    operator!=(AttestationClaim const& lhs, AttestationClaim const& rhs);
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

    STObject
    toSTObject() const;

    friend bool
    operator==(
        AttestationCreateAccount const& lhs,
        AttestationCreateAccount const& rhs);

    friend bool
    operator!=(
        AttestationCreateAccount const& lhs,
        AttestationCreateAccount const& rhs);

private:
    std::vector<std::uint8_t>
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

};  // namespace AttestationBatch

// Attestations from witness servers for various claimids and bridge.
class STXChainAttestationBatch final : public STBase
{
public:
    using TClaims = boost::container::flat_multiset<
        AttestationBatch::AttestationClaim,
        AttestationBatch::CmpByClaimID>;

    using TCreates = boost::container::flat_multiset<
        AttestationBatch::AttestationCreateAccount,
        AttestationBatch::CmpByCreateCount>;

private:
    STXChainBridge bridge_;
    TClaims claims_;
    TCreates creates_;

public:
    using value_type = STXChainAttestationBatch;

    STXChainAttestationBatch();

    explicit STXChainAttestationBatch(SField const& name);

    STXChainAttestationBatch(STXChainAttestationBatch const& rhs) = default;

    template <
        class TClaimIter,
        class TCreateIter = AttestationBatch::AttestationCreateAccount* const>
    STXChainAttestationBatch(
        STXChainBridge const& bridge,
        TClaimIter claimBegin,
        TClaimIter claimEnd,
        TCreateIter createBegin = nullptr,
        TCreateIter createEnd = nullptr);

    explicit STXChainAttestationBatch(STObject const& o);

    explicit STXChainAttestationBatch(Json::Value const& o);

    explicit STXChainAttestationBatch(SField const& name, Json::Value const& o);

    explicit STXChainAttestationBatch(SerialIter& sit, SField const& name);

    STXChainAttestationBatch&
    operator=(STXChainAttestationBatch const& rhs) = default;

    STObject
    toSTObject() const;

    // verify that all the signatures attest to their data.
    bool
    verify() const;

    STXChainBridge const&
    bridge() const;

    TClaims const&
    claims() const;

    TCreates const&
    creates() const;

    std::size_t
    numAttestations() const;

    SerializedTypeID
    getSType() const override;

    Json::Value getJson(JsonOptions) const override;

    void
    add(Serializer& s) const override;

    bool
    isEquivalent(const STBase& t) const override;

    bool
    isDefault() const override;

    value_type const&
    value() const noexcept;

private:
    static std::unique_ptr<STXChainAttestationBatch>
    construct(SerialIter&, SField const& name);
    STBase*
    copy(std::size_t n, void* buf) const override;
    STBase*
    move(std::size_t n, void* buf) override;

    friend bool
    operator==(
        STXChainAttestationBatch const& lhs,
        STXChainAttestationBatch const& rhs);

    friend bool
    operator!=(
        STXChainAttestationBatch const& lhs,
        STXChainAttestationBatch const& rhs);
};

template <class TClaimIter, class TCreateIter>
STXChainAttestationBatch::STXChainAttestationBatch(
    STXChainBridge const& bridge,
    TClaimIter claimBegin,
    TClaimIter claimEnd,
    TCreateIter createBegin,
    TCreateIter createEnd)
    : STBase{sfXChainAttestationBatch}, bridge_{bridge}
{
    claims_.reserve(std::distance(claimBegin, claimEnd));
    creates_.reserve(std::distance(createBegin, createEnd));
    for (auto i = claimBegin; i != claimEnd; ++i)
        claims_.insert(*i);
    for (auto i = createBegin; i != createEnd; ++i)
        creates_.insert(*i);
}

inline STXChainBridge const&
STXChainAttestationBatch::bridge() const
{
    return bridge_;
}

inline STXChainAttestationBatch::TClaims const&
STXChainAttestationBatch::claims() const
{
    return claims_;
}

inline STXChainAttestationBatch::TCreates const&
STXChainAttestationBatch::creates() const
{
    return creates_;
}

inline STXChainAttestationBatch::value_type const&
STXChainAttestationBatch::value() const noexcept
{
    return *this;
}

}  // namespace ripple

#endif
