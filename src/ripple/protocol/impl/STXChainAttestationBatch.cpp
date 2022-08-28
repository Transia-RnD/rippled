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

#include <ripple/protocol/STXChainAttestationBatch.h>

#include <ripple/basics/StringUtilities.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/jss.h>

#include <algorithm>

namespace ripple {

namespace AttestationBatch {

AttestationBase::AttestationBase(
    PublicKey const& publicKey_,
    Buffer signature_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_)
    : publicKey{publicKey_}
    , signature{std::move(signature_)}
    , sendingAccount{sendingAccount_}
    , sendingAmount{sendingAmount_}
    , rewardAccount{rewardAccount_}
    , wasLockingChainSend{wasLockingChainSend_}
{
}

bool
AttestationBase::equalHelper(
    AttestationBase const& lhs,
    AttestationBase const& rhs)
{
    return std::tie(
               lhs.publicKey,
               lhs.signature,
               lhs.sendingAccount,
               lhs.sendingAmount,
               lhs.rewardAccount,
               lhs.wasLockingChainSend) ==
        std::tie(
               rhs.publicKey,
               rhs.signature,
               rhs.sendingAccount,
               rhs.sendingAmount,
               rhs.rewardAccount,
               rhs.wasLockingChainSend);
}

bool
AttestationBase::sameEventHelper(
    AttestationBase const& lhs,
    AttestationBase const& rhs)
{
    return std::tie(
               lhs.sendingAccount,
               lhs.sendingAmount,
               lhs.wasLockingChainSend) ==
        std::tie(
               rhs.sendingAccount, rhs.sendingAmount, rhs.wasLockingChainSend);
}

bool
AttestationBase::verify(STXChainBridge const& bridge) const
{
    std::vector<std::uint8_t> msg = message(bridge);
    return ripple::verify(publicKey, makeSlice(msg), signature);
}

AttestationBase::AttestationBase(STObject const& o)
    : publicKey{o[sfPublicKey]}
    , signature{o[sfSignature]}
    , sendingAccount{o[sfAccount]}
    , sendingAmount{o[sfAmount]}
    , rewardAccount{o[sfAttestationRewardAccount]}
    , wasLockingChainSend{bool(o[sfWasLockingChainSend])}
{
}

AttestationBase::AttestationBase(Json::Value const& v)
    : publicKey{Json::getOrThrow<PublicKey>(v, sfPublicKey)}
    , signature{Json::getOrThrow<Buffer>(v, sfSignature)}
    , sendingAccount{Json::getOrThrow<AccountID>(v, sfAccount)}
    , sendingAmount{Json::getOrThrow<STAmount>(v, sfAmount)}
    , rewardAccount{Json::getOrThrow<AccountID>(v, sfAttestationRewardAccount)}
    , wasLockingChainSend{Json::getOrThrow<bool>(v, sfWasLockingChainSend)}
{
}

void
AttestationBase::addHelper(STObject& o) const
{
    o[sfPublicKey] = publicKey;
    o[sfSignature] = signature;
    o[sfAmount] = sendingAmount;
    o[sfAccount] = sendingAccount;
    o[sfAttestationRewardAccount] = rewardAccount;
    o[sfWasLockingChainSend] = wasLockingChainSend;
}

AttestationClaim::AttestationClaim(
    PublicKey const& publicKey_,
    Buffer signature_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t claimID_,
    std::optional<AccountID> const& dst_)
    : AttestationBase{publicKey_, std::move(signature_), sendingAccount_, sendingAmount_, rewardAccount_, wasLockingChainSend_}
    , claimID{claimID_}
    , dst{dst_}
{
}

AttestationClaim::AttestationClaim(
    STXChainBridge const& bridge,
    PublicKey const& publicKey_,
    SecretKey const& secretKey_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t claimID_,
    std::optional<AccountID> const& dst_)
    : AttestationClaim{
          publicKey_,
          Buffer{},
          sendingAccount_,
          sendingAmount_,
          rewardAccount_,
          wasLockingChainSend_,
          claimID_,
          dst_}
{
    auto const toSign = message(bridge);
    signature = sign(publicKey_, secretKey_, makeSlice(toSign));
}

AttestationClaim::AttestationClaim(STObject const& o)
    : AttestationBase(o), claimID{o[sfXChainClaimID]}, dst{o[~sfDestination]}
{
}

AttestationClaim::AttestationClaim(Json::Value const& v)
    : AttestationBase{v}
    , claimID{Json::getOrThrow<std::uint64_t>(v, sfXChainClaimID)}
{
    if (v.isMember(sfDestination.getJsonName()))
        dst = Json::getOrThrow<AccountID>(v, sfDestination);
}

STObject
AttestationClaim::toSTObject() const
{
    STObject o{sfXChainClaimAttestationBatchElement};
    addHelper(o);
    o[sfXChainClaimID] = claimID;
    if (dst)
        o[sfDestination] = *dst;
    return o;
}

std::vector<std::uint8_t>
AttestationClaim::message(
    STXChainBridge const& bridge,
    AccountID const& sendingAccount,
    STAmount const& sendingAmount,
    AccountID const& rewardAccount,
    bool wasLockingChainSend,
    std::uint64_t claimID,
    std::optional<AccountID> const& dst)
{
    Serializer s;

    bridge.add(s);
    s.addBitString(sendingAccount);
    sendingAmount.add(s);
    s.addBitString(rewardAccount);
    std::uint8_t const lc = wasLockingChainSend ? 1 : 0;
    s.add8(lc);

    s.add64(claimID);
    if (dst)
        s.addBitString(*dst);

    return std::move(s.modData());
}

std::vector<std::uint8_t>
AttestationClaim::message(STXChainBridge const& bridge) const
{
    return AttestationClaim::message(
        bridge,
        sendingAccount,
        sendingAmount,
        rewardAccount,
        wasLockingChainSend,
        claimID,
        dst);
}

bool
AttestationClaim::sameEvent(AttestationClaim const& rhs) const
{
    return AttestationClaim::sameEventHelper(*this, rhs) &&
        tie(claimID, dst) == tie(rhs.claimID, rhs.dst);
}

bool
operator==(AttestationClaim const& lhs, AttestationClaim const& rhs)
{
    return AttestationClaim::equalHelper(lhs, rhs) &&
        tie(lhs.claimID, lhs.dst) == tie(rhs.claimID, rhs.dst);
}

bool
operator!=(AttestationClaim const& lhs, AttestationClaim const& rhs)
{
    return !(lhs == rhs);
}

AttestationCreateAccount::AttestationCreateAccount(STObject const& o)
    : AttestationBase(o)
    , createCount{o[sfXChainAccountCreateCount]}
    , toCreate{o[sfDestination]}
    , rewardAmount{o[sfSignatureReward]}
{
}

AttestationCreateAccount::AttestationCreateAccount(Json::Value const& v)
    : AttestationBase{v}
    , createCount{Json::getOrThrow<std::uint64_t>(
          v,
          sfXChainAccountCreateCount)}
    , toCreate{Json::getOrThrow<AccountID>(v, sfDestination)}
    , rewardAmount{Json::getOrThrow<STAmount>(v, sfSignatureReward)}
{
}

AttestationCreateAccount::AttestationCreateAccount(
    PublicKey const& publicKey_,
    Buffer signature_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    STAmount const& rewardAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t createCount_,
    AccountID const& toCreate_)
    : AttestationBase{publicKey_, std::move(signature_), sendingAccount_, sendingAmount_, rewardAccount_, wasLockingChainSend_}
    , createCount{createCount_}
    , toCreate{toCreate_}
    , rewardAmount{rewardAmount_}
{
}

AttestationCreateAccount::AttestationCreateAccount(
    STXChainBridge const& bridge,
    PublicKey const& publicKey_,
    SecretKey const& secretKey_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    STAmount const& rewardAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t createCount_,
    AccountID const& toCreate_)
    : AttestationCreateAccount{
          publicKey_,
          Buffer{},
          sendingAccount_,
          sendingAmount_,
          rewardAmount_,
          rewardAccount_,
          wasLockingChainSend_,
          createCount_,
          toCreate_}
{
    auto const toSign = message(bridge);
    signature = sign(publicKey_, secretKey_, makeSlice(toSign));
}

STObject
AttestationCreateAccount::toSTObject() const
{
    STObject o{sfXChainCreateAccountAttestationBatchElement};
    addHelper(o);

    o[sfXChainAccountCreateCount] = createCount;
    o[sfDestination] = toCreate;
    o[sfSignatureReward] = rewardAmount;

    return o;
}

std::vector<std::uint8_t>
AttestationCreateAccount::message(
    STXChainBridge const& bridge,
    AccountID const& sendingAccount,
    STAmount const& sendingAmount,
    STAmount const& rewardAmount,
    AccountID const& rewardAccount,
    bool wasLockingChainSend,
    std::uint64_t createCount,
    AccountID const& dst)
{
    Serializer s;

    bridge.add(s);
    s.addBitString(sendingAccount);
    sendingAmount.add(s);
    rewardAmount.add(s);
    s.addBitString(rewardAccount);
    std::uint8_t const lc = wasLockingChainSend ? 1 : 0;
    s.add8(lc);

    s.add64(createCount);
    s.addBitString(dst);

    return std::move(s.modData());
}

std::vector<std::uint8_t>
AttestationCreateAccount::message(STXChainBridge const& bridge) const
{
    return AttestationCreateAccount::message(
        bridge,
        sendingAccount,
        sendingAmount,
        rewardAmount,
        rewardAccount,
        wasLockingChainSend,
        createCount,
        toCreate);
}

bool
AttestationCreateAccount::sameEvent(AttestationCreateAccount const& rhs) const
{
    return AttestationCreateAccount::sameEventHelper(*this, rhs) &&
        std::tie(createCount, toCreate, rewardAmount) ==
        std::tie(rhs.createCount, rhs.toCreate, rhs.rewardAmount);
}

bool
operator==(
    AttestationCreateAccount const& lhs,
    AttestationCreateAccount const& rhs)
{
    return AttestationCreateAccount::equalHelper(lhs, rhs) &&
        std::tie(lhs.createCount, lhs.toCreate, lhs.rewardAmount) ==
        std::tie(rhs.createCount, rhs.toCreate, rhs.rewardAmount);
}

bool
operator!=(
    AttestationCreateAccount const& lhs,
    AttestationCreateAccount const& rhs)
{
    return !(lhs == rhs);
}

}  // namespace AttestationBatch

bool
operator==(
    STXChainAttestationBatch const& lhs,
    STXChainAttestationBatch const& rhs)
{
    return std::tie(lhs.bridge_, lhs.claims_, lhs.creates_) ==
        std::tie(rhs.bridge_, rhs.claims_, rhs.creates_);
}

bool
operator!=(
    STXChainAttestationBatch const& lhs,
    STXChainAttestationBatch const& rhs)
{
    return !operator==(lhs, rhs);
}

STXChainAttestationBatch::STXChainAttestationBatch()
    : STBase{sfXChainAttestationBatch}
{
}

STXChainAttestationBatch::STXChainAttestationBatch(SField const& name)
    : STBase{name}
{
}

STXChainAttestationBatch::STXChainAttestationBatch(STObject const& o)
    : STBase{sfXChainAttestationBatch}
    , bridge_{o.getFieldObject(sfXChainBridge)}
{
    {
        STArray const claimAtts{o.getFieldArray(sfXChainClaimAttestationBatch)};
        for (auto const& c : claimAtts)
        {
            claims_.emplace(c);
        }
    }
    {
        STArray const createAtts{
            o.getFieldArray(sfXChainCreateAccountAttestationBatch)};
        for (auto const& c : createAtts)
        {
            creates_.emplace(c);
        }
    }
}

STXChainAttestationBatch::STXChainAttestationBatch(Json::Value const& o)
    : STXChainAttestationBatch{sfXChainAttestationBatch, o}
{
}

STXChainAttestationBatch::STXChainAttestationBatch(
    SField const& name,
    Json::Value const& o)
    : STBase{name}
{
    // TODO; Check that there are no extra fields
    {
        if (!o.isMember(sfXChainBridge.getJsonName()))
        {
            Throw<std::runtime_error>(
                "STXChainAttestationBatch missing Bridge field.");
        }
        auto const& b = o[sfXChainBridge.getJsonName()];
        if (!b.isObject())
        {
            Throw<std::runtime_error>(
                "STXChainAttestationBatch Bridge must be an object.");
        }

        bridge_ = STXChainBridge{b};
    }

    if (o.isMember(sfXChainClaimAttestationBatch.getJsonName()))
    {
        auto const& claims = o[sfXChainClaimAttestationBatch.getJsonName()];
        if (!claims.isArray())
        {
            Throw<std::runtime_error>(
                "STXChainAttestationBatch XChainClaimAttesationBatch must "
                "be "
                "an array.");
        }
        claims_.reserve(claims.size());
        for (auto const& c : claims)
        {
            if (!c.isMember(sfXChainClaimAttestationBatchElement.getJsonName()))
            {
                Throw<std::runtime_error>(
                    "XChainClaimAttesationBatch must contain a "
                    "XChainClaimAttestationBatchElement field");
            }
            auto const& elem =
                c[sfXChainClaimAttestationBatchElement.getJsonName()];
            if (!elem.isObject())
            {
                Throw<std::runtime_error>(
                    "XChainClaimAttesationBatch contains a "
                    "XChainClaimAttestationBatchElement that is not an "
                    "object");
            }
            claims_.emplace(elem);
        }
    }
    if (o.isMember(sfXChainCreateAccountAttestationBatch.getJsonName()))
    {
        auto const& createAccounts =
            o[sfXChainCreateAccountAttestationBatch.getJsonName()];
        if (!createAccounts.isArray())
        {
            Throw<std::runtime_error>(
                "STXChainAttestationBatch "
                "XChainCreateAccountAttesationBatch "
                "must be an array.");
        }
        creates_.reserve(createAccounts.size());
        for (auto const& c : createAccounts)
        {
            if (!c.isMember(
                    sfXChainCreateAccountAttestationBatchElement.getJsonName()))
            {
                Throw<std::runtime_error>(
                    "XChainCreateAccountAttesationBatch must contain a "
                    "XChainCreateAccountAttestationBatchElement field");
            }
            auto const& elem =
                c[sfXChainCreateAccountAttestationBatchElement.getJsonName()];
            if (!elem.isObject())
            {
                Throw<std::runtime_error>(
                    "XChainCreateAccountAttesationBatch contains a "
                    "XChainCreateAccountAttestationBatchElement that is "
                    "not an "
                    "object");
            }
            creates_.emplace(elem);
        }
    }
}

STXChainAttestationBatch::STXChainAttestationBatch(
    SerialIter& sit,
    SField const& name)
    : STBase{name}, bridge_{sit, sfXChainBridge}
{
    {
        STArray const a{sit, sfXChainClaimAttestationBatch};
        claims_.reserve(a.size());
        for (auto const& c : a)
            claims_.emplace(c);
    }
    {
        STArray const a{sit, sfXChainCreateAccountAttestationBatch};
        creates_.reserve(a.size());
        for (auto const& c : a)
            creates_.emplace(c);
    }
}

void
STXChainAttestationBatch::add(Serializer& s) const
{
    bridge_.add(s);
    {
        STArray claimAtts{sfXChainClaimAttestationBatch, claims_.size()};
        for (auto const& claim : claims_)
            claimAtts.push_back(claim.toSTObject());
        claimAtts.add(s);
        s.addFieldID(STI_ARRAY, 1);
    }
    {
        STArray createAtts{
            sfXChainCreateAccountAttestationBatch, creates_.size()};
        for (auto const& create : creates_)
            createAtts.push_back(create.toSTObject());
        createAtts.add(s);
        s.addFieldID(STI_ARRAY, 1);
    }
}

Json::Value
STXChainAttestationBatch::getJson(JsonOptions jo) const
{
    Json::Value v;
    v[sfXChainBridge.getJsonName()] = bridge_.getJson(jo);
    // TODO: remove the code duplication with `add`
    {
        STArray claimAtts{sfXChainClaimAttestationBatch, claims_.size()};
        for (auto const& claim : claims_)
            claimAtts.push_back(claim.toSTObject());
        v[sfXChainClaimAttestationBatch.getJsonName()] = claimAtts.getJson(jo);
    }
    {
        STArray createAtts{
            sfXChainCreateAccountAttestationBatch, creates_.size()};
        for (auto const& create : creates_)
            createAtts.push_back(create.toSTObject());
        v[sfXChainCreateAccountAttestationBatch.getJsonName()] =
            createAtts.getJson(jo);
    }
    return v;
}

STObject
STXChainAttestationBatch::toSTObject() const
{
    STObject o{sfXChainAttestationBatch};
    o[sfXChainBridge] = bridge_;
    {
        STArray claimAtts{sfXChainClaimAttestationBatch, claims_.size()};
        for (auto const& claim : claims_)
        {
            claimAtts.push_back(claim.toSTObject());
        }
        // TODO: Both the array and the object are called the same thing
        // this correct? (same in the loop below)
        o.setFieldArray(sfXChainClaimAttestationBatch, claimAtts);
    }
    {
        STArray createAtts{
            sfXChainCreateAccountAttestationBatch, creates_.size()};
        for (auto const& create : creates_)
        {
            createAtts.push_back(create.toSTObject());
        }
        o.setFieldArray(sfXChainCreateAccountAttestationBatch, createAtts);
    }
    return o;
}

std::size_t
STXChainAttestationBatch::numAttestations() const
{
    return claims_.size() + creates_.size();
}

bool
STXChainAttestationBatch::verify() const
{
    return std::all_of(
               claims_.begin(),
               claims_.end(),
               [&](auto const& c) { return c.verify(bridge_); }) &&
        std::all_of(creates_.begin(), creates_.end(), [&](auto const& c) {
               return c.verify(bridge_);
           });
}

bool
STXChainAttestationBatch::noConflicts() const
{
    // Check that all the batches attest to the same thing
    auto isConsistent = [](auto batchStart, auto batchEnd) -> bool {
        if (batchStart == batchEnd)
            return true;
        auto const& toMatch = *batchStart;
        ++batchStart;
        for (auto i = batchStart; i != batchEnd; ++i)
        {
            if (!toMatch.sameEvent(*i))
            {
                return false;
            }
        }
        return true;
    };

    {
        // Check that all the claim batches attest to the same thing
        auto r = for_each_create_batch<bool>(
            creates_.begin(), creates_.end(), isConsistent);
        if (!std::all_of(r.begin(), r.end(), [](bool v) { return v; }))
        {
            return false;
        }
    }
    {
        auto r = for_each_claim_batch<bool>(
            claims_.begin(), claims_.end(), isConsistent);
        if (!std::all_of(r.begin(), r.end(), [](bool v) { return v; }))
        {
            return false;
        }
    }

    return true;
}

bool
STXChainAttestationBatch::validAmounts() const
{
    for (auto const& c : creates_)
    {
        if (!isLegalNet(c.rewardAmount) || !isLegalNet(c.sendingAmount))
            return false;
    }
    for (auto const& c : claims_)
    {
        if (!isLegalNet(c.sendingAmount))
            return false;
    }

    return true;
}

SerializedTypeID
STXChainAttestationBatch::getSType() const
{
    return STI_XCHAIN_ATTESTATION_BATCH;
}

bool
STXChainAttestationBatch::isEquivalent(const STBase& t) const
{
    const STXChainAttestationBatch* v =
        dynamic_cast<const STXChainAttestationBatch*>(&t);
    return v && (*v == *this);
}

bool
STXChainAttestationBatch::isDefault() const
{
    return bridge_.isDefault() && claims_.empty() && creates_.empty();
}

std::unique_ptr<STXChainAttestationBatch>
STXChainAttestationBatch::construct(SerialIter& sit, SField const& name)
{
    return std::make_unique<STXChainAttestationBatch>(sit, name);
}

STBase*
STXChainAttestationBatch::copy(std::size_t n, void* buf) const
{
    return emplace(n, buf, *this);
}

STBase*
STXChainAttestationBatch::move(std::size_t n, void* buf)
{
    return emplace(n, buf, std::move(*this));
}
}  // namespace ripple
