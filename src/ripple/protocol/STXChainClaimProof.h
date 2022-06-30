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

#ifndef RIPPLE_PROTOCOL_STXCHAINPROOF_H_INCLUDED
#define RIPPLE_PROTOCOL_STXCHAINPROOF_H_INCLUDED

#include <ripple/basics/Buffer.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STXChainBridge.h>

#include <vector>

namespace ripple {

class STXChainClaimProof final : public STBase
{
public:
    using SigCollection = std::vector<std::pair<PublicKey, Buffer>>;

private:
    STXChainBridge sidechain_;
    STAmount amount_;
    std::uint32_t xChainSeqNum_{};
    // True if the asset was sent to the door account on the src chain.
    // False if the asset was sent to the door account on the dst chain.
    bool wasSrcChainSend_{true};
    SigCollection signatures_;

public:
    using value_type = STXChainClaimProof;

    STXChainClaimProof() = default;
    STXChainClaimProof(STXChainClaimProof const& rhs) = default;
    STXChainClaimProof&
    operator=(STXChainClaimProof const& rhs) = default;

    STXChainClaimProof(
        STXChainBridge const& sidechain,
        STAmount const& amount,
        std::uint32_t xChainSeqNum,
        bool wasSrcChainSend,
        SigCollection&& sigs,
        SField const& f = sfGeneric);

    STXChainClaimProof(SField const& f);

    STXChainClaimProof(SerialIter&, SField const& name = sfGeneric);

    SerializedTypeID
    getSType() const override;

    void
    add(Serializer& s) const override;

    bool
    isEquivalent(const STBase& t) const override;

    bool
    isDefault() const override;

    STXChainClaimProof::value_type const&
    value() const noexcept;

    friend bool
    operator==(STXChainClaimProof const& lhs, STXChainClaimProof const& rhs)
    {
        return std::tie(
                   lhs.wasSrcChainSend_,
                   lhs.sidechain_,
                   lhs.xChainSeqNum_,
                   lhs.signatures_) ==
            std::tie(
                   rhs.wasSrcChainSend_,
                   rhs.sidechain_,
                   rhs.xChainSeqNum_,
                   rhs.signatures_);
    }

    friend bool
    operator!=(STXChainClaimProof const& lhs, STXChainClaimProof const& rhs)
    {
        return !(lhs == rhs);
    }

    STXChainBridge const&
    sidechain() const
    {
        return sidechain_;
    };

    STAmount
    amount() const
    {
        return amount_;
    };

    std::uint32_t
    xChainSeqNum() const
    {
        return xChainSeqNum_;
    };

    bool
    wasSrcChainSend() const
    {
        return wasSrcChainSend_;
    }

    bool
    expectSrcChainClaim() const
    {
        return !wasSrcChainSend_;
    }

    SigCollection const&
    signatures() const
    {
        return signatures_;
    };

    // verify that all the signatures attest to transaction data.
    bool
    verify() const;

private:
    // Return the message that was expected to be signed by the attesters given
    // the data to be proved.
    std::vector<std::uint8_t>
    message() const;

    STBase*
    copy(std::size_t n, void* buf) const override;
    STBase*
    move(std::size_t n, void* buf) override;

    friend class detail::STVar;
};

std::vector<std::uint8_t>
ChainClaimProofMessage(
    STXChainBridge const& sidechain,
    STAmount const& amount,
    std::uint32_t xChainSeqNum,
    bool wasSrcChainSend);

// May throw
STXChainClaimProof
STXChainClaimProofFromJson(SField const& name, Json::Value const& v);

}  // namespace ripple

#endif
