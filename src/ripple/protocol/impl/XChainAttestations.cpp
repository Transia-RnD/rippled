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

#include <ripple/protocol/XChainAttestations.h>

#include <ripple/basics/StringUtilities.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/jss.h>

#include <optional>

namespace ripple {

bool
operator==(
    XChainAttestations::Attestation const& lhs,
    XChainAttestations::Attestation const& rhs)
{
    return std::tie(
               lhs.keyAccount,
               lhs.amount,
               lhs.rewardAccount,
               lhs.wasLockingChainSend,
               lhs.dst) ==
        std::tie(
               rhs.keyAccount,
               rhs.amount,
               rhs.rewardAccount,
               rhs.wasLockingChainSend,
               rhs.dst);
}
bool
operator!=(
    XChainAttestations::Attestation const& lhs,
    XChainAttestations::Attestation const& rhs)
{
    return !operator==(lhs, rhs);
}

XChainAttestations::Attestation::Attestation(
    AccountID const& keyAccount_,
    STAmount const& amount_,
    AccountID const& rewardAccount_,
    bool wasLockingChainSend_,
    std::optional<AccountID> const& dst_)
    : keyAccount(keyAccount_)
    , amount(sfAmount, amount_)
    , rewardAccount(rewardAccount_)
    , wasLockingChainSend(wasLockingChainSend_)
    , dst(dst_)
{
}
XChainAttestations::Attestation::Attestation(
    STAccount const& keyAccount_,
    STAmount const& amount_,
    STAccount const& rewardAccount_,
    bool wasLockingChainSend_,
    std::optional<STAccount> const& dst_)
    : Attestation{
          keyAccount_.value(),
          amount_,
          rewardAccount_.value(),
          wasLockingChainSend_,
          dst_ ? std::optional<AccountID>{dst_->value()} : std::nullopt}
{
}

XChainAttestations::Attestation::Attestation(STObject const& o)
    : Attestation{
          o[sfAttestationSignerAccount],
          o[sfAmount],
          o[sfAttestationRewardAccount],
          o[sfWasLockingChainSend] != 0,
          o[~sfDestination]} {};

XChainAttestations::Attestation::Attestation(
    AttestationBatch::AttestationClaim const& claimAtt)
    : Attestation{
          calcAccountID(claimAtt.publicKey),
          claimAtt.sendingAmount,
          claimAtt.rewardAccount,
          claimAtt.wasLockingChainSend,
          claimAtt.dst}
{
}

STObject
XChainAttestations::Attestation::toSTObject() const
{
    STObject o{sfXChainProofSig};
    o[sfAttestationSignerAccount] =
        STAccount{sfAttestationSignerAccount, keyAccount};
    o[sfAmount] = STAmount{sfAmount, amount};
    o[sfAttestationRewardAccount] =
        STAccount{sfAttestationRewardAccount, rewardAccount};
    o[sfWasLockingChainSend] = wasLockingChainSend;
    if (dst)
        o[sfDestination] = STAccount{sfDestination, *dst};
    return o;
}

XChainAttestations::XChainAttestations(AttCollection&& atts)
    : attestations_{std::move(atts)}
{
}

XChainAttestations::AttCollection::const_iterator
XChainAttestations::begin() const
{
    return attestations_.begin();
}

XChainAttestations::AttCollection::const_iterator
XChainAttestations::end() const
{
    return attestations_.end();
}

XChainAttestations::XChainAttestations(Json::Value const& v)
{
    // TODO: Rewrite this whole thing in the style of the
    // STXChainAttestationBatch
    if (!v.isObject())
    {
        Throw<std::runtime_error>(
            "XChainAttestations can only be specified with a 'object' Json "
            "value");
    }
    // TODO: Throw is a field is not present
    // TODO: Throw if too many signatures
    attestations_ = [&] {
        auto const jAtts = v[jss::attestations];
        std::vector<XChainAttestations::Attestation> r;
        r.reserve(jAtts.size());
        for (auto const& a : jAtts)
        {
            auto const signingKeyB58 = a[jss::signing_key].asString();
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
            STAmount const amt = amountFromJson(sfAmount, a[jss::amount]);

            Json::Value const attrRewardAccStr =
                v[jss::attestation_reward_account];
            if (!attrRewardAccStr.isString())
            {
                Throw<std::runtime_error>(
                    "XChainAttestations attestation_reward_account must be a "
                    "string Json value");
            }
            auto const attrRewardAcc =
                parseBase58<AccountID>(attrRewardAccStr.asString());
            if (!attrRewardAcc)
            {
                Throw<std::runtime_error>(
                    "XChainAttestations attestation_reward_account must be a "
                    "valid account");
            }
            std::optional<AccountID> dst;
            if (v.isMember(jss::destination))
            {
                Json::Value const dstAccStr = v[jss::destination];
                if (!dstAccStr.isString())
                {
                    Throw<std::runtime_error>(
                        "XChainAttestations destination must be a string "
                        "Json value");
                }
                dst = parseBase58<AccountID>(dstAccStr.asString());
                if (!dst)
                {
                    Throw<std::runtime_error>(
                        "XChainAttestations attestation_reward_account must "
                        "be a valid account");
                }
            }
            bool wasLockingChainSend = false;
            if (!v.isMember(sfWasLockingChainSend.getJsonName()))
            {
                Throw<std::runtime_error>(
                    "XChainAttestations missing field: WasLockingChainSend");
            }
            wasLockingChainSend =
                v[sfWasLockingChainSend.getJsonName()].asBool();

            r.emplace_back(
                calcAccountID(*pk),
                amt,
                *attrRewardAcc,
                wasLockingChainSend,
                dst);
        }
        return r;
    }();
}

XChainAttestations::XChainAttestations(STArray const& arr)
{
    attestations_.reserve(arr.size());
    for (auto const& o : arr)
        attestations_.emplace_back(o);
}

STArray
XChainAttestations::toSTArray() const
{
    STArray r{sfXChainAttestations, attestations_.size()};
    for (auto const& e : attestations_)
        r.emplace_back(e.toSTObject());
    return r;
}

std::optional<std::vector<AccountID>>
XChainAttestations::onNewAttestation(
    AttestationBatch::AttestationClaim const& claimAtt,
    std::uint32_t quorum,
    std::unordered_map<AccountID, std::uint32_t> const& signersList)
{
    // TODO: check if claimAtt is part of the signersList? (should already be
    // done)
    {
        // Remove attestations that are no longer part of the signers list
        auto i = std::remove_if(
            attestations_.begin(), attestations_.end(), [&](auto const& a) {
                return !signersList.count(a.keyAccount);
            });
        attestations_.erase(i, attestations_.end());
    }

    {
        // Add the new attestation, but only if it is not currently part of the
        // collection or the amount it attests to is greater or equal (the equal
        // case can be used to change the reward account)
        //
        auto const claimSigningAccount = calcAccountID(claimAtt.publicKey);
        if (auto i = std::find_if(
                attestations_.begin(),
                attestations_.end(),
                [&](auto const& a) {
                    return a.keyAccount == claimSigningAccount;
                });
            i != attestations_.end())
        {
            // existing attestation
            if (claimAtt.sendingAmount >= i->amount)
            {
                // replace old attestation with new attestion
                *i = Attestation{claimAtt};
            }
        }
        {
            attestations_.emplace_back(claimAtt);
        }
    }

    {
        // Check if we have quorum for the amount on specified on the new
        // claimAtt
        std::vector<AccountID> rewardAccounts;
        rewardAccounts.reserve(attestations_.size());
        std::uint32_t weight = 0;
        for (auto const& a : attestations_)
        {
            if (a.amount != claimAtt.sendingAmount || a.dst != claimAtt.dst ||
                a.wasLockingChainSend != claimAtt.wasLockingChainSend)
                continue;
            auto i = signersList.find(a.keyAccount);
            if (i == signersList.end())
            {
                assert(0);  // should have already been checked
                continue;
            }
            weight += i->second;
            rewardAccounts.push_back(a.rewardAccount);
        }

        if (weight >= quorum)
            return rewardAccounts;
    }

    return std::nullopt;
};

}  // namespace ripple
