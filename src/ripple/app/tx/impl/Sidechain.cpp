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
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STSidechain.h>
#include <ripple/protocol/STXChainClaimProof.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/st.h>
#include "ripple/protocol/STAmount.h"
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

//------------------------------------------------------------------------------

NotTEC
SidechainCreate::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureSidechains))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    auto const account = ctx.tx[sfAccount];
    auto const quorum = ctx.tx[sfSignerQuorum];
    auto const sidechain = ctx.tx[sfSidechain];
    if (sidechain.srcChainDoor() == sidechain.dstChainDoor())
    {
        return temEQUAL_DOOR_ACCOUNTS;
    }

    if (sidechain.srcChainDoor() != account &&
        sidechain.dstChainDoor() != account)
    {
        return temSIDECHAIN_NONDOOR_OWNER;
    }

    if (isXRP(sidechain.srcChainIssue()) != isXRP(sidechain.dstChainIssue()))
    {
        // Because ious and xrp have different numeric ranges, both the src and
        // dst issues must be both XRP or both IOU.
        return temSIDECHAIN_BAD_ISSUES;
    }

    auto signersR = SignerEntries::deserialize(ctx.tx, ctx.j, "transaction");
    if (!signersR.has_value())
        return signersR.error();
    auto const& signers = signersR.value();

    // Make sure no signers reference this account.  Also make sure the
    // quorum can be reached.
    std::uint64_t allSignersWeight(0);
    for (auto const& signer : signers)
    {
        std::uint32_t const weight = signer.weight;
        if (weight <= 0)
            return temBAD_WEIGHT;

        allSignersWeight += signer.weight;

        if (signer.account == account)
            return temBAD_SIGNER;
    }

    if (quorum <= 0 || allSignersWeight < quorum)
        return temBAD_QUORUM;

    return preflight2(ctx);
}

TER
SidechainCreate::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const sidechain = ctx.tx[sfSidechain];

    if (ctx.view.read(keylet::sidechain(sidechain)))
    {
        return tecDUPLICATE;
    }

    if (!isXRP(sidechain.srcChainIssue()) &&
        !ctx.view.read(keylet::account(sidechain.srcChainIssue().account)))
    {
        return tecNO_ISSUER;
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
SidechainCreate::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const sidechain = ctx_.tx[sfSidechain];
    auto const quorum = ctx_.tx[sfSignerQuorum];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    Keylet const sidechainKeylet = keylet::sidechain(sidechain);
    auto const sleSC = std::make_shared<SLE>(sidechainKeylet);

    (*sleSC)[sfAccount] = account;
    (*sleSC)[sfSidechain] = sidechain;
    (*sleSC)[sfXChainSequence] = 0;

    (*sleSC)[sfSignerQuorum] = quorum;
    {
        // set the signer entries
        auto signersR =
            SignerEntries::deserialize(ctx_.tx, ctx_.journal, "transaction");
        if (!signersR.has_value())
            return signersR.error();  // This is already checked in preflight

        auto& signers = signersR.value();

        std::sort(signers.begin(), signers.end());
        // This constructor reserves, doesn't change size.
        STArray toLedger(signers.size());
        for (auto const& entry : signers)
        {
            toLedger.emplace_back(sfSignerEntry);
            STObject& obj = toLedger.back();
            obj.reserve(2);
            obj.setAccountID(sfAccount, entry.account);
            obj.setFieldU16(sfSignerWeight, entry.weight);
        }

        sleSC->setFieldArray(sfSignerEntries, toLedger);
    }

    // Add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account),
            sidechainKeylet,
            describeOwnerDir(account));
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
SidechainClaim::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureSidechains))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    STXChainClaimProof proof = ctx.tx[sfXChainClaimProof];

    // Verify the signatures
    if (!proof.verify())
        return temBAD_XCHAIN_PROOF;

    if (proof.amount().signum() <= 0)
        return temBAD_AMOUNT;

    {
        STSidechain const& sc = proof.sidechain();
        Issue const& expectedIssue = proof.expectSrcChainClaim()
            ? sc.srcChainIssue()
            : sc.dstChainIssue();
        if (proof.amount().issue() != expectedIssue)
            return temBAD_AMOUNT;
    }

    return preflight2(ctx);
}

