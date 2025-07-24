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
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/PayChan.h>
#include <xrpl/protocol/digest.h>

namespace ripple {

NotTEC
RecurringPaymentClaim::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    std::uint32_t const txFlags = ctx.tx.getFlags();
    if (txFlags & tfUniversalMask)
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentSet: Invalid flags set";
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
    AccountID const creditAccount = ctx.tx.getAccountID(sfAccount);
    STAmount const limitAmount = ctx.tx.getFieldAmount(sfAmount);
    // Get the recurring payment sle
    Keylet const k = keylet::recurringPayment(ctx.tx.getFieldH256(sfRecurringPaymentID));
    auto const sle = ctx.view.read(k);
    if (!sle)
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Recurring payment not found";
        return tecNO_TARGET;
    }
    
    // - `sfDestination`: The intended recipient.
    if (!sle->isFieldPresent(sfDestination))
    {
        // verify locked funds exist
        if (!sle->isFieldPresent(sfAmount) || sle->getFieldAmount(sfAmount) < limitAmount)
        {
            JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Insufficient locked funds";
            return tecINSUFFICIENT_FUNDS;
        }

        // verify signature
        if (!ctx.tx.isFieldPresent(sfSignature))
        {
            JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Signature is required when Destination is not set";
            return temMALFORMED;
        }
        auto const pk = (*sle)[~sfPublicKey];
        if (!publicKeyType(*pk))
            return temMALFORMED;
        
        auto const sig = ctx.tx[~sfSignature];
        PublicKey const publicKey(*pk);
        Serializer msg;

        serializeRecurringAuthorization(msg, k.key, creditAccount, limitAmount.xrp());
        if (!verify(publicKey, msg.slice(), *sig, /*canonical*/ true))
            return temBAD_SIGNATURE;
    }
    else
    {
        if (creditAccount != sle->getAccountID(sfDestination))
        {
            JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Invalid claiming account";
            return tecNO_PERMISSION;
        }
    }

    // - `Amount` is < 0.
    if (ctx.tx.getFieldAmount(sfAmount) <= XRPAmount{0})
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Amount must be positive";
        return temBAD_AMOUNT;
    }

    // - `Amount` (type) does not equal `RecurringPayment` `Amount` (type).
    if (ctx.tx.getFieldAmount(sfAmount).getCurrency() != sle->getFieldAmount(sfLimitAmount).getCurrency())
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Amount currency does not match recurring payment";
        return temBAD_AMOUNT;
    }

    // - `sfLockedAmount` < `Amount`.
    // if (sle->getFieldAmount(sfAmount) < ctx.tx.getFieldAmount(sfLimitAmount))
    // {
    //     JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Locked amount is less than claim amount";
    //     return tecINSUFFICIENT_FUNDS;
    // }

    // - Current time is after `Expiration`.
    if (sle->isFieldPresent(sfExpiration) && ctx.view.parentCloseTime().time_since_epoch().count() > sle->getFieldU32(sfExpiration))
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Recurring payment has expired";
        return tecEXPIRED;
    }
    // // - Current time is before `NextResetTime` (if first claim in period).
    // if (ctx.view.parentCloseTime().time_since_epoch().count() < sle->getFieldU32(sfNextResetTime))
    // {
    //     JLOG(ctx.j.trace()) << "RecurringPaymentClaim: Next reset time has not been reached";
    //     return tecTOO_SOON;
    // }

    return tesSUCCESS;
}

TER
RecurringPaymentClaim::doApply()
{
    auto const creditAccount = ctx_.tx.getAccountID(sfAccount);
    auto const slec = ctx_.view().peek(keylet::account(creditAccount));
    if (!slec)
    {
        JLOG(ctx_.journal.trace()) << "RecurringPaymentClaim: Credit account not found";
        return tefINTERNAL;
    }

    auto const sle = ctx_.view().peek(keylet::recurringPayment(ctx_.tx.getFieldH256(sfRecurringPaymentID)));
    if (!sle)
    {
        JLOG(ctx_.journal.trace()) << "RecurringPaymentClaim: Recurring payment not found";
        return tecNO_TARGET;
    }
    
    auto const debitAccount = sle->getAccountID(sfAccount);
    auto const sled = ctx_.view().peek(keylet::account(debitAccount));
    if (!sled)
    {
        JLOG(ctx_.journal.trace()) << "RecurringPaymentClaim: Debit account not found";
        return tefINTERNAL;
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

    // - `ClaimedThisPeriod + Amount` exceeds the authorized amount for the period.
    if (sle->getFieldAmount(sfClaimedThisPeriod) + ctx_.tx.getFieldAmount(sfAmount) > sle->getFieldAmount(sfLimitAmount))
    {
        JLOG(ctx_.journal.trace()) << "RecurringPaymentClaim: Claimed amount exceeds authorized amount for the period";
        return tecLIMIT_EXCEEDED;
    }

    // Add the claimed amount to the current period's claimed amount
    STAmount const claimAmount = ctx_.tx.getFieldAmount(sfAmount);
    sle->setFieldAmount(sfClaimedThisPeriod, sle->getFieldAmount(sfClaimedThisPeriod) + claimAmount);

    // Deduct the claimed amount from the locked funds
    // STAmount const lockedAmount = sle->getFieldAmount(sfLockedAmount);
    // if (lockedAmount < claimAmount)
    // {
    //     JLOG(ctx_.journal.trace()) << "RecurringPaymentClaim: Insufficient locked funds";
    //     return tecINSUFFICIENT_FUNDS;
    // }
    // sle->setFieldAmount(sfLockedAmount, lockedAmount - claimAmount);

    if (!sle->isFieldPresent(sfDestination))
    {
        // Credit the destination account
        (*slec)[sfBalance] = (*slec)[sfBalance] + claimAmount;
        // Debit the source account
        (*sle)[sfAmount] = (*sle)[sfAmount] - claimAmount;
    }
    else
    {
        // Credit the account
        (*slec)[sfBalance] = (*slec)[sfBalance] + claimAmount;
        // Debit the account
        (*sled)[sfBalance] = (*sled)[sfBalance] - claimAmount;
    }
    // update the view
    ctx_.view().update(sle);
    ctx_.view().update(slec);
    ctx_.view().update(sled);
    return tesSUCCESS;
}

}  // namespace ripple
