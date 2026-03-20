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

#include <xrpl/tx/transactors/Option/MarginWithdraw.h>
#include <xrpl/tx/transactors/Option/MarginUtils.h>
#include <xrpl/ledger/Dir.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
MarginWithdraw::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "MarginWithdraw: invalid flags.";
        return temINVALID_FLAG;
    }

    // Validate withdrawal amount is positive
    STAmount const amount = ctx.tx[sfAmount];
    if (amount <= beast::zero)
    {
        JLOG(ctx.j.debug()) << "MarginWithdraw: amount must be positive.";
        return temBAD_AMOUNT;
    }

    return tesSUCCESS;
}

TER
MarginWithdraw::preclaim(PreclaimContext const& ctx)
{
    auto const accountID = ctx.tx[sfAccount];
    uint256 const marginAccountID = ctx.tx[sfMarginAccountID];

    // Verify margin account exists
    auto const sleMarginAcct =
        ctx.view.read(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
    {
        JLOG(ctx.j.debug())
            << "MarginWithdraw: margin account does not exist.";
        return tecNO_ENTRY;
    }

    // Verify the caller owns this margin account
    if (sleMarginAcct->getAccountID(sfAccount) != accountID)
    {
        JLOG(ctx.j.debug())
            << "MarginWithdraw: account does not own this margin account.";
        return tecNO_PERMISSION;
    }

    // Verify withdrawal amount matches collateral asset
    STAmount const amount = ctx.tx[sfAmount];
    Issue const collateralIssue =
        sleMarginAcct->getFieldIssue(sfCollateralAsset).get<Issue>();
    if (amount.issue() != collateralIssue)
    {
        JLOG(ctx.j.debug())
            << "MarginWithdraw: amount currency must match collateral asset.";
        return temMALFORMED;
    }

    // Verify sufficient collateral balance
    Number currentBalance =
        sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
    if (currentBalance < Number(amount))
    {
        JLOG(ctx.j.debug())
            << "MarginWithdraw: insufficient collateral balance.";
        return tecUNFUNDED_PAYMENT;
    }

    return tesSUCCESS;
}

TER
MarginWithdraw::doApply()
{
    Sandbox sb(&ctx_.view());

    uint256 const marginAccountID = ctx_.tx[sfMarginAccountID];
    STAmount const amount = ctx_.tx[sfAmount];

    // Read margin account
    auto sleMarginAcct = sb.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
        return tecNO_ENTRY;

    auto sleSource = sb.peek(keylet::account(account_));
    if (!sleSource)
        return terNO_ACCOUNT;

    // Check that withdrawal doesn't breach maintenance margin.
    Number currentBalance =
        sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
    Number withdrawAmount = Number(amount);

    if (currentBalance < withdrawAmount)
    {
        JLOG(j_.debug()) << "MarginWithdraw: insufficient collateral.";
        return tecUNFUNDED_PAYMENT;
    }

    // Mark-to-market check: after withdrawal, verify margin remains healthy.
    // Simulate the withdrawal by checking if the reduced balance still
    // satisfies maintenance margin requirements for all open positions.
    Issue const collateralIssue =
        sleMarginAcct->getFieldIssue(sfCollateralAsset).get<Issue>();

    // Try to get a mark price for any open positions.
    // If we have open positions AND a valid mark price, enforce margin health.
    Number const postWithdrawBalance = currentBalance - withdrawAmount;

    // Temporarily set the reduced balance to check health
    sleMarginAcct->at(sfCollateralBalance) =
        STNumber{sfCollateralBalance, postWithdrawBalance};

    // Find the base asset from any linked position to get mark price
    // We check margin health with the reduced balance
    Dir const ownerDir(sb, keylet::ownerDir(account_));
    bool hasPositions = false;
    for (auto const& sle : ownerDir)
    {
        if (sle->getType() == ltMARGIN_POSITION &&
            sle->getFieldH256(sfMarginAccountID) == marginAccountID)
        {
            hasPositions = true;
            break;
        }
    }

    if (hasPositions)
    {
        // Get mark price from oracle for margin health check
        Number markPrice(0);
        for (auto const& sle : ownerDir)
        {
            if (sle->getType() != ltMARGIN_POSITION)
                continue;
            if (sle->getFieldH256(sfMarginAccountID) != marginAccountID)
                continue;

            auto const slePair = sb.read(
                Keylet{ltOPTION_PAIR, sle->getFieldH256(sfOptionPairID)});
            if (slePair)
                markPrice = margin::getMarkPrice(sb, slePair);
            if (markPrice > Number(0))
                break;
        }

        // SECURITY: If positions exist but no oracle price available,
        // reject withdrawal. Cannot verify margin health without price.
        if (markPrice <= Number(0))
        {
            sleMarginAcct->at(sfCollateralBalance) =
                STNumber{sfCollateralBalance, currentBalance};
            JLOG(j_.debug())
                << "MarginWithdraw: no oracle price available, "
                   "cannot verify margin health.";
            return tecNO_PERMISSION;
        }

        if (!margin::isMarginHealthy(sb, sleMarginAcct, markPrice))
        {
            sleMarginAcct->at(sfCollateralBalance) =
                STNumber{sfCollateralBalance, currentBalance};
            JLOG(j_.debug())
                << "MarginWithdraw: withdrawal would breach "
                   "maintenance margin.";
            return tecNO_PERMISSION;
        }
    }

    // Restore balance to current (we'll deduct properly below)
    sleMarginAcct->at(sfCollateralBalance) =
        STNumber{sfCollateralBalance, currentBalance};

    // Transfer tokens back to user from custodial account
    if (isXRP(amount))
    {
        // Mint XRP back to the withdrawer (counterpart of burn on deposit)
        auto const sourceBalance = sleSource->getFieldAmount(sfBalance);
        sleSource->setFieldAmount(sfBalance, sourceBalance + amount);
        sb.update(sleSource);
        sb.rawDestroyXRP(-amount.xrp());
    }
    else
    {
        // For IOU withdrawals, transfer from collateral issuer (custodial)
        auto const ter = accountSend(
            sb, collateralIssue.account, account_, amount, j_);
        if (ter != tesSUCCESS)
        {
            JLOG(j_.debug()) << "MarginWithdraw: failed to transfer IOU.";
            return ter;
        }
    }

    // Update collateral balance
    currentBalance = currentBalance - withdrawAmount;
    sleMarginAcct->at(sfCollateralBalance) =
        STNumber{sfCollateralBalance, currentBalance};

    sb.update(sleMarginAcct);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