TER
SidechainClaim::preclaim(PreclaimContext const& ctx)
{
    AccountID const account = ctx.tx[sfAccount];
    STXChainClaimProof proof = ctx.tx[sfXChainClaimProof];
    STSidechain const& sidechain = proof.sidechain();
    STAmount const& amount = proof.amount();
    auto const xChainSeq = proof.xChainSeqNum();

    auto const sleSC = ctx.view.read(keylet::sidechain(sidechain));
    if (!sleSC)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    if (!ctx.view.read(keylet::account(ctx.tx[sfDestination])))
    {
        return tecNO_DST;
    }

    {
        // Check that the amount specified in the proof matches the expected
        // issue
        auto const thisDoor = (*sleSC)[sfAccount];

        bool isSrcChain = false;
        {
            if (thisDoor == sidechain.srcChainDoor())
                isSrcChain = true;
            else if (thisDoor == sidechain.dstChainDoor())
                isSrcChain = false;
            else
                return tecINTERNAL;
        }

        if (isSrcChain)
        {
            if (sidechain.dstChainIssue() != amount.issue())
                return tecBAD_XCHAIN_TRANSFER_ISSUE;
        }
        else
        {
            if (sidechain.srcChainIssue() != amount.issue())
                return tecBAD_XCHAIN_TRANSFER_ISSUE;
        }

        if (isSrcChain != proof.expectSrcChainClaim())
        {
            // Tried to send the proof to the wrong chain
            return tecXCHAIN_CLAIM_WRONG_CHAIN;
        }
    }

    {
        // Check that the sequence number is owned by the sender of this
        // transaction
        auto const sleSQ =
            ctx.view.read(keylet::xChainSeqNum(sidechain, xChainSeq));
        if (!sleSQ)
        {
            // Sequence number doesn't exist
            return tecBAD_XCHAIN_TRANSFER_SEQ_NUM;
        }

        if ((*sleSQ)[sfAccount] != account)
        {
            // Sequence number isn't owned by the sender of this transaction
            return tecBAD_XCHAIN_TRANSFER_SEQ_NUM;
        }
    }

    {
        std::unordered_map<AccountID, std::uint16_t> const m = [&] {
            std::unordered_map<AccountID, std::uint16_t> r;
            STArray const signerEntries = sleSC->getFieldArray(sfSignerEntries);
            for (auto const& e : signerEntries)
            {
                r[e[sfAccount]] = e[sfSignerWeight];
            }
            return r;
        }();

        std::uint32_t totalWeight = 0;
        for (auto const& [pk, _] : proof.signatures())
        {
            auto const id = calcAccountID(pk);
            auto const it = m.find(id);
            if (it == m.end())
                return tecXCHAIN_PROOF_UNKNOWN_KEY;
            totalWeight += it->second;
        }

        if (totalWeight < (*sleSC)[sfSignerQuorum])
            return tecXCHAIN_PROOF_NO_QUORUM;
    }

    return tesSUCCESS;
}

