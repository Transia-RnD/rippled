//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Nerd Nest XYZ.

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

#include <xrpld/app/tx/detail/MessageCreate.h>
#include <xrpld/ledger/View.h>

namespace ripple {

TxConsequences
MessageCreate::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, beast::zero};
}

NotTEC
MessageCreate::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureMessage))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    // Check that the transaction is well-formed.
    if (ctx.tx.getFieldVL(sfMessageData).size() > maxMessageLength)
    {
        return temMALFORMED;
    }

    // Account != Destination
    if (ctx.tx.getAccountID(sfDestination) == ctx.tx.getAccountID(sfAccount))
    {
        return temDST_IS_SRC;
    }

    // Validate Destination
    AccountID const dest = ctx.tx.getAccountID(sfDestination);
    if (!dest || dest == noAccount())
    {
        JLOG(ctx.j.warn()) << "Malformed transaction: Invalid destination.";
        return temDST_NEEDED;
    }

    return preflight2(ctx);
}

TER
MessageCreate::preclaim(PreclaimContext const& ctx)
{
    return tesSUCCESS;
}

TER
MessageCreate::doApply()
{
    auto const sleAccount = ctx_.view().peek(keylet::account(account_));
    if (!sleAccount)
        return tefINTERNAL;

    AccountID const destID = ctx_.tx.getAccountID(sfDestination);
    auto const k = keylet::account(destID);
    auto sleDest = ctx_.view().peek(k);
    if (!sleDest)
    {
        // Create the destination account if it doesn't exist.
        std::uint32_t const seqno{
            view().rules().enabled(featureDeletableAccounts) ? view().seq()
                                                             : 1};
        sleDest = std::make_shared<SLE>(k);
        sleDest->setAccountID(sfAccount, destID);
        sleDest->setFieldU32(sfSequence, seqno);
        sleDest->setFieldAmount(sfBalance, STAmount(0));
        view().insert(sleDest);
    }

    uint32_t const sequence = ctx_.tx.getSeqProxy().value();
    Keylet const nKeylet = keylet::notification(account_, sequence);
    auto const sle = std::make_shared<SLE>(nKeylet);

    // Check reserve availability for new object creation
    {
        auto const reserve = ctx_.view().fees().accountReserve(
            sleAccount->getFieldU32(sfOwnerCount) + 1);
        auto const& balance = sleAccount->getFieldAmount(sfBalance);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    sle->setAccountID(sfOwner, account_);
    sle->setAccountID(sfDestination, destID);
    sle->setFieldU32(sfSequence, sequence);
    sle->setFieldVL(sfMessageData, ctx_.tx.getFieldVL(sfMessageData));
    if (ctx_.tx.isFieldPresent(sfMessageType))
        sle->setFieldVL(sfMessageType, ctx_.tx.getFieldVL(sfMessageType));
    if (ctx_.tx.isFieldPresent(sfMessageFormat))
        sle->setFieldVL(sfMessageFormat, ctx_.tx.getFieldVL(sfMessageFormat));

    sle->setFieldU32(sfLastUpdateTime, ctx_.view().parentCloseTime().time_since_epoch().count());

    // Add ledger object to owner's page
    {
        auto page = ctx_.view().dirInsert(
            keylet::ownerDir(account_), sle->key(), describeOwnerDir(account_));
        if (!page)
            return tecDIR_FULL;
        (*sle)[sfOwnerNode] = *page;
    }
    adjustOwnerCount(ctx_.view(), sleAccount, 1, ctx_.journal);

    // Add ledger object to dest's page
    {
        auto page = ctx_.view().dirInsert(
            keylet::ownerDir(destID), sle->key(), describeOwnerDir(destID));
        if (!page)
            return tecDIR_FULL;
        (*sle)[sfDestinationNode] = *page;
    }
    adjustOwnerCount(ctx_.view(), sleDest, 1, ctx_.journal);

    ctx_.view().insert(sle);
    return tesSUCCESS;
}

}  // namespace ripple
