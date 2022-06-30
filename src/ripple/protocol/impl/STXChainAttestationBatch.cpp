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
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/jss.h>

#include <boost/lexical_cast.hpp>

#include <algorithm>

namespace ripple {

namespace {
// TODO: Add these to json_value.h?
struct JsonMissingKeyError : std::exception
{
    char const* const key;
    mutable std::string msg;
    JsonMissingKeyError(Json::StaticString const& k) : key{k.c_str()}
    {
    }
    const char*
    what() const noexcept override
    {
        if (msg.empty())
        {
            msg = std::string("Missing json key: ") + key;
        }
        return msg.c_str();
    }
};

struct JsonTypeMismatchError : std::exception
{
    char const* const key;
    std::string const expectedType;
    mutable std::string msg;
    JsonTypeMismatchError(Json::StaticString const& k, std::string et)
        : key{k.c_str()}, expectedType{std::move(et)}
    {
    }
    const char*
    what() const noexcept override
    {
        if (msg.empty())
        {
            msg = std::string("Type mismatch on json key: ") + key +
                "; expected type: " + expectedType;
        }
        return msg.c_str();
    }
};

template <class T>
T
getOrThrow(Json::Value const& v, SField const& field)
{
    static_assert(sizeof(T) == -1, "This function must be specialized");
}

template <>
std::string
getOrThrow(Json::Value const& v, SField const& field)
{
    Json::StaticString const& key = field.getJsonName();
    if (!v.isMember(key))
        Throw<JsonMissingKeyError>(key);

    Json::Value const& inner = v[key];
    if (!inner.isString())
        Throw<JsonTypeMismatchError>(key, "string");
    return inner.asString();
}

// Note, this allows integer numeric fields to act as bools
template <>
bool
getOrThrow(Json::Value const& v, SField const& field)
{
    Json::StaticString const& key = field.getJsonName();
    if (!v.isMember(key))
        Throw<JsonMissingKeyError>(key);
    Json::Value const& inner = v[key];
    if (inner.isBool())
        return inner.asBool();
    if (!inner.isIntegral())
        Throw<JsonTypeMismatchError>(key, "bool");

    return inner.asInt() != 0;
}

template <>
std::uint64_t
getOrThrow(Json::Value const& v, SField const& field)
{
    Json::StaticString const& key = field.getJsonName();
    if (!v.isMember(key))
        Throw<JsonMissingKeyError>(key);
    Json::Value const& inner = v[key];
    if (inner.isUInt())
        return inner.asUInt();
    if (inner.isInt())
    {
        auto const r = inner.asInt();
        if (r < 0)
            Throw<JsonTypeMismatchError>(key, "uint64");
        return r;
    }
    if (inner.isString())
    {
        auto const s = inner.asString();
        try
        {
            return boost::lexical_cast<std::uint64_t>(s);
        }
        catch (...)
        {
        }
    }
    Throw<JsonTypeMismatchError>(key, "uint64");
}

template <>
PublicKey
getOrThrow(Json::Value const& v, SField const& field)
{
    std::string const b58 = getOrThrow<std::string>(v, field);
    if (auto pubKeyBlob = strUnHex(b58); publicKeyType(makeSlice(*pubKeyBlob)))
    {
        return PublicKey{makeSlice(*pubKeyBlob)};
    }
    for (auto const tokenType :
         {TokenType::NodePublic, TokenType::AccountPublic})
    {
        if (auto const pk = parseBase58<PublicKey>(tokenType, b58))
            return *pk;
    }
    Throw<JsonTypeMismatchError>(field.getJsonName(), "PublicKey");
}

template <>
AccountID
getOrThrow(Json::Value const& v, SField const& field)
{
    std::string const b58 = getOrThrow<std::string>(v, field);
    if (auto const r = parseBase58<AccountID>(b58))
        return *r;
    Throw<JsonTypeMismatchError>(field.getJsonName(), "AccountID");
}

template <>
Buffer
getOrThrow(Json::Value const& v, SField const& field)
{
    std::string const hex = getOrThrow<std::string>(v, field);
    if (auto const r = strUnHex(hex))
    {
        // TODO: mismatch between a buffer and a blob
        return Buffer{r->data(), r->size()};
    }
    Throw<JsonTypeMismatchError>(field.getJsonName(), "Buffer");
}

template <>
STAmount
getOrThrow(Json::Value const& v, SField const& field)
{
    Json::StaticString const& key = field.getJsonName();
    if (!v.isMember(key))
        Throw<JsonMissingKeyError>(key);
    Json::Value const& inner = v[key];
    return amountFromJson(field, inner);
}
}  // namespace

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
    : publicKey{getOrThrow<PublicKey>(v, sfPublicKey)}
    , signature{getOrThrow<Buffer>(v, sfSignature)}
    , sendingAccount{getOrThrow<AccountID>(v, sfAccount)}
    , sendingAmount{getOrThrow<STAmount>(v, sfAmount)}
    , rewardAccount{getOrThrow<AccountID>(v, sfAttestationRewardAccount)}
    , wasLockingChainSend{getOrThrow<bool>(v, sfWasLockingChainSend)}
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

AttestationClaim::AttestationClaim(STObject const& o)
    : AttestationBase(o), claimID{o[sfXChainClaimID]}, dst{o[~sfDestination]}
{
}

AttestationClaim::AttestationClaim(Json::Value const& v)
    : AttestationBase{v}, claimID{getOrThrow<std::uint64_t>(v, sfXChainClaimID)}
{
    if (v.isMember(sfDestination.getJsonName()))
        dst = getOrThrow<AccountID>(v, sfDestination);
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
    , createCount{getOrThrow<std::uint64_t>(v, sfXChainAccountCreateCount)}
    , toCreate{getOrThrow<AccountID>(v, sfDestination)}
    , rewardAmount{getOrThrow<STAmount>(v, sfSignatureReward)}
{
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
AttestationCreateAccount::message(STXChainBridge const& bridge) const
{
    Serializer s;

    bridge.add(s);
    s.addBitString(sendingAccount);
    sendingAmount.add(s);
    s.addBitString(rewardAccount);
    std::uint8_t const lc = wasLockingChainSend ? 1 : 0;
    s.add8(lc);

    s.add64(createCount);
    s.addBitString(toCreate);
    rewardAmount.add(s);

    return std::move(s.modData());
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
                    "XChainClaimAttestationBatchElement that is not an object");
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
                "STXChainAttestationBatch XChainCreateAccountAttesationBatch "
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
                    "XChainCreateAccountAttestationBatchElement that is not an "
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
    }
    {
        STArray createAtts{
            sfXChainCreateAccountAttestationBatch, creates_.size()};
        for (auto const& create : creates_)
            createAtts.push_back(create.toSTObject());
        createAtts.add(s);
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