TER
SidechainClaim::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const dst = ctx_.tx[sfDestination];
    STXChainClaimProof proof = ctx_.tx[sfXChainClaimProof];
    STSidechain const& sidechain = proof.sidechain();
    STAmount const& otherChainAmount = proof.amount();
    auto const xChainSeq = proof.xChainSeqNum();

    auto const sleAcc = psb.peek(keylet::account(account));
    auto const sleSC = psb.read(keylet::sidechain(sidechain));
    auto const seqK = keylet::xChainSeqNum(sidechain, xChainSeq);
    auto const sleSQ = psb.read(seqK);

    if (!(sleSC && sleSQ && sleAcc))
        return tecINTERNAL;

    auto const thisDoor = (*sleSC)[sfAccount];

    Issue const thisChainIssue = [&] {
        bool const isSrcChain = (thisDoor == sidechain.srcChainDoor());
        return isSrcChain ? sidechain.srcChainIssue()
                          : sidechain.dstChainIssue();
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

    // TODO: handle DepositAuth
    //       handle dipping below reserve
    // TODO: Create a payment transaction instead of calling flow directly?
    if (thisChainAmount.native())
    {
        // TODO: Check reserve
        auto const sleDoor = psb.peek(keylet::account(thisDoor));
        assert(sleDoor);
        if (!sleDoor)
            return tecINTERNAL;

        if ((*sleDoor)[sfBalance] < thisChainAmount)
        {
            return tecINSUFFICIENT_FUNDS;
        }
        auto const sleDst = psb.peek(keylet::account(dst));
        if (!sleDst)
        {
            // TODO
            return tecNO_DST;
        }
        (*sleDoor)[sfBalance] = (*sleDoor)[sfBalance] - thisChainAmount;
        (*sleDst)[sfBalance] = (*sleDst)[sfBalance] + thisChainAmount;
        psb.update(sleDoor);
        psb.update(sleDst);
        psb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

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

    {
        // Remove the sequence number
        // It's important that the sequence number is only removed if the
        // payment succeeds
        auto const page = (*sleSQ)[sfOwnerNode];
        if (!psb.dirRemove(keylet::ownerDir(account), page, seqK.key, true))
        {
            JLOG(j_.fatal())
                << "Unable to delete xchain seq number from owner.";
            return tefBAD_LEDGER;
        }
    }

    adjustOwnerCount(psb, sleAcc, -1, j_);

    psb.apply(ctx_.rawView());
    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
SidechainXChainTransfer::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureSidechains))
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
SidechainXChainTransfer::preclaim(PreclaimContext const& ctx)
{
    auto const sidechain = ctx.tx[sfSidechain];
    auto const amount = ctx.tx[sfAmount];

    auto const sleSC = ctx.view.read(keylet::sidechain(sidechain));
    if (!sleSC)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    auto const thisDoor = (*sleSC)[sfAccount];

    bool isSrcChain = false;
    {
        if (thisDoor == sidechain.srcChainDoor())
            isSrcChain = true;
        else if (thisDoor == sidechain.dstChainDoor())
            isSrcChain = false;
        else
            return tecINTERNAL;
    }

    if (isSrcChain)
    {
        if (sidechain.srcChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;
    }
    else
    {
        if (sidechain.dstChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;
    }

    return tesSUCCESS;
}

TER
SidechainXChainTransfer::doApply()
{
    PaymentSandbox psb(&ctx_.view());

    auto const account = ctx_.tx[sfAccount];
    auto const amount = ctx_.tx[sfAmount];
    auto const sidechain = ctx_.tx[sfSidechain];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleSC = psb.read(keylet::sidechain(sidechain));
    if (!sleSC)
        return tecINTERNAL;

    auto const dst = (*sleSC)[sfAccount];

    // TODO: handle DepositAuth
    //       handle dipping below reserve
    // TODO: Create a payment transaction instead of calling flow directly?
    if (amount.native())
    {
        // TODO: Check reserve
        if ((*sle)[sfBalance] < amount)
        {
            return tecINSUFFICIENT_FUNDS;
        }
        auto const sleDst = psb.peek(keylet::account(dst));
        if (!sleDst)
        {
            // TODO
            return tecNO_DST;
        }
        (*sle)[sfBalance] = (*sle)[sfBalance] - amount;
        (*sleDst)[sfBalance] = (*sleDst)[sfBalance] + amount;
        psb.update(sle);
        psb.update(sleDst);
        psb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

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
SidechainXChainSeqNumCreate::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureSidechains))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    return preflight2(ctx);
}

TER
SidechainXChainSeqNumCreate::preclaim(PreclaimContext const& ctx)
{
    auto const account = ctx.tx[sfAccount];
    auto const sidechain = ctx.tx[sfSidechain];

    if (!ctx.view.read(keylet::sidechain(sidechain)))
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
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
SidechainXChainSeqNumCreate::doApply()
{
    auto const account = ctx_.tx[sfAccount];
    auto const sidechain = ctx_.tx[sfSidechain];

    auto const sleAcc = ctx_.view().peek(keylet::account(account));
    if (!sleAcc)
        return tecINTERNAL;

    auto const sleSC = ctx_.view().peek(keylet::sidechain(sidechain));
    if (!sleSC)
        return tecINTERNAL;

    std::uint32_t const xChainSeq = (*sleSC)[sfXChainSequence] + 1;
    if (xChainSeq == 0)
        return tecINTERNAL;  // overflow

    (*sleSC)[sfXChainSequence] = xChainSeq;

    Keylet const seqKeylet = keylet::xChainSeqNum(sidechain, xChainSeq);
    if (ctx_.view().read(seqKeylet))
        return tecINTERNAL;  // already checked out!?!

    auto const sleQ = std::make_shared<SLE>(seqKeylet);

    (*sleQ)[sfAccount] = account;
    (*sleQ)[sfSidechain] = sidechain;
    (*sleQ)[sfXChainSequence] = xChainSeq;

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
    ctx_.view().update(sleSC);
    ctx_.view().update(sleAcc);

    return tesSUCCESS;
}

//------------------------------------------------------------------------------

NotTEC
SidechainXChainCreateAccount::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureSidechains))
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
    auto const sidechain = ctx.tx[sfSidechain];
    auto const amount = ctx.tx[sfAmount];

    auto const sleSC = ctx.view.read(keylet::sidechain(sidechain));
    if (!sleSC)
    {
        // TODO: custom return code for no sidechain?
        return tecNO_ENTRY;
    }

    auto const thisDoor = (*sleSC)[sfAccount];

    bool isSrcChain = false;
    {
        if (thisDoor == sidechain.srcChainDoor())
            isSrcChain = true;
        else if (thisDoor == sidechain.dstChainDoor())
            isSrcChain = false;
        else
            return tecINTERNAL;
    }

    if (isSrcChain)
    {
        if (sidechain.srcChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(sidechain.dstChainIssue()))
            return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
    }
    else
    {
        if (sidechain.dstChainIssue() != ctx.tx[sfAmount].issue())
            return tecBAD_XCHAIN_TRANSFER_ISSUE;

        if (!isXRP(sidechain.srcChainIssue()))
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
    auto const sidechain = ctx_.tx[sfSidechain];

    auto const sle = psb.peek(keylet::account(account));
    if (!sle)
        return tecINTERNAL;

    auto const sleSC = psb.read(keylet::sidechain(sidechain));
    if (!sleSC)
        return tecINTERNAL;

    auto const dst = (*sleSC)[sfAccount];

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
    if (!ctx.rules.enabled(featureSidechains))
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
    STSidechain const& sidechain = ctx.tx[sfSidechain];

    auto const sleSC = ctx.view.read(keylet::sidechain(sidechain));
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
            if (thisDoor == sidechain.srcChainDoor())
                isSrcChain = true;
            else if (thisDoor == sidechain.dstChainDoor())
                isSrcChain = false;
            else
                return tecINTERNAL;
        }

        if (isSrcChain)
        {
            if (!isXRP(sidechain.dstChainIssue()))
                return tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE;
        }
        else
        {
            if (!isXRP(sidechain.srcChainIssue()))
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
    STSidechain const& sidechain = ctx_.tx[sfSidechain];

    auto const sleAcc = psb.peek(keylet::account(account));
    auto const sleSC = psb.read(keylet::sidechain(sidechain));

    if (!(sleSC && sleAcc))
        return tecINTERNAL;

    auto const thisDoor = (*sleSC)[sfAccount];

    Issue const thisChainIssue = [&] {
        bool const isSrcChain = (thisDoor == sidechain.srcChainDoor());
        return isSrcChain ? sidechain.srcChainIssue()
                          : sidechain.dstChainIssue();
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
