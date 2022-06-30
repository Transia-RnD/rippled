//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/paths/Flow.h>
#include <ripple/app/tx/impl/Sidechain.h>
#include <ripple/app/tx/impl/SignerEntries.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/basics/chrono.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/PaymentSandbox.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/STXChainClaimProof.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/XChainAttestations.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/st.h>

#include <unordered_map>
#include <unordered_set>

namespace ripple {

/*
    Sidechains

        Sidechains allow the transfer of assets from one chain to another. While
        the asset is used on the other chain, it is kept in an account on the
        mainchain.
        TODO: Finish this description

        TODO: Txn to change signatures
        TODO: Txn to remove the sidechain - track assets in trust?
        TODO: add trace and debug logging

*/

namespace {
TER
transferHelper(
    PaymentSandbox& psb,
    AccountID const& src,
    AccountID const& dst,
    STAmount const& amt,
    beast::Journal j)
{
    // TODO: handle DepositAuth
    //       handle dipping below reserve
    // TODO: Create a payment transaction instead of calling flow directly?
    // TODO: Set delivered amount?
    if (amt.native())
    {
        // TODO: Check reserve
        auto const sleSrc = psb.peek(keylet::account(src));
        assert(sleSrc);
        if (!sleSrc)
            return tecINTERNAL;

        if ((*sleSrc)[sfBalance] < amt)
        {
            return tecINSUFFICIENT_FUNDS;
        }
        auto const sleDst = psb.peek(keylet::account(dst));
        if (!sleDst)
        {
            // TODO
            return tecNO_DST;
        }
        (*sleSrc)[sfBalance] = (*sleSrc)[sfBalance] - amt;
        (*sleDst)[sfBalance] = (*sleDst)[sfBalance] + amt;
        psb.update(sleSrc);
        psb.update(sleDst);

        return tesSUCCESS;
    }

    auto const result = flow(
        psb,
        amt,
        src,
        dst,
        STPathSet{},
        /*default path*/ true,
        /*partial payment*/ false,
        /*owner pays transfer fee*/ true,
        /*offer crossing*/ false,
        /*limit quality*/ std::nullopt,
        /*sendmax*/ std::nullopt,
        j);

    return result.result();
}

// move the funds
// if funds moved, remove the claimid
// distribute the reward pool
TER
finalizeClaimHelper(
    PaymentSandbox& psb,
    STXChainBridge const& bridgeSpec,
    AccountID const& dst,
    STAmount const& sendingAmount,
    bool wasLockingChainSend,
    // sle for the claim id
    std::shared_ptr<SLE> const& sleCID,
    std::vector<AccountID> const& rewardAccounts,
    beast::Journal j)
{
    STAmount const thisChainAmount = [&] {
        STAmount r = sendingAmount;
        auto const issue = wasLockingChainSend ? bridgeSpec.issuingChainIssue()
                                               : bridgeSpec.lockingChainIssue();
        r.setIssue(issue);
        return r;
    }();
    auto const& thisDoor = wasLockingChainSend ? bridgeSpec.issuingChainDoor()
                                               : bridgeSpec.lockingChainDoor();

    auto const thTer = transferHelper(psb, thisDoor, dst, thisChainAmount, j);

    if (!isTesSuccess(thTer))
        return thTer;

    STAmount const rewardPool = (*sleCID)[sfSignatureReward];
    auto const cidOwner = (*sleCID)[sfAccount];
    {
        // Remove the sequence number
        // It's important that the sequence number is only removed if the
        // payment succeeds
        auto const sleOwner = psb.peek(keylet::account(cidOwner));
        auto const page = (*sleCID)[sfOwnerNode];
        if (!psb.dirRemove(
                keylet::ownerDir(cidOwner), page, sleCID->key(), true))
        {
            JLOG(j.fatal()) << "Unable to delete xchain seq number from owner.";
            return tefBAD_LEDGER;
        }

        // Remove the sequence number from the ledger
        psb.erase(sleCID);

        adjustOwnerCount(psb, sleOwner, -1, j);
    }

    if (!rewardAccounts.empty())
    {
        // distribute the reward pool
        STAmount const share = [&] {
            STAmount const den{rewardAccounts.size()};
            return divide(rewardPool, den, rewardPool.issue());
        }();
        STAmount distributed = rewardPool.zeroed();
        for (auto const& ra : rewardAccounts)
        {
            auto const thTer = transferHelper(psb, cidOwner, ra, share, j);
            if (thTer == tecINSUFFICIENT_FUNDS || thTer == tecINTERNAL)
                return thTer;

            if (isTesSuccess(thTer))
                distributed += share;

            // let txn succeed if error distributing rewards (other than
            // inability to pay)
        }

        if (distributed > rewardPool)
            return tecINTERNAL;
    }

    // TODO: Update the bridge balance
    return tesSUCCESS;
}

}  // namespace
//------------------------------------------------------------------------------

NotTEC
BridgeCreate::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const account = ctx.tx[sfAccount];
    auto const reward = ctx.tx[sfSignatureReward];
    auto const minAccountCreate = ctx.tx[~sfMinAccountCreateAmount];
    auto const bridge = ctx.tx[sfXChainBridge];
    if (bridge.lockingChainDoor() == bridge.issuingChainDoor())
    {
        return temEQUAL_DOOR_ACCOUNTS;
    }

