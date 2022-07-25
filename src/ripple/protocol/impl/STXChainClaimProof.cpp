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

#include <ripple/protocol/STXChainClaimProof.h>

#include <ripple/basics/StringUtilities.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/InnerObjectFormats.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/jss.h>

namespace ripple {

STXChainClaimProof::STXChainClaimProof(
    STXChainBridge const& sidechain,
    STAmount const& amount,
    std::uint32_t xChainSeqNum,
    bool wasSrcChainSend,
    SigCollection&& sigs,
    SField const& f)
    : STBase(f)
    , sidechain_{sidechain}
    , amount_{amount}
    , xChainSeqNum_{xChainSeqNum}
    , wasSrcChainSend_{wasSrcChainSend}
    , signatures_{std::move(sigs)}
{
}

STXChainClaimProof::STXChainClaimProof(SField const& f) : STBase(f)
{
}

STXChainClaimProof::value_type const&
STXChainClaimProof::value() const noexcept
{
    return *this;
}

STXChainClaimProof::STXChainClaimProof(SerialIter& sit, SField const& name)
    : STBase(name), sidechain_{sit, sfXChainBridge}, amount_{sit, sfAmount}
{
    xChainSeqNum_ = sit.get32();
    wasSrcChainSend_ = static_cast<bool>(sit.get8());
    STArray sigs{sit, sfXChainProofSigs};
    signatures_.reserve(sigs.size());
    for (auto const& sigPair : sigs)
    {
        PublicKey pk{sigPair[sfPublicKey]};
        Buffer sig{sigPair[sfSignature]};
        signatures_.emplace_back(pk, std::move(sig));
    }
}

STBase*
STXChainClaimProof::copy(std::size_t n, void* buf) const
{
    return emplace(n, buf, *this);
}

STBase*
STXChainClaimProof::move(std::size_t n, void* buf)
{
    return emplace(n, buf, std::move(*this));
}

SerializedTypeID
STXChainClaimProof::getSType() const
{
    return STI_XCHAIN_CLAIM_PROOF;
}

void
STXChainClaimProof::add(Serializer& s) const
{
    sidechain_.add(s);
    amount_.add(s);
    s.add32(xChainSeqNum_);
    std::uint8_t const isSrc = wasSrcChainSend_ ? 1 : 0;
    s.add8(isSrc);
    STArray sigs{sfXChainProofSigs, signatures_.size()};
    for (auto const& [pk, sig] : signatures_)
    {
        STObject o{sfXChainClaimProofSig};
        o[sfPublicKey] = pk;
        o[sfSignature] = sig;
        sigs.push_back(o);
    }
    sigs.add(s);
}

bool
STXChainClaimProof::isEquivalent(const STBase& t) const
{
    const STXChainClaimProof* v = dynamic_cast<const STXChainClaimProof*>(&t);
    return v && (*v == *this);
}

bool
STXChainClaimProof::isDefault() const
{
    // TODO: add isDefault to sidechain_
    return amount_.isDefault() && xChainSeqNum_ == 0 && wasSrcChainSend_ &&
        signatures_.empty();
    // return sidechain_.isDefault() && amount_.isDefault() &&
    //    xChainSeqNum_ == 0 && wasSrcChainSend_ && signatures_.empty();
}

std::vector<std::uint8_t>
STXChainClaimProof::message() const
{
    return ChainClaimProofMessage(
        sidechain_, amount_, xChainSeqNum_, wasSrcChainSend_);
}

bool
STXChainClaimProof::verify() const
{
    std::vector<std::uint8_t> msg = message();
    for (auto const& [pk, sig] : signatures_)
    {
        if (!ripple::verify(pk, makeSlice(msg), sig))
            return false;
    }
    return true;
}

std::vector<std::uint8_t>
ChainClaimProofMessage(
    STXChainBridge const& sidechain,
    STAmount const& amount,
    std::uint32_t xChainSeqNum,
    bool wasSrcChainSend)
{
    Serializer s;

    sidechain.add(s);
    amount.add(s);
    s.add32(xChainSeqNum);
    std::uint8_t const isSrc = wasSrcChainSend ? 1 : 0;
    s.add8(isSrc);

    return std::move(s.modData());
}

STXChainClaimProof
STXChainClaimProofFromJson(SField const& name, Json::Value const& v)
{
    if (!v.isObject())
    {
        Throw<std::runtime_error>(
            "STXChainClaimProof can only be specified with a 'object' Json "
            "value");
    }
    // TODO: Throw is a field is not present
    // TODO: Throw if too many signatures
    STXChainBridge sidechain{v[jss::sidechain]};
    STAmount const amt = amountFromJson(sfAmount, v[jss::amount]);
    std::uint32_t const xchainSeq = v[jss::xchain_seq].asUInt();
    bool const isSrc = v[jss::was_src_chain_send].asBool();
    std::vector<std::pair<PublicKey, Buffer>> sigs = [&] {
        auto const jSigs = v[jss::signatures];
        std::vector<std::pair<PublicKey, Buffer>> r;
        r.reserve(jSigs.size());
        for (auto const& sig : jSigs)
        {
            auto const signingKeyB58 = sig[jss::signing_key].asString();
            std::optional<PublicKey> pk;
            for (auto const tokenType :
                 {TokenType::NodePublic, TokenType::AccountPublic})
            {
                pk = parseBase58<PublicKey>(tokenType, signingKeyB58);
                if (pk)
                    break;
            }
            if (!pk)
            {
                Throw<std::runtime_error>(
                    "Invalid base 58 signing public key in claim proof");
            }
            auto const sigHex = sig[jss::signature].asString();
            auto sigBlob = strUnHex(sigHex);
            if (!sigBlob)
            {
                Throw<std::runtime_error>(
                    "Invalid hex signature in claim proof");
            }
            // TODO: Mismatch between buffer and blob
            r.emplace_back(*pk, Buffer(sigBlob->data(), sigBlob->size()));
        }
        return r;
    }();

    return STXChainClaimProof{
        sidechain, amt, xchainSeq, isSrc, std::move(sigs), name};
}

}  // namespace ripple
