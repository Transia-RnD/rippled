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
#include <ripple/app/tx/impl/SignerEntries.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/app/tx/impl/XChainBridge.h>
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
#include <ripple/protocol/STObject.h>
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
        auto const dstK = keylet::account(dst);
        auto sleDst = psb.peek(dstK);
        if (!sleDst)
        {
            // Create the account.
            std::uint32_t const seqno{
                psb.rules().enabled(featureDeletableAccounts) ? psb.seq() : 1};

            sleDst = std::make_shared<SLE>(dstK);
            sleDst->setAccountID(sfAccount, dst);
            sleDst->setFieldU32(sfSequence, seqno);

            psb.insert(sleDst);
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
    AccountID const& rewardPoolSrc,
    STAmount const& rewardPool,
    std::vector<AccountID> const& rewardAccounts,
    bool wasLockingChainSend,
    // sle for the claim id (may be an NULL or XChainClaimID or
    // XChainCreateAccountClaimID). Don't read fields that aren't in common with
    // those two types and always check for NULL. Remove on success (if not
    // null).
    std::shared_ptr<SLE> const& sleCID,
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

    if (sleCID)
    {
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
                JLOG(j.fatal())
                    << "Unable to delete xchain seq number from owner.";
                return tefBAD_LEDGER;
            }

            // Remove the sequence number from the ledger
            psb.erase(sleCID);

            adjustOwnerCount(psb, sleOwner, -1, j);
        }
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
            auto const thTer = transferHelper(psb, rewardPoolSrc, ra, share, j);
            if (thTer == tecINSUFFICIENT_FUNDS || thTer == tecINTERNAL)
                return thTer;

            if (isTesSuccess(thTer))
                distributed += share;

            // TODO: Update spec - remaining reward pool stays with the pool
            // source

            // let txn succeed if error distributing rewards (other than
            // inability to pay)
        }

        if (distributed > rewardPool)
            return tecINTERNAL;
    }

    // TODO: Update the bridge balance
    return tesSUCCESS;
}

