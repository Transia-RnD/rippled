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
#include <xrpld/app/tx/detail/RecurringPaymentClaim.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>

namespace ripple {

NotTEC
RecurringPaymentClaim::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    std::uint32_t const txFlags = ctx.tx.getFlags();
    if (txFlags & tfUniversalMask)
    {
        JLOG(ctx.j.error()) << "RecurringPaymentSet: Invalid flags set";
        return temINVALID_FLAG;
    }

    return preflight2(ctx);
}

TER
RecurringPaymentClaim::checkPermission(ReadView const& view, STTx const& tx)
{
    return tesSUCCESS;
}

TER
RecurringPaymentClaim::preclaim(PreclaimContext const& ctx)
{
    // Get the recurring payment sle
    auto const sle = ctx.view.read(keylet::recurringPayment(ctx.tx.getFieldH256(sfRecurringPaymentID)));
    if (!sle)
    {
        JLOG(ctx.j.error()) << "RecurringPaymentClaim: Recurring payment not found";
        return tecNO_TARGET;
    }
    
    // - `sfDestination`: The intended recipient.
    if (ctx.tx.getAccountID(sfDestination) != sle->getAccountID(sfDestination))
    {
        JLOG(ctx.j.error()) << "RecurringPaymentClaim: Destination does not match recurring payment";
        return tecNO_PERMISSION;
    }

    // - `Amount` is < 0.
    if (ctx.tx.getFieldAmount(sfAmount) <= XRPAmount{0})
    {
        JLOG(ctx.j.error()) << "RecurringPaymentClaim: Amount must be positive";
        return temBAD_AMOUNT;
    }

    // - `Amount` (type) does not equal `RecurringPayment` `Amount` (type).
    if (ctx.tx.getFieldAmount(sfAmount).getCurrency() != sle->getFieldAmount(sfAmount).getCurrency())
    {
        JLOG(ctx.j.error()) << "RecurringPaymentClaim: Amount currency does not match recurring payment";
        return temBAD_AMOUNT;
    }

    // - `ClaimedThisPeriod + Amount` exceeds the authorized amount for the period.
    // if (sle->getFieldAmount(sfClaimedThisPeriod) + ctx.tx.getFieldAmount(sfAmount) > sle->getFieldAmount(sfAmount))
    // {
    //     JLOG(ctx.j.error()) << "RecurringPaymentClaim: Claimed amount exceeds authorized amount for the period";
    //     return tecINSUFFICIENT_FUNDS;
    // }

    // - `sfLockedAmount` < `Amount`.
    // if (sle->getFieldAmount(sfLockedAmount) < ctx.tx.getFieldAmount(sfAmount))
    // {
    //     JLOG(ctx.j.error()) << "RecurringPaymentClaim: Locked amount is less than claim amount";
    //     return tecINSUFFICIENT_FUNDS;
    // }

    // - Current time is after `Expiration`.
    if (sle->isFieldPresent(sfExpiration) && ctx.view.parentCloseTime().time_since_epoch().count() > sle->getFieldU32(sfExpiration))
    {
        JLOG(ctx.j.error()) << "RecurringPaymentClaim: Recurring payment has expired";
        return tecEXPIRED;
    }
    // // - Current time is before `NextResetTime` (if first claim in period).
    // if (ctx.view.parentCloseTime().time_since_epoch().count() < sle->getFieldU32(sfNextResetTime))
    // {
    //     JLOG(ctx.j.error()) << "RecurringPaymentClaim: Next reset time has not been reached";
    //     return tecTOO_SOON;
    // }

    return tesSUCCESS;
}

TER
RecurringPaymentClaim::doApply()
{
    auto const account = ctx_.tx.getAccountID(sfAccount);
    auto const slea = ctx_.view().peek(keylet::account(account));
    if (!slea)
    {
        JLOG(ctx_.journal.error()) << "RecurringPaymentClaim: Source account not found";
        return tefINTERNAL;
    }

    auto const destination = ctx_.tx.getAccountID(sfDestination);
    auto const sled = ctx_.view().peek(keylet::account(destination));
    if (!sled)
    {
        JLOG(ctx_.journal.error()) << "RecurringPaymentClaim: Destination account not found";
        return tefINTERNAL;
    }

    
    // - If current time >= `NextResetTime`, reset `ClaimedThisPeriod` to 0 and set `NextResetTime` to `NextResetTime + Frequency` (repeat until in future).
    // - Add `Amount` to `ClaimedThisPeriod`.
    // - Deduct the specified `Amount` from `sfLockedFunds` in the `RecurringPayment` ledger entry.
    // - Credit the specified `Amount` to the destination account.
    // - Remove the `RecurringPayment` ledger object if time > `Expiration` (and return any remaining locked funds to the owner).

    auto const sle = ctx_.view().peek(keylet::recurringPayment(ctx_.tx.getFieldH256(sfRecurringPaymentID)));
    if (!sle)
    {
        JLOG(ctx_.journal.error()) << "RecurringPaymentClaim: Recurring payment not found";
        return tecNO_TARGET;
    }

    // Check if the current time is after the next reset time
    auto const currentTime = ctx_.view().parentCloseTime().time_since_epoch().count();
    if (currentTime >= sle->getFieldU32(sfNextResetTime))
    {
        // Reset the claimed amount for the period
        sle->setFieldAmount(sfClaimedThisPeriod, XRPAmount(0));
        // Update the next reset time
        auto const frequency = sle->getFieldU64(sfFrequency);
        sle->setFieldU32(sfNextResetTime, currentTime + frequency);
    }

    if (sle->getFieldAmount(sfClaimedThisPeriod) + ctx_.tx.getFieldAmount(sfAmount) > sle->getFieldAmount(sfAmount))
    {
        JLOG(ctx_.journal.error()) << "RecurringPaymentClaim: Claimed amount exceeds authorized amount for the period";
        return tecINSUFFICIENT_FUNDS;
    }

    // Add the claimed amount to the current period's claimed amount
    STAmount const claimAmount = ctx_.tx.getFieldAmount(sfAmount);
    sle->setFieldAmount(sfClaimedThisPeriod, sle->getFieldAmount(sfClaimedThisPeriod) + claimAmount);

    // Deduct the claimed amount from the locked funds
    // STAmount const lockedAmount = sle->getFieldAmount(sfLockedAmount);
    // if (lockedAmount < claimAmount)
    // {
    //     JLOG(ctx_.journal.error()) << "RecurringPaymentClaim: Insufficient locked funds";
    //     return tecINSUFFICIENT_FUNDS;
    // }
    // sle->setFieldAmount(sfLockedAmount, lockedAmount - claimAmount);

    // Credit the destination account
    (*sled)[sfBalance] = (*sled)[sfBalance] + claimAmount;
    // Debit the source account
    (*slea)[sfBalance] = (*slea)[sfBalance] - claimAmount;
    // update the view
    ctx_.view().update(sle);
    ctx_.view().update(sled);
    ctx_.view().update(slea);
    return tesSUCCESS;
}

}  // namespace ripple
