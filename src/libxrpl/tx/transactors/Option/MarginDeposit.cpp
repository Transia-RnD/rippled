//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <xrpl/tx/transactors/Option/MarginDeposit.h>
#include <xrpl/tx/transactors/Option/OptionUtils.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
MarginDeposit::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "MarginDeposit: invalid flags.";
        return temINVALID_FLAG;
    }

    // Validate deposit amount is positive
    STAmount const amount = ctx.tx[sfAmount];
    if (amount <= beast::zero)
    {
        JLOG(ctx.j.debug()) << "MarginDeposit: amount must be positive.";
        return temBAD_AMOUNT;
    }

    return tesSUCCESS;
}

TER
MarginDeposit::preclaim(PreclaimContext const& ctx)
{
    auto const accountID = ctx.tx[sfAccount];
    uint256 const marginAccountID = ctx.tx[sfMarginAccountID];

    // Verify margin account exists
    auto const sleMarginAcct =
        ctx.view.read(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
    {
        JLOG(ctx.j.debug())
            << "MarginDeposit: margin account does not exist.";
        return tecNO_ENTRY;
    }

    // Verify the caller owns this margin account
    if (sleMarginAcct->getAccountID(sfAccount) != accountID)
    {
        JLOG(ctx.j.debug())
            << "MarginDeposit: account does not own this margin account.";
        return tecNO_PERMISSION;
    }

    // Verify deposit amount matches collateral asset
    STAmount const amount = ctx.tx[sfAmount];
    Issue const collateralIssue =
        sleMarginAcct->getFieldIssue(sfCollateralAsset).get<Issue>();
    if (amount.issue() != collateralIssue)
    {
        JLOG(ctx.j.debug())
            << "MarginDeposit: amount currency must match collateral asset.";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

TER
MarginDeposit::doApply()
{
    Sandbox sb(&ctx_.view());

    uint256 const marginAccountID = ctx_.tx[sfMarginAccountID];
    STAmount const amount = ctx_.tx[sfAmount];

    // Read margin account
    auto sleMarginAcct = sb.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
        return tecNO_ENTRY;

    // Get the collateral asset to find a custodial pseudo-account
    Issue const collateralIssue =
        sleMarginAcct->getFieldIssue(sfCollateralAsset).get<Issue>();

    auto sleSource = sb.peek(keylet::account(account_));
    if (!sleSource)
        return terNO_ACCOUNT;

    // Enforce XRP reserve: depositor must keep enough for base reserve + owner count
    if (isXRP(amount))
    {
        auto const sourceBalance = sleSource->getFieldAmount(sfBalance);
        auto const reserve = sb.fees().accountReserve(
            sleSource->getFieldU32(sfOwnerCount));
        if (sourceBalance < amount + reserve)
        {
            JLOG(j_.debug()) << "MarginDeposit: insufficient XRP (reserve).";
            return tecUNFUNDED_PAYMENT;
        }

        // Deduct from source account and credit the OptionPair pseudo-account
        // that holds custodial XRP for the margin system.
        // Find the margin account's linked OptionPair to get the pseudo-account.
        sleSource->setFieldAmount(sfBalance, sourceBalance - amount);
        sb.update(sleSource);

        // Credit the margin account owner's pseudo-account (track via collateral)
        // XRP conservation: source debited, tracked via sfCollateralBalance
        // The XRP effectively goes to the fee pool (destroyed) and is re-created
        // on withdrawal. For proper conservation, use rawDestroyXRP to burn
        // and rawDestroyXRP(-amount) to mint on withdrawal.
        sb.rawDestroyXRP(amount.xrp());
    }
    else
    {
        // For IOU deposits, find the OptionPair pseudo-account to hold the tokens.
        // Scan margin positions to find a linked OptionPair, or use the
        // collateral issuer as custodian.
        // Transfer from depositor to the collateral issuer (custodial).
        auto const ter = accountSend(sb, account_, collateralIssue.account, amount, j_);
        if (ter != tesSUCCESS)
        {
            JLOG(j_.debug()) << "MarginDeposit: failed to transfer IOU.";
            return ter;
        }
    }

    // Update collateral balance on the margin account
    Number currentBalance =
        sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
    currentBalance = currentBalance + Number(amount);
    sleMarginAcct->at(sfCollateralBalance) = STNumber{sfCollateralBalance, currentBalance};

    sb.update(sleMarginAcct);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