std::tuple<std::unordered_map<AccountID, std::uint32_t>, std::uint32_t, TER>
getSignersListAndQuorum(ApplyView& view, SLE const& sleB, beast::Journal j)
{
    std::unordered_map<AccountID, std::uint32_t> r;
    std::uint32_t q = std::numeric_limits<std::uint32_t>::max();

    auto const sleS = view.read(keylet::signers(sleB[sfAccount]));
    if (!sleS)
        return {r, q, tecXCHAIN_NO_SIGNERS_LIST};
    q = (*sleS)[sfSignerQuorum];

    auto const accountSigners = SignerEntries::deserialize(*sleS, j, "ledger");

    if (!accountSigners)
    {
        return {r, q, tecINTERNAL};
    }

    for (auto const& as : *accountSigners)
    {
        r[as.account] = as.weight;
    }

    return {std::move(r), q, tesSUCCESS};
};
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
    auto const sleB = std::make_shared<SLE>(bridgeKeylet);

    (*sleB)[sfAccount] = account;
    (*sleB)[sfSignatureReward] = reward;
    if (minAccountCreate)
        (*sleB)[sfMinAccountCreateAmount] = *minAccountCreate;
    (*sleB)[sfXChainBridge] = bridge;
    (*sleB)[sfXChainClaimID] = 0;
    (*sleB)[sfXChainAccountCreateCount] = 0;
    (*sleB)[sfXChainAccountClaimCount] = 0;
    // TODO: Initialize the balance

    // Add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account), bridgeKeylet, describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*sleB)[sfOwnerNode] = *page;
    }

    adjustOwnerCount(ctx_.view(), sleAcc, 1, ctx_.journal);

    ctx_.view().insert(sleB);
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

    auto const thisDoor = (*sleB)[sfAccount];
    bool isLockingChain = false;
    {
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

    // quorum is checked in `doApply`
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

    AccountID const thisDoor = (*sleB)[sfAccount];
    bool isLockingChain = false;
    {
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    auto const sendingAmount = [&]() -> STAmount {
        STAmount r(thisChainAmount);
        if (isLockingChain)
            r.setIssue(bridgeSpec.issuingChainIssue());
        else
            r.setIssue(bridgeSpec.lockingChainIssue());
        return r;
    }();

    auto const wasLockingChainSend = !isLockingChain;

    auto const [signersList, quorum, slTer] =
        getSignersListAndQuorum(ctx_.view(), *sleB, ctx_.journal);

    if (!isTesSuccess(slTer))
        return slTer;

    XChainClaimAttestations curAtts{
        sleCID->getFieldArray(sfXChainClaimAttestations)};

    auto claimR = curAtts.onClaim(
        sendingAmount, wasLockingChainSend, quorum, signersList);
    if (!claimR.has_value())
        return claimR.error();

    auto const& rewardAccounts = claimR.value();
    auto const& rewardPoolSrc = (*sleCID)[sfAccount];

    auto const r = finalizeClaimHelper(
        psb,
        bridgeSpec,
        dst,
        sendingAmount,
        rewardPoolSrc,
        (*sleCID)[sfSignatureReward],
        rewardAccounts,
        wasLockingChainSend,
        sleCID,
        ctx_.journal);
    if (!isTesSuccess(r))
        return r;

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

    auto const sleB = ctx.view.read(keylet::bridge(sidechain));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    auto const thisDoor = (*sleB)[sfAccount];

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

    auto const sleB = psb.read(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    auto const dst = (*sleB)[sfAccount];

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
    sleQ->setFieldArray(
        sfXChainClaimAttestations, STArray{sfXChainClaimAttestations});

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

// TODO: Do all the claims for a claimid as a batch
// If we don't do this some of the signers may miss out on the reward because a
// quorum may be reached before we get to those signatures
TER
XChainAddAttestation::applyClaim(
    AttestationBatch::AttestationClaim const& att,
    STXChainBridge const& bridgeSpec,
    std::unordered_map<AccountID, std::uint32_t> const& signersList,
    std::uint32_t quorum)
{
    PaymentSandbox psb(&ctx_.view());

    auto const sleCID =
        psb.peek(keylet::xChainClaimID(bridgeSpec, att.claimID));
    if (!sleCID)
        return tecXCHAIN_NO_CLAIM_ID;

    if (!signersList.count(calcAccountID(att.publicKey)))
    {
        return tecXCHAIN_PROOF_UNKNOWN_KEY;
    }

    AccountID const otherChainAccount = (*sleCID)[sfOtherChainAccount];
    if (att.sendingAccount != otherChainAccount)
    {
        return tecXCHAIN_SENDING_ACCOUNT_MISMATCH;
    }

    XChainClaimAttestations curAtts{
        sleCID->getFieldArray(sfXChainClaimAttestations)};

    auto const rewardAccounts =
        curAtts.onNewAttestation(att, quorum, signersList);

    if (rewardAccounts && att.dst)
    {
        auto const& rewardPoolSrc = (*sleCID)[sfAccount];
        auto const r = finalizeClaimHelper(
            psb,
            bridgeSpec,
            *att.dst,
            att.sendingAmount,
            rewardPoolSrc,
            (*sleCID)[sfSignatureReward],
            *rewardAccounts,
            att.wasLockingChainSend,
            sleCID,
            ctx_.journal);
        if (!isTesSuccess(r))
            return r;
    }
    else
    {
        // update the claim id
        sleCID->setFieldArray(sfXChainClaimAttestations, curAtts.toSTArray());
        psb.update(sleCID);
    }

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

TER
XChainAddAttestation::applyCreateAccountAtt(
    AttestationBatch::AttestationCreateAccount const& att,
    AccountID const& doorAccount,
    Keylet const& doorK,
    STXChainBridge const& bridgeSpec,
    Keylet const& bridgeK,
    std::unordered_map<AccountID, std::uint32_t> const& signersList,
    std::uint32_t quorum)
{
    PaymentSandbox psb(&ctx_.view());

    auto const sleDoor = psb.peek(doorK);
    if (!sleDoor)
        return tecINTERNAL;

    auto const sleB = psb.peek(bridgeK);
    if (!sleB)
        return tecINTERNAL;

    std::int64_t const claimCount = (*sleB)[sfXChainAccountClaimCount];

    if (att.createCount <= claimCount)
    {
        return tecXCHAIN_ACCOUNT_CREATE_PAST;
    }
    if (att.createCount >= claimCount + 128)
    {
        // Limit the number of claims on the account
        return tecXCHAIN_ACCOUNT_CREATE_TOO_MANY;
    }

    auto const claimKeylet =
        keylet::xChainCreateAccountClaimID(bridgeSpec, att.createCount);

    // sleCID may be null. If it's null it isn't created until the end of this
    // function (if needed)
    auto const sleCID = psb.peek(claimKeylet);
    bool createCID = false;
    if (!sleCID)
    {
        createCID = true;

        // Check reserve
        auto const balance = (*sleDoor)[sfBalance];
        auto const reserve =
            psb.fees().accountReserve((*sleDoor)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    if (!signersList.count(calcAccountID(att.publicKey)))
    {
        return tecXCHAIN_PROOF_UNKNOWN_KEY;
    }

    XChainCreateAccountAttestations curAtts = [&] {
        if (sleCID)
            return XChainCreateAccountAttestations{
                sleCID->getFieldArray(sfXChainCreateAccountAttestations)};
        return XChainCreateAccountAttestations{};
    }();

    auto const rewardAccounts =
        curAtts.onNewAttestation(att, quorum, signersList);

    // Account create transactions must happen in order
    if (rewardAccounts && claimCount + 1 == att.createCount)
    {
        auto const r = finalizeClaimHelper(
            psb,
            bridgeSpec,
            att.toCreate,
            att.sendingAmount,
            /*rewardPoolSrc*/ doorAccount,
            att.rewardAmount,
            *rewardAccounts,
            att.wasLockingChainSend,
            sleCID,
            ctx_.journal);
        if (!isTesSuccess(r))
            return r;
        (*sleB)[sfXChainAccountClaimCount] = att.createCount;
        psb.update(sleB);
    }
    else
    {
        if (createCID)
        {
            if (sleCID)
                return tecINTERNAL;

            auto const sleCID = std::make_shared<SLE>(claimKeylet);
            (*sleCID)[sfAccount] = doorAccount;
            (*sleCID)[sfXChainBridge] = bridgeSpec;
            (*sleCID)[sfXChainAccountCreateCount] = att.createCount;
            sleCID->setFieldArray(
                sfXChainCreateAccountAttestations, curAtts.toSTArray());

            // Add to owner directory of the door account
            auto const page = ctx_.view().dirInsert(
                keylet::ownerDir(doorAccount),
                claimKeylet,
                describeOwnerDir(doorAccount));
            if (!page)
                return tecDIR_FULL;
            (*sleCID)[sfOwnerNode] = *page;

            // Reserve was already checked
            adjustOwnerCount(psb, sleDoor, 1, ctx_.journal);
            psb.insert(sleCID);
            psb.update(sleDoor);
        }
        else
        {
            if (!sleCID)
                return tecINTERNAL;
            sleCID->setFieldArray(
                sfXChainCreateAccountAttestations, curAtts.toSTArray());
            psb.update(sleCID);
        }
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

    // TODO: sle's cannot overlap calls to applyCreateAccountAtt and
    // applyCreateAccountAtt because those functions create a sandbox
    // Reduce the scope of sleB
    auto const bridgeK = keylet::bridge(bridgeSpec);
    auto const sleB = ctx_.view().peek(bridgeK);
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }
    auto const thisDoor = (*sleB)[sfAccount];
    auto const doorK = keylet::account(thisDoor);

    // signersList is a map from account id to weights
    auto const [signersList, quorum, slTer] =
        getSignersListAndQuorum(ctx_.view(), *sleB, ctx_.journal);

    if (!isTesSuccess(slTer))
        return slTer;

    for (auto const& createAtt : batch.creates())
    {
        auto const r = applyCreateAccountAtt(
            createAtt,
            thisDoor,
            doorK,
            bridgeSpec,
            bridgeK,
            signersList,
            quorum);

        if (r == tecINTERNAL)
            return r;

        applyRestuls.push_back(r);
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
XChainCreateAccount::preflight(PreflightContext const& ctx)
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

    auto const reward = ctx.tx[sfSignatureReward];
    if (reward.signum() <= 0 || !reward.native())
        return temBAD_AMOUNT;

    if (reward.issue() != amount.issue())
        return temBAD_AMOUNT;

    return preflight2(ctx);
}

TER
XChainCreateAccount::preclaim(PreclaimContext const& ctx)
{
    STXChainBridge const bridgeSpec = ctx.tx[sfXChainBridge];
    STAmount const amount = ctx.tx[sfAmount];
    STAmount const reward = ctx.tx[sfSignatureReward];

    auto const sleB = ctx.view.read(keylet::bridge(bridgeSpec));
    if (!sleB)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    if (reward != (*sleB)[sfSignatureReward])
    {
        return tecXCHAIN_REWARD_MISMATCH;
    }

    std::optional<STAmount> minCreateAmount =
        (*sleB)[~sfMinAccountCreateAmount];

    if (!minCreateAmount)
    {
        return tecXCHAIN_INSUFF_CREATE_AMOUNT;
    }

    if (minCreateAmount->issue() != amount.issue() || amount < minCreateAmount)
        return tecBAD_XCHAIN_TRANSFER_ISSUE;

    AccountID const thisDoor = (*sleB)[sfAccount];
    bool isLockingChain = false;
    {
        if (thisDoor == bridgeSpec.lockingChainDoor())
            isLockingChain = true;
        else if (thisDoor == bridgeSpec.issuingChainDoor())
            isLockingChain = false;
        else
            return tecINTERNAL;
    }

    if (isLockingChain)
    {
        if (bridgeSpec.lockingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(bridgeSpec.issuingChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }
    else
    {
        if (bridgeSpec.issuingChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(bridgeSpec.lockingChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }

    return tesSUCCESS;
}

TER
XChainCreateAccount::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    AccountID const account = ctx_.tx[sfAccount];
    STAmount const amount = ctx_.tx[sfAmount];
    STAmount const reward = ctx_.tx[sfSignatureReward];
    STXChainBridge const bridge = ctx_.tx[sfXChainBridge];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleB = psb.peek(keylet::bridge(bridge));
    if (!sleB)
        return tecINTERNAL;

    auto const dst = (*sleB)[sfAccount];

    STAmount const toTransfer = amount + reward;
    auto const thTer =
        transferHelper(psb, account, dst, toTransfer, ctx_.journal);

    if (!isTesSuccess(thTer))
        return thTer;

    (*sleB)[sfXChainAccountCreateCount] =
        (*sleB)[sfXChainAccountCreateCount] + 1;
    psb.update(sleB);

    psb.apply(ctx_.rawView());

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
XChainClaimAccount::preflight(PreflightContext const& ctx)
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
XChainClaimAccount::preclaim(PreclaimContext const& ctx)
{
    auto const amount = ctx.tx[sfAmount];
    STXChainBridge const& sidechain = ctx.tx[sfXChainBridge];

    auto const sleB = ctx.view.read(keylet::bridge(sidechain));
    if (!sleB)
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
        auto const thisDoor = (*sleB)[sfAccount];

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
XChainClaimAccount::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const otherChainAmount = ctx_.tx[sfAmount];
    auto const dst = ctx_.tx[sfDestination];
    STXChainBridge const& sidechain = ctx_.tx[sfXChainBridge];

    auto const sleAcc = psb.peek(keylet::account(account));
    auto const sleB = psb.read(keylet::bridge(sidechain));

    if (!(sleB && sleAcc))
        return tecINTERNAL;

    auto const thisDoor = (*sleB)[sfAccount];

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
