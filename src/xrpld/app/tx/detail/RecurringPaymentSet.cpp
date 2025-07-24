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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/tx/detail/RecurringPaymentSet.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>

namespace ripple {

NotTEC
RecurringPaymentSet::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    std::uint32_t const txFlags = ctx.tx.getFlags();
    if (txFlags & tfUniversalMask)
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentSet: Invalid flags set";
        return temINVALID_FLAG;
    }

    // - If `sfDestination` is omitted, `sfPublicKey` **must** be present and must match the account's master or regular key.
    if (!ctx.tx.isFieldPresent(sfDestination))
    {
        if (!ctx.tx.isFieldPresent(sfPublicKey))
        {
            JLOG(ctx.j.trace()) << "RecurringPaymentSet: PublicKey is required when Destination is not present";
            return temMALFORMED;
        }
    }
    else
    {
        if (ctx.tx.isFieldPresent(sfPublicKey))
        {
            JLOG(ctx.j.trace()) << "RecurringPaymentSet: PublicKey must not be present when Destination is set";
            return temMALFORMED;
        }
    }

    // - `Destination` is the same as `Account`.
    if (!ctx.tx.isFieldPresent(sfDestination) && ctx.tx.getAccountID(sfAccount) == ctx.tx.getAccountID(sfDestination))
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentSet: Destination is the same as Account";
        return temMALFORMED;
    }
    // - `Amount` is invalid OR <= 0.
    if (!ctx.tx.isFieldPresent(sfAmount) || ctx.tx.getFieldAmount(sfAmount) <= XRPAmount(0))
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentSet: Amount is invalid or <= 0";
        return temMALFORMED;
    }

    // - `Frequency` <= SYSTEM_MINIMUM.
    auto const SYSTEM_MINIMUM = 1; // 30 days
    if (!ctx.tx.isFieldPresent(sfFrequency) || ctx.tx.getFieldU64(sfFrequency) < SYSTEM_MINIMUM)
        return temMALFORMED;

    // - `Expiration` is less than the `StartTime`.
    if (ctx.tx.isFieldPresent(sfStartTime) && ctx.tx.isFieldPresent(sfExpiration) && ctx.tx.getFieldU32(sfExpiration) < ctx.tx.getFieldU32(sfStartTime))
        return temMALFORMED;

    return preflight2(ctx);
}

TER
RecurringPaymentSet::checkPermission(ReadView const& view, STTx const& tx)
{
    return tesSUCCESS;
}

TER
RecurringPaymentSet::preclaim(PreclaimContext const& ctx)
{
    AccountID const account = ctx.tx.getAccountID(sfAccount);
    if (!ctx.view.exists(keylet::account(account)))
        return terNO_ACCOUNT;

    AccountID const dest = ctx.tx.isFieldPresent(sfDestination) ? ctx.tx.getAccountID(sfDestination) : noAccount();
    if (dest != noAccount() && !ctx.view.exists(keylet::account(dest)))
        return tecNO_DST;
    
    uint32_t const seq = ctx.tx.getSeqValue();

    if (!ctx.tx.isFieldPresent(sfRecurringPaymentID))
    {
        // create
        // - `RecurringPayment` ledger entry exists (on create).
        if (ctx.view.exists(keylet::recurringPayment(account, dest, seq)))
            return tecDUPLICATE;

        // - `Account` does not have a valid Trustline (if IOU).
        // IGNORE: Not implemented yet.
    
        // - `StartTime` is less than the current time.
        if (ctx.tx.isFieldPresent(sfStartTime) && ctx.tx.getFieldU32(sfStartTime) < ctx.view.parentCloseTime().time_since_epoch().count())
            return tecEXPIRED;
        // - `Expiration` is less than the current time.
        if (ctx.tx.isFieldPresent(sfExpiration) && ctx.tx.getFieldU32(sfExpiration) < ctx.view.parentCloseTime().time_since_epoch().count())
            return tecEXPIRED;
    }
    else
    {
        // update
        // - `RecurringPayment` ledger entry does not exist (on update).
        if (!ctx.view.exists(keylet::recurringPayment(ctx.tx.getFieldH256(sfRecurringPaymentID))))
            return tecNO_ENTRY;

        // - `RecurringPaymentID` submitted and `Amount` not present or optional `Expiration` not present.
        // IGNORE: Not implemented yet.
        
        // - `Expiration` is less than the `NextResetTime`.
        // IGNORE: Not implemented yet.
    }
    return tesSUCCESS;
}