    if (bridge.lockingChainDoor() != account &&
        bridge.issuingChainDoor() != account)
    {
        return temSIDECHAIN_NONDOOR_OWNER;
    }

    if (isXRP(bridge.lockingChainIssue()) != isXRP(bridge.issuingChainIssue()))
    {
        // Because ious and xrp have different numeric ranges, both the src and
        // dst issues must be both XRP or both IOU.
        return temSIDECHAIN_BAD_ISSUES;
    }

    if (!isXRP(reward) || reward.signum() <= 0)
    {
        return temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT;
    }

    if (minAccountCreate &&
        (!isXRP(*minAccountCreate) || minAccountCreate->signum() < 0))
    {
        return temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT;
    }

    return preflight2(ctx);
}

TER
BridgeCreate::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const bridge = ctx.tx[sfXChainBridge];

    if (ctx.view.read(keylet::bridge(bridge)))
    {
        return tecDUPLICATE;
    }

    bool const isLockingChain = (account == bridge.lockingChainDoor());

    if (isLockingChain)
    {
        if (!isXRP(bridge.lockingChainIssue()) &&
            !ctx.view.read(keylet::account(bridge.lockingChainIssue().account)))
        {
            return tecNO_ISSUER;
        }
    }
    else
    {
        // issuing chain
        if (!isXRP(bridge.issuingChainIssue()) &&
            !ctx.view.read(keylet::account(bridge.issuingChainIssue().account)))
        {
            return tecNO_ISSUER;
        }
    }

    {
        // Check reserve
        auto const sle = ctx.view.read(keylet::account(account));
        if (!sle)
            return terNO_ACCOUNT;

        auto const balance = (*sle)[sfBalance];
        auto const reserve =
            ctx.view.fees().accountReserve((*sle)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    return tesSUCCESS;
}

TER
BridgeCreate::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const bridge = ctx_.tx[sfXChainBridge];
    auto const reward = ctx_.tx[sfSignatureReward];
    auto const minAccountCreate = ctx_.tx[~sfMinAccountCreateAmount];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    Keylet const bridgeKeylet = keylet::bridge(bridge);
    auto const sleSC = std::make_shared<SLE>(bridgeKeylet);

    (*sleSC)[sfAccount] = account;
    (*sleSC)[sfSignatureReward] = reward;
    if (minAccountCreate)
        (*sleSC)[sfMinAccountCreateAmount] = *minAccountCreate;
    (*sleSC)[sfXChainBridge] = bridge;
    (*sleSC)[sfXChainClaimID] = 0;
    (*sleSC)[sfXChainAccountCreateCount] = 0;
    (*sleSC)[sfXChainAccountClaimCount] = 0;
    // TODO: Initialize the balance

    // Add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account), bridgeKeylet, describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*sleSC)[sfOwnerNode] = *page;
    }

    adjustOwnerCount(ctx_.view(), sleAcc, 1, ctx_.journal);

    ctx_.view().insert(sleSC);
    ctx_.view().update(sleAcc);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainClaim::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    STXChainBridge const bridgeSpec = ctx.tx[sfXChainBridge];
    auto const amount = ctx.tx[sfAmount];
    AccountID const account = ctx.tx[sfAccount];

    if (amount.signum() <= 0 ||
        amount.issue() != bridgeSpec.lockingChainIssue() ||
        amount.issue() != bridgeSpec.issuingChainIssue())
    {
        return temBAD_AMOUNT;
    }

    return preflight2(ctx);
}

