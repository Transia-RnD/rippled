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
#include <xrpld/app/tx/detail/RecurringPaymentLock.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>

namespace ripple {

NotTEC
RecurringPaymentLock::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    std::uint32_t const txFlags = ctx.tx.getFlags();
    if (txFlags & tfUniversalMask)
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentLock: Invalid flags set";
        return temINVALID_FLAG;
    }

    if (!ctx.tx.isFieldPresent(sfAmount) || ctx.tx.getFieldAmount(sfAmount) <= XRPAmount(0))
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentLock: Amount is invalid or <= 0";
        return temMALFORMED;
    }

    return preflight2(ctx);
}

TER
RecurringPaymentLock::checkPermission(ReadView const& view, STTx const& tx)
{
    return tesSUCCESS;
}

TER
RecurringPaymentLock::preclaim(PreclaimContext const& ctx)
{
    auto const sle = ctx.view.read(keylet::recurringPayment(ctx.tx.getFieldH256(sfRecurringPaymentID)));
    if (!sle)
    {
        JLOG(ctx.j.trace()) << "RecurringPaymentLock: Recurring payment not found";
        return tecNO_TARGET;
    }
    
    return tesSUCCESS;
}

TER
RecurringPaymentLock::doApply()
{
    auto const account = ctx_.tx.getAccountID(sfAccount);
    STAmount const lockAmount = ctx_.tx.getFieldAmount(sfAmount);
    auto const slea = ctx_.view().peek(keylet::account(account));
    if (!slea)
    {
        JLOG(ctx_.journal.trace()) << "RecurringPaymentLock: Source account not found";
        return tefINTERNAL;
    }

    auto const sle = ctx_.view().peek(keylet::recurringPayment(ctx_.tx.getFieldH256(sfRecurringPaymentID)));
    if (!sle)
    {
        JLOG(ctx_.journal.trace()) << "RecurringPaymentLock: Recurring payment not found";
        return tecNO_TARGET;
    }

    // Credit the recurring claim
    (*sle)[sfAmount] = (*sle)[sfAmount] + lockAmount;
    // Debit the source account
    (*slea)[sfBalance] = (*slea)[sfBalance] - lockAmount;
    // update the view
    ctx_.view().update(sle);
    ctx_.view().update(slea);
    return tesSUCCESS;
}

}  // namespace ripple