TER
RecurringPaymentSet::doApply()
{
    if (!ctx_.tx.isFieldPresent(sfRecurringPaymentID))
    {
        // create

        AccountID const account = ctx_.tx.getAccountID(sfAccount);
        auto const slea = ctx_.view().peek(keylet::account(account));
        if (!slea)
            return tefINTERNAL;

        // check reserves
        STAmount const reserve{ctx_.view().fees().accountReserve(slea->getFieldU32(sfOwnerCount) + 1)};
        if (mPriorBalance < reserve)
            return tecINSUFFICIENT_RESERVE;

        AccountID const dest = ctx_.tx.isFieldPresent(sfDestination) ? ctx_.tx.getAccountID(sfDestination) : noAccount();
        if (dest != noAccount() && !ctx_.view().exists(keylet::account(dest)))
            return tefINTERNAL;

        uint32_t const seq = ctx_.tx.getSeqValue();
        STAmount const limitAmount = ctx_.tx.getFieldAmount(sfAmount);
        
        auto const keylet = keylet::recurringPayment(account, dest, seq);
        auto const sle = std::make_shared<SLE>(keylet);
        // required fields
        sle->setAccountID(sfAccount, ctx_.tx.getAccountID(sfAccount));
        sle->setFieldAmount(sfLimitAmount, limitAmount);
        sle->setFieldU64(sfFrequency, ctx_.tx.getFieldU64(sfFrequency));
        sle->setFieldAmount(sfClaimedThisPeriod, XRPAmount(0));
        // optional fields
        if (ctx_.tx.isFieldPresent(sfDestination))
            sle->setAccountID(sfDestination, ctx_.tx.getAccountID(sfDestination));
        if (ctx_.tx.isFieldPresent(sfPublicKey))
            sle->setFieldVL(sfPublicKey, ctx_.tx.getFieldVL(sfPublicKey));
        // if (ctx_.tx.isFieldPresent(sfStartTime))
        //     sle->setFieldU32(sfStartTime, ctx_.tx.getFieldU64(sfStartTime));
        if (ctx_.tx.isFieldPresent(sfExpiration))
            sle->setFieldU32(sfExpiration, ctx_.tx.getFieldU64(sfExpiration));
        if (ctx_.tx.isFieldPresent(sfStartTime))
            sle->setFieldU32(sfNextResetTime, ctx_.tx.getFieldU32(sfStartTime));
        else
            sle->setFieldU32(sfNextResetTime, ctx_.view().parentCloseTime().time_since_epoch().count() + ctx_.tx.getFieldU64(sfFrequency));

        // if no destination, set locked funds
        if (!ctx_.tx.isFieldPresent(sfDestination))
        {
            (*slea)[sfBalance] = (*slea)[sfBalance] - limitAmount;
            sle->setFieldAmount(sfAmount, limitAmount);
        }

        ctx_.view().insert(sle);

        // add to owner directory
        {
            auto const page = ctx_.view().dirInsert(
                keylet::ownerDir(account),
                keylet,
                describeOwnerDir(account));
            if (!page)
                return tecDIR_FULL;
            (*sle)[sfOwnerNode] = *page;
        }

        // optional: add to destination directory
        if (dest != noAccount())
        {
            auto const page = ctx_.view().dirInsert(
                keylet::ownerDir(dest), keylet, describeOwnerDir(dest));
            if (!page)
                return tecDIR_FULL;
            (*sle)[sfDestinationNode] = *page;
        }

        // update owner count
        adjustOwnerCount(ctx_.view(), slea, 1, ctx_.journal);

    }
    else
    {
        // update
        JLOG(ctx_.journal.trace()) << "RecurringPaymentSet: Updating existing recurring payment";
    }
    return tesSUCCESS;
}

}  // namespace ripple