TER
XChainClaim::preclaim(PreclaimContext const& ctx)
{
    AccountID const account = ctx.tx[sfAccount];
    STXChainBridge bridgeSpec = ctx.tx[sfXChainBridge];
    STAmount const& thisChainAmount = ctx.tx[sfAmount];
    auto const claimID = ctx.tx[sfXChainClaimID];

    auto const sleB = ctx.view.read(keylet::bridge(bridgeSpec));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    if (!ctx.view.read(keylet::account(ctx.tx[sfDestination])))
    {
        return tecNO_DST;
    }

    bool isLockingChain = false;
    {
        auto const thisDoor = (*sleB)[sfAccount];
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    {
        // Check that the amount specified matches the expected issue

        if (isLockingChain)
        {
            if (bridgeSpec.lockingChainIssue() != thisChainAmount.issue())
                return tecBAD_XCHAIN_TRANSFER_ISSUE;
        }
        else
        {
            if (bridgeSpec.issuingChainIssue() != thisChainAmount.issue())
                return tecBAD_XCHAIN_TRANSFER_ISSUE;
        }
    }

    if (isXRP(bridgeSpec.lockingChainIssue()) !=
        isXRP(bridgeSpec.issuingChainIssue()))
    {
        // Should have been caught when creating the bridge
        // Detect here so `otherChainAmount` doesn't switch from IOU -> XRP
        // and the numeric issues that need to be addressed with that.
        return tecINTERNAL;
    }

    auto const otherChainAmount = [&]() -> STAmount {
        STAmount r(thisChainAmount);
        if (isLockingChain)
            r.setIssue(bridgeSpec.issuingChainIssue());
        else
            r.setIssue(bridgeSpec.lockingChainIssue());
        return r;
    }();

    auto const sleCID =
        ctx.view.read(keylet::xChainClaimID(bridgeSpec, claimID));
    {
        // Check that the sequence number is owned by the sender of this
        // transaction
        if (!sleCID)
        {
            return tecXCHAIN_NO_CLAIM_ID;
        }

        if ((*sleCID)[sfAccount] != account)
        {
            // Sequence number isn't owned by the sender of this transaction
            return tecXCHAIN_BAD_CLAIM_ID;
        }
    }

    {
        // Check that the claim id has a quorum for the current signatures on
        // the account
        auto const sleSigners = ctx.view.read(keylet::signers(account));
        if (!sleSigners)
            return tecXCHAIN_NO_SIGNERS_LIST;

        XChainAttestations const attestations{
            sleCID->getFieldArray(sfXChainAttestations)};

        auto const accountSigners =
            SignerEntries::deserialize(*sleSigners, ctx.j, "ledger");

        if (!accountSigners)
        {
            return tecINTERNAL;
        }

        auto const quorum = (*sleSigners)[sfSignerQuorum];

        auto const weight = [&]() -> std::uint32_t {
            auto const attMap =
                [&]() -> std::unordered_map<
                          AccountID,
                          XChainAttestations::Attestation const*> {
                std::unordered_map<
                    AccountID,
                    XChainAttestations::Attestation const*>
                    r;
                for (auto const& a : attestations)
                {
                    r[a.keyAccount] = &a;
                }
                return r;
            }();

            std::uint32_t w = 0;
            for (auto const& as : *accountSigners)
            {
                auto it = attMap.find(as.account);
                if (it == attMap.end() ||
                    it->second->amount != otherChainAmount)
                    continue;
                w += as.weight;
            }
            return w;
        }();

        if (weight < quorum)
        {
            return tecXCHAIN_CLAIM_NO_QUORUM;
        }
    }

    return tesSUCCESS;
}

TER
XChainClaim::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    AccountID const account = ctx_.tx[sfAccount];
    auto const dst = ctx_.tx[sfDestination];
    STXChainBridge bridgeSpec = ctx_.tx[sfXChainBridge];
    STAmount const& thisChainAmount = ctx_.tx[sfAmount];
    auto const claimID = ctx_.tx[sfXChainClaimID];

    auto const sleAcc = psb.peek(keylet::account(account));
    auto const sleB = psb.peek(keylet::bridge(bridgeSpec));
    auto const sleCID = psb.peek(keylet::xChainClaimID(bridgeSpec, claimID));

    if (!(sleB && sleCID && sleAcc))
        return tecINTERNAL;

    auto const thisDoor = (*sleB)[sfAccount];

    auto const thTer =
        transferHelper(psb, thisDoor, dst, thisChainAmount, ctx_.journal);

    if (!isTesSuccess(thTer))
        return thTer;

    {
        // Remove the sequence number
        // It's important that the sequence number is only removed if the
        // payment succeeds
        auto const page = (*sleCID)[sfOwnerNode];
        if (!psb.dirRemove(
                keylet::ownerDir(account), page, sleCID->key(), true))
        {
            JLOG(j_.fatal())
                << "Unable to delete xchain seq number from owner.";
            return tefBAD_LEDGER;
        }

        // Remove the sequence number from the ledger
        psb.erase(sleCID);
    }

    // TODO: Update the bridge balance

    adjustOwnerCount(psb, sleAcc, -1, j_);

    psb.apply(ctx_.rawView());
    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainCommit::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const amount = ctx.tx[sfAmount];

    if (amount.signum() <= 0)
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
XChainCommit::preclaim(PreclaimContext const& ctx)
{
    auto const sidechain = ctx.tx[sfXChainBridge];
    auto const amount = ctx.tx[sfAmount];

    auto const sleSC = ctx.view.read(keylet::bridge(sidechain));
    if (!sleSC)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    auto const thisDoor = (*sleSC)[sfAccount];

    bool isLockingChain = false;
    {
        if (thisDoor == sidechain.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == sidechain.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    if (isLockingChain)
    {
        if (sidechain.lockingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;
    }
    else
    {
        if (sidechain.issuingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;
    }

    return tesSUCCESS;
}

TER
XChainCommit::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const amount = ctx_.tx[sfAmount];
    auto const bridge = ctx_.tx[sfXChainBridge];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleSC = psb.read(keylet::bridge(bridge));
    if (!sleSC)
        return tecINTERNAL;

    auto const dst = (*sleSC)[sfAccount];

    auto const thTer = transferHelper(psb, account, dst, amount, ctx_.journal);
    if (!isTesSuccess(thTer))
        return thTer;

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainCreateClaimID::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const reward = ctx.tx[sfSignatureReward];

    if (!isXRP(reward) || reward.signum() < 0)
        return temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT;

    return preflight2(ctx);
}

TER
XChainCreateClaimID::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const bridgeSpec = ctx.tx[sfXChainBridge];
    auto const bridge = ctx.view.read(keylet::bridge(bridgeSpec));

    if (!bridge)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    // Check that the reward matches
    auto const reward = ctx.tx[sfSignatureReward];

    if (reward != (*bridge)[sfSignatureReward])
    {
        return tecXCHAIN_REWARD_MISMATCH;
    }

    {
        // Check reserve
        auto const sle = ctx.view.read(keylet::account(account));
        if (!sle)
            return terNO_ACCOUNT;

        auto const balance = (*sle)[sfBalance];
        auto const reserve =
            ctx.view.fees().accountReserve((*sle)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    return tesSUCCESS;
}

TER
XChainCreateClaimID::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const bridge = ctx_.tx[sfXChainBridge];
    auto const reward = ctx_.tx[sfSignatureReward];
    auto const otherChainAccount = ctx_.tx[sfOtherChainAccount];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    auto const sleB = ctx_.view().peek(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    std::uint32_t const claimID = (*sleB)[sfXChainClaimID] + 1;
    if (claimID == 0)
        return tecINTERNAL;  // overflow

    (*sleB)[sfXChainClaimID] = claimID;

    Keylet const seqKeylet = keylet::xChainClaimID(bridge, claimID);
    if (ctx_.view().read(seqKeylet))
        return tecINTERNAL;  // already checked out!?!

    auto const sleQ = std::make_shared<SLE>(seqKeylet);

    (*sleQ)[sfAccount] = account;
    (*sleQ)[sfXChainBridge] = bridge;
    (*sleQ)[sfXChainClaimID] = claimID;
    (*sleQ)[sfOtherChainAccount] = otherChainAccount;
    (*sleQ)[sfSignatureReward] = reward;
    sleQ->setFieldArray(sfXChainAttestations, STArray{sfXChainAttestations});

    // Add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account), seqKeylet, describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*sleQ)[sfOwnerNode] = *page;
    }

    adjustOwnerCount(ctx_.view(), sleAcc, 1, ctx_.journal);

    ctx_.view().insert(sleQ);
    ctx_.view().update(sleB);
    ctx_.view().update(sleAcc);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainAddAttestation::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    STXChainAttestationBatch const batch = ctx.tx[sfXChainAttestationBatch];

    if (batch.numAttestations() > 8)
    {
        return temXCHAIN_TOO_MANY_ATTESTATIONS;
    }

    if (!batch.verify())
        return temBAD_XCHAIN_PROOF;

    auto const& bridgeSpec = batch.bridge();
    // If any attestation is for a negative amount or for an amount
    // that isn't expected by the given bridge, the whole transaction is bad
    auto checkAmount = [&](auto const& att) -> bool {
        if (att.sendingAmount.signum() <= 0)
            return false;
        auto const expectedIssue = att.wasLockingChainSend
            ? bridgeSpec.lockingChainIssue()
            : bridgeSpec.issuingChainIssue();
        if (att.sendingAmount.issue() != expectedIssue)
            return false;
        return true;
    };

    auto const& creates = batch.creates();
    auto const& claims = batch.claims();
    if (!(std::all_of(creates.begin(), creates.end(), checkAmount) &&
          std::all_of(claims.begin(), claims.end(), checkAmount)))
    {
        return temBAD_XCHAIN_PROOF;
    }

    return preflight2(ctx);
}

TER
XChainAddAttestation::preclaim(PreclaimContext const& ctx)
{
    // TBD
    return tesSUCCESS;
}

TER
XChainAddAttestation::applyClaim(
    AttestationBatch::AttestationClaim const& claimAtt,
    STXChainBridge const& bridgeSpec,
    std::unordered_map<AccountID, std::uint32_t> const& signersList,
    std::uint32_t quorum)
{
    PaymentSandbox psb(&ctx_.view());

    auto const sleCID =
        psb.peek(keylet::xChainClaimID(bridgeSpec, claimAtt.claimID));
    if (!sleCID)
        return tecXCHAIN_NO_CLAIM_ID;

    if (!signersList.count(calcAccountID(claimAtt.publicKey)))
    {
        return tecXCHAIN_PROOF_UNKNOWN_KEY;
    }

    AccountID const otherChainAccount = (*sleCID)[sfOtherChainAccount];
    if (claimAtt.sendingAccount != otherChainAccount)
    {
        return tecXCHAIN_SENDING_ACCOUNT_MISMATCH;
    }

    XChainAttestations curAtts{sleCID->getFieldArray(sfXChainAttestations)};

    auto const rewardAccounts =
        curAtts.onNewAttestation(claimAtt, quorum, signersList);

    if (rewardAccounts && claimAtt.dst)
    {
        auto const r = finalizeClaimHelper(
            psb,
            bridgeSpec,
            *claimAtt.dst,
            claimAtt.sendingAmount,
            claimAtt.wasLockingChainSend,
            sleCID,
            *rewardAccounts,
            ctx_.journal);
        if (!isTesSuccess(r))
            return r;
    }
    else
    {
        // update the claim id
        sleCID->setFieldArray(sfXChainAttestations, curAtts.toSTArray());
        psb.update(sleCID);
    }

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

TER
XChainAddAttestation::doApply()
{
    STXChainAttestationBatch const batch = ctx_.tx[sfXChainAttestationBatch];

    std::vector<TER> applyRestuls;
    applyRestuls.reserve(batch.numAttestations());

    auto const& bridgeSpec = batch.bridge();

    auto const sleB = ctx_.view().read(keylet::bridge(bridgeSpec));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }
    auto const thisDoor = (*sleB)[sfAccount];
    if (!thisDoor)
        return tecINTERNAL;

    // map from account id to weights
    auto const [signersList, quorum, slTer] =
        [&]() -> std::tuple<
                  std::unordered_map<AccountID, std::uint32_t>,
                  std::uint32_t,
                  TER> {
        std::unordered_map<AccountID, std::uint32_t> r;
        std::uint32_t q = std::numeric_limits<std::uint32_t>::max();

        auto const sleS = ctx_.view().read(keylet::signers((*sleB)[sfAccount]));
        if (!sleS)
            return {r, q, tecXCHAIN_NO_SIGNERS_LIST};
        q = (*sleS)[sfSignerQuorum];

        auto const accountSigners =
            SignerEntries::deserialize(*sleS, ctx_.journal, "ledger");

        if (!accountSigners)
        {
            return {r, q, tecINTERNAL};
        }

        for (auto const& as : *accountSigners)
        {
            r[as.account] = as.weight;
        }

        return {std::move(r), q, tesSUCCESS};
    }();

    if (!isTesSuccess(slTer))
        return slTer;

    for (auto const& createAtt : batch.creates())
    {
        (void)createAtt;
        // TODO
    }

    for (auto const& claimAtt : batch.claims())
    {
        auto const r = applyClaim(claimAtt, bridgeSpec, signersList, quorum);

        if (r == tecINTERNAL)
            return r;

        applyRestuls.push_back(r);
    }

    if (applyRestuls.size() == 1)
        return applyRestuls[0];

    if (std::any_of(applyRestuls.begin(), applyRestuls.end(), isTesSuccess))
        return tesSUCCESS;

    // TODO: What to return here?
    return applyRestuls[0];
}

//------------------------------------------------------------------------------

NotTEC
SidechainXChainCreateAccount::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const amount = ctx.tx[sfAmount];

    if (amount.signum() <= 0 || !amount.native())
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
SidechainXChainCreateAccount::preclaim(PreclaimContext const& ctx)
{
    auto const sidechain = ctx.tx[sfXChainBridge];
    auto const amount = ctx.tx[sfAmount];

    auto const sleSC = ctx.view.read(keylet::bridge(sidechain));
    if (!sleSC)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    auto const thisDoor = (*sleSC)[sfAccount];

    bool isSrcChain = false;
    {
        if (thisDoor == sidechain.lockingChainDoor())
            isSrcChain = true;
        else if (thisDoor == sidechain.issuingChainDoor())
            isSrcChain = false;
        else
            return tecINTERNAL;
    }

    if (isSrcChain)
    {
        if (sidechain.lockingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(sidechain.issuingChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }
    else
    {
        if (sidechain.issuingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(sidechain.lockingChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }

    return tesSUCCESS;
}

TER
SidechainXChainCreateAccount::doApply()
{
    // TODO: remove code duplication with XChainTransfer::doApply
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const amount = ctx_.tx[sfAmount];
    auto const sidechain = ctx_.tx[sfXChainBridge];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleSC = psb.read(keylet::bridge(sidechain));
    if (!sleSC)
        return tecINTERNAL;

    auto const dst = (*sleSC)[sfAccount];

    // TODO: Make sure we turn ters into tecs or can spam for free!
    auto const result = flow(
        psb,
        amount,
        account,
        dst,
        STPathSet{},
        /*default path*/ true,
        /*partial payment*/ false,
        /*owner pays transfer fee*/ true,
        /*offer crossing*/ false,
        /*limit quality*/ std::nullopt,
        /*sendmax*/ std::nullopt,
        ctx_.journal);

    if (isTesSuccess(result.result()))
    {
        psb.apply(ctx_.rawView());
    }

    return result.result();
}

//------------------------------------------------------------------------------

NotTEC
SidechainXChainClaimAccount::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureXChainBridge))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const amount = ctx.tx[sfAmount];
    if (amount.signum() <= 0)
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
SidechainXChainClaimAccount::preclaim(PreclaimContext const& ctx)
{
    auto const amount = ctx.tx[sfAmount];
    STXChainBridge const& sidechain = ctx.tx[sfXChainBridge];

    auto const sleSC = ctx.view.read(keylet::bridge(sidechain));
    if (!sleSC)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    if (ctx.view.read(keylet::account(ctx.tx[sfDestination])))
    {
        return tecXCHAIN_CLAIM_ACCOUNT_DST_EXISTS;
    }

    {
        // Check that the amount specified in the proof matches the expected
        // issue
        auto const thisDoor = (*sleSC)[sfAccount];

        bool isSrcChain = false;
        {
            if (thisDoor == sidechain.lockingChainDoor())
                isSrcChain = true;
            else if (thisDoor == sidechain.issuingChainDoor())
                isSrcChain = false;
            else
                return tecINTERNAL;
        }

        if (isSrcChain)
        {
            if (!isXRP(sidechain.issuingChainIssue()))
                return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
        }
        else
        {
            if (!isXRP(sidechain.lockingChainIssue()))
                return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
        }
    }

    return tesSUCCESS;
}

TER
SidechainXChainClaimAccount::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const otherChainAmount = ctx_.tx[sfAmount];
    auto const dst = ctx_.tx[sfDestination];
    STXChainBridge const& sidechain = ctx_.tx[sfXChainBridge];

    auto const sleAcc = psb.peek(keylet::account(account));
    auto const sleSC = psb.read(keylet::bridge(sidechain));

    if (!(sleSC && sleAcc))
        return tecINTERNAL;

    auto const thisDoor = (*sleSC)[sfAccount];

    Issue const thisChainIssue = [&] {
        bool const isSrcChain = (thisDoor == sidechain.lockingChainDoor());
        return isSrcChain ? sidechain.lockingChainIssue()
                          : sidechain.issuingChainIssue();
    }();

    if (otherChainAmount.native() != isXRP(thisChainIssue))
    {
        // Should have been caught when creating the sidechain
        return tecINTERNAL;
    }

    STAmount const thisChainAmount = [&] {
        STAmount r{otherChainAmount};
        r.setIssue(thisChainIssue);
        return r;
    }();

    if (!thisChainAmount.native())
        return tecINTERNAL;

    // TODO: Make sure we turn ters into tecs or can spam for free!
    auto const result = flow(
        psb,
        thisChainAmount,
        thisDoor,
        dst,
        STPathSet{},
        /*default path*/ true,
        /*partial payment*/ false,
        /*owner pays transfer fee*/ true,
        /*offer crossing*/ false,
        /*limit quality*/ std::nullopt,
        /*sendmax*/ std::nullopt,
        ctx_.journal);

    if (!isTesSuccess(result.result()))
        return result.result();

    // TODO: Can I make the account non-deletable?

    psb.apply(ctx_.rawView());
    return tesSUCCESS;
}

//------------------------------------------------------------------------------

}  // namespace ripple
