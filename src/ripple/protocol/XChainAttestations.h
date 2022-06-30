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

#ifndef RIPPLE_PROTOCOL_STXATTESTATIONS_H_INCLUDED
#define RIPPLE_PROTOCOL_STXATTESTATIONS_H_INCLUDED

#include <ripple/basics/Buffer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainBridge.h>

#include "ripple/protocol/STObject.h"
#include <cstddef>
#include <vector>

namespace ripple {

namespace AttestationBatch {
struct AttestationClaim;
}

// Attestations from witness servers for a particular claimid and bridge.
// Only one attestation per signature is allowed. If more than one is added, the
// attestation with the larger amount is kept.
class XChainAttestations final
{
public:
    struct Attestation
    {
        AccountID keyAccount;
        STAmount amount;
        AccountID rewardAccount;
        bool wasLockingChainSend;
        std::optional<AccountID> dst;

        explicit Attestation(
            AccountID const& keyAccount_,
            STAmount const& amount_,
            AccountID const& rewardAccount_,
            bool wasLockingChainSend_,
            std::optional<AccountID> const& dst);

        explicit Attestation(
            STAccount const& keyAccount_,
            STAmount const& amount_,
            STAccount const& rewardAccount_,
            bool wasLockingChainSend_,
            std::optional<STAccount> const& dst);

        explicit Attestation(
            AttestationBatch::AttestationClaim const& claimAtt);

        explicit Attestation(STObject const& o);

        STObject
        toSTObject() const;

        friend bool
        operator==(Attestation const& lhs, Attestation const& rhs);
        friend bool
        operator!=(Attestation const& lhs, Attestation const& rhs);
    };

    using AttCollection = std::vector<Attestation>;

private:
    AttCollection attestations_;

public:
    XChainAttestations() = default;
    XChainAttestations(XChainAttestations const& rhs) = default;
    XChainAttestations&
    operator=(XChainAttestations const& rhs) = default;

    XChainAttestations(AttCollection&& sigs);

    explicit XChainAttestations(Json::Value const& v);

    explicit XChainAttestations(STArray const& arr);

    STArray
    toSTArray() const;

    /**
     Handle a new attestation event.

     Attempt to add the given attestation and reconcile with the current
     signer's list. Attestations that are not part of the current signer's
     list will be removed.

     @param claimAtt New attestation to add. It will be added if it is not
     already part of the collection, or attests to a larger value.

     @param quorum Min weight required for a quorum

     @param signersList Map from signer's account id (derived from public keys)
     to the weight of that key.

     @return optional reward accounts. If after handling the new attestation
     there is a quorum for the amount specified on the new attestation, then
     return the reward accounts for that amount, otherwise return a nullopt.
     Note that if the signer's list changes and there have been `commit`
     transactions at different amounts then there may be a different subset that
     has reached quorum. However, to "trigger" that subset would require adding
     (or re-adding) an attestation that supports that subset.

     The reason for using a nullopt instead of an empty vector when a quorum is
     not reached is to allow for an interface where a quorum is reached but no
     rewards are distributed.

     @note This function is not called `add` because it does more than just
           add the new attestation (in fact, it may not add the attestation at
           all). Instead, it handles the event of a new attestation.
     */
    std::optional<std::vector<AccountID>>
    onNewAttestation(
        AttestationBatch::AttestationClaim const& claimAtt,
        std::uint32_t quorum,
        std::unordered_map<AccountID, std::uint32_t> const& signersList);

    AttCollection::const_iterator
    begin() const;

    AttCollection::const_iterator
    end() const;

    std::size_t
    size() const;

    bool
    empty() const;

    AttCollection const&
    attestations() const;

    // verify that all the signatures attest to transaction data.
    bool
    verify() const;

private:
    // Return the message that was expected to be signed by the attesters given
    // the data to be proved.
    std::vector<std::uint8_t>
    message() const;
};

inline bool
operator==(XChainAttestations const& lhs, XChainAttestations const& rhs)
{
    return lhs.attestations() == rhs.attestations();
}

inline bool
operator!=(XChainAttestations const& lhs, XChainAttestations const& rhs)
{
    return !(lhs == rhs);
}

inline XChainAttestations::AttCollection const&
XChainAttestations::attestations() const
{
    return attestations_;
};

inline std::size_t
XChainAttestations::size() const
{
    return attestations_.size();
}

inline bool
XChainAttestations::empty() const
{
    return attestations_.empty();
}
}  // namespace ripple

#endif  // STXCHAINATTESTATIONS_H_
