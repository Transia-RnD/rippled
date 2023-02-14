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

#include <ripple/basics/Log.h>
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

namespace Attestations {

TER
checkAttestationPublicKey(
    ReadView const& view,
    std::unordered_map<AccountID, std::uint32_t> const& signersList,
    AccountID const& attestationSignerAccount,
    PublicKey const& pk,
    beast::Journal j)
{
    if (!signersList.contains(attestationSignerAccount))
    {
        return tecNO_PERMISSION;
    }

    AccountID const accountFromPK = calcAccountID(pk);

    if (auto const sleAttestationSigningAccount =
            view.read(keylet::account(attestationSignerAccount)))
    {
        if (accountFromPK == attestationSignerAccount)
        {
            // master key
            if (sleAttestationSigningAccount->getFieldU32(sfFlags) &
                lsfDisableMaster)
            {
                JLOG(j.trace()) << "Attempt to add an attestation with "
                                   "disabled master key.";
                return tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR;
            }
        }
        else
        {
            // regular key
            if (std::optional<AccountID> regularKey =
                    (*sleAttestationSigningAccount)[~sfRegularKey];
                regularKey != accountFromPK)
            {
                if (!regularKey)
                {
                    JLOG(j.trace())
                        << "Attempt to add an attestation with "
                           "account present and non-present regular key.";
                }
                else
                {
                    JLOG(j.trace()) << "Attempt to add an attestation with "
                                       "account present and mismatched "
                                       "regular key/public key.";
                }
                return tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR;
            }
        }
    }
    else
    {
        // account does not exist.
        if (calcAccountID(pk) != attestationSignerAccount)
        {
            JLOG(j.trace())
                << "Attempt to add an attestation with non-existant account "
                   "and mismatched pk/account pair.";
            return tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR;
        }
    }

    return tesSUCCESS;
}

AttestationBase::AttestationBase(
    AccountID attestationSignerAccount_,
    PublicKey const& publicKey_,
    Buffer signature_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_)
    : attestationSignerAccount{attestationSignerAccount_}
    , publicKey{publicKey_}
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
               lhs.attestationSignerAccount,
               lhs.publicKey,
               lhs.signature,
               lhs.sendingAccount,
               lhs.sendingAmount,
               lhs.rewardAccount,
               lhs.wasLockingChainSend) ==
        std::tie(
               rhs.attestationSignerAccount,
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
    : attestationSignerAccount{o[sfAttestationSignerAccount]}
    , publicKey{o[sfPublicKey]}
    , signature{o[sfSignature]}
    , sendingAccount{o[sfAccount]}
    , sendingAmount{o[sfAmount]}
    , rewardAccount{o[sfAttestationRewardAccount]}
    , wasLockingChainSend{bool(o[sfWasLockingChainSend])}
{
}

AttestationBase::AttestationBase(Json::Value const& v)
    : attestationSignerAccount{Json::getOrThrow<AccountID>(
          v,
          sfAttestationSignerAccount)}
    , publicKey{Json::getOrThrow<PublicKey>(v, sfPublicKey)}
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
    o[sfAttestationSignerAccount] = attestationSignerAccount;
    o[sfPublicKey] = publicKey;
    o[sfSignature] = signature;
    o[sfAmount] = sendingAmount;
    o[sfAccount] = sendingAccount;
    o[sfAttestationRewardAccount] = rewardAccount;
    o[sfWasLockingChainSend] = wasLockingChainSend;
}

AttestationClaim::AttestationClaim(
    AccountID attestationSignerAccount_,
    PublicKey const& publicKey_,
    Buffer signature_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t claimID_,
    std::optional<AccountID> const& dst_)
    : AttestationBase(
          attestationSignerAccount_,
          publicKey_,
          std::move(signature_),
          sendingAccount_,
          sendingAmount_,
          rewardAccount_,
          wasLockingChainSend_)
    , claimID{claimID_}
    , dst{dst_}
{
}

AttestationClaim::AttestationClaim(
    STXChainBridge const& bridge,
    AccountID attestationSignerAccount_,
    PublicKey const& publicKey_,
    SecretKey const& secretKey_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t claimID_,
    std::optional<AccountID> const& dst_)
    : AttestationClaim{
          attestationSignerAccount_,
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
AttestationClaim::validAmounts() const
{
    return isLegalNet(sendingAmount);
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
    AccountID attestationSignerAccount_,
    PublicKey const& publicKey_,
    Buffer signature_,
    AccountID const& sendingAccount_,
    STAmount const& sendingAmount_,
    STAmount const& rewardAmount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::uint64_t createCount_,
    AccountID const& toCreate_)
    : AttestationBase(
          attestationSignerAccount_,
          publicKey_,
          std::move(signature_),
          sendingAccount_,
          sendingAmount_,
          rewardAccount_,
          wasLockingChainSend_)
    , createCount{createCount_}
    , toCreate{toCreate_}
    , rewardAmount{rewardAmount_}
{
}

AttestationCreateAccount::AttestationCreateAccount(
    STXChainBridge const& bridge,
    AccountID attestationSignerAccount_,
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
          attestationSignerAccount_,
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
AttestationCreateAccount::validAmounts() const
{
    return isLegalNet(rewardAmount) && isLegalNet(sendingAmount);
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

}  // namespace Attestations
}  // namespace ripple
