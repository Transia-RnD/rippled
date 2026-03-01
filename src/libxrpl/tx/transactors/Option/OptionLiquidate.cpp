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

#include <xrpl/tx/transactors/Option/OptionLiquidate.h>
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
OptionLiquidate::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "OptionLiquidate: invalid flags.";
        return temINVALID_FLAG;
    }

    return tesSUCCESS;
}

TER
OptionLiquidate::preclaim(PreclaimContext const& ctx)
{
    uint256 const marginPositionID = ctx.tx[sfMarginPositionID];

    // Verify margin position exists
    auto const slePosition =
        ctx.view.read(Keylet{ltMARGIN_POSITION, marginPositionID});
    if (!slePosition)
    {
        JLOG(ctx.j.debug())
            << "OptionLiquidate: margin position does not exist.";
        return tecNO_ENTRY;
    }

    // Look up the margin account
    uint256 const marginAccountID =
        slePosition->getFieldH256(sfMarginAccountID);
    auto const sleMarginAcct =
        ctx.view.read(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
    {
        JLOG(ctx.j.debug())
            << "OptionLiquidate: margin account does not exist.";
        return tecNO_ENTRY;
    }

    // Get the asset pair to look up mark price
    Issue const issue =
        slePosition->getFieldIssue(sfAsset).get<Issue>();
    uint256 const optionPairID =
        slePosition->getFieldH256(sfOptionPairID);
    auto const slePair =
        ctx.view.read(Keylet{ltOPTION_PAIR, optionPairID});
    if (!slePair)
    {
        JLOG(ctx.j.debug())
            << "OptionLiquidate: option pair does not exist.";
        return tecNO_ENTRY;
    }

    // SECURITY: Prevent self-liquidation (owner cannot liquidate own position
    // to extract the liquidation bonus)
    AccountID const posOwner = slePosition->getAccountID(sfAccount);
    if (ctx.tx[sfAccount] == posOwner)
    {
        JLOG(ctx.j.debug())
            << "OptionLiquidate: cannot liquidate own position.";
        return tecNO_PERMISSION;
    }

    // Get mark price from oracle
    Issue const quoteIssue = slePair->getFieldIssue(sfAsset2).get<Issue>();
    Number const markPrice =
        margin::getMarkPrice(ctx.view, issue, quoteIssue);

    // SECURITY: Reject liquidation if no oracle price available.
    // markPrice=0 would make all long positions appear underwater.
    if (markPrice <= Number(0))
    {
        JLOG(ctx.j.debug())
            << "OptionLiquidate: no oracle price available.";
        return tecNO_PERMISSION;
    }

    // Check if the position is actually underwater
    std::uint32_t const marginMode =
        sleMarginAcct->getFieldU32(sfMarginMode);

    if (marginMode == 1)  // Cross-margin
    {
        // For cross-margin, check account-level health
        if (margin::isMarginHealthy(ctx.view, sleMarginAcct, markPrice))
        {
            JLOG(ctx.j.debug())
                << "OptionLiquidate: position is healthy (cross-margin).";
            return tecCANT_LIQUIDATE;
        }
    }
    else  // Isolated margin
    {
        // For isolated, check this specific position
        Number const allocatedMargin =
            slePosition->at(~sfAllocatedMargin).value_or(Number(0));
        Number const pnl =
            margin::calculateUnrealizedPnl(slePosition, markPrice);
        Number const positionEquity = allocatedMargin + pnl;

        Number const notional =
            slePosition->at(~sfNotionalValue).value_or(Number(0));
        // Use 5% default maintenance margin
        Number const maintenance =
            margin::calculateMaintenanceMargin(notional, 5000);

        if (positionEquity >= maintenance)
        {
            JLOG(ctx.j.debug())
                << "OptionLiquidate: position is healthy (isolated).";
            return tecCANT_LIQUIDATE;
        }
    }

    return tesSUCCESS;
}

TER
OptionLiquidate::doApply()
{
    Sandbox sb(&ctx_.view());

    uint256 const marginPositionID = ctx_.tx[sfMarginPositionID];

    auto slePosition =
        sb.peek(Keylet{ltMARGIN_POSITION, marginPositionID});
    if (!slePosition)
        return tecNO_ENTRY;

    AccountID const positionOwner = slePosition->getAccountID(sfAccount);

    // SECURITY: Re-verify self-liquidation guard in doApply (TOCTOU defense)
    if (account_ == positionOwner)
        return tecNO_PERMISSION;

    uint256 const marginAccountID =
        slePosition->getFieldH256(sfMarginAccountID);

    auto sleMarginAcct =
        sb.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
        return tecNO_ENTRY;

    // Get the asset pair info
    uint256 const optionPairID =
        slePosition->getFieldH256(sfOptionPairID);
    auto slePair = sb.peek(Keylet{ltOPTION_PAIR, optionPairID});
    if (!slePair)
        return tecNO_ENTRY;

    Issue const issue =
        slePosition->getFieldIssue(sfAsset).get<Issue>();
    Issue const quoteIssue =
        slePair->getFieldIssue(sfAsset2).get<Issue>();

    // Get mark price
    Number const markPrice = margin::getMarkPrice(sb, issue, quoteIssue);

    // SECURITY: Re-verify oracle availability in doApply (TOCTOU defense)
    if (markPrice <= Number(0))
        return tecNO_PERMISSION;

    // SECURITY: Re-verify position is still underwater in doApply (TOCTOU defense)
    {
        Number const am =
            slePosition->at(~sfAllocatedMargin).value_or(Number(0));
        Number const p = margin::calculateUnrealizedPnl(slePosition, markPrice);
        Number const eq = am + p;
        Number const n =
            slePosition->at(~sfNotionalValue).value_or(Number(0));
        Number const maint = margin::calculateMaintenanceMargin(n, 5000);
        if (eq >= maint)
            return tecCANT_LIQUIDATE;
    }

    // Calculate remaining collateral after PnL
    Number const allocatedMargin =
        slePosition->at(~sfAllocatedMargin).value_or(Number(0));
    Number const pnl =
        margin::calculateUnrealizedPnl(slePosition, markPrice);
    Number const remainingCollateral = allocatedMargin + pnl;

    // Look up liquidation bonus rate from the leverage tier
    auto const sleTier =
        sb.read(keylet::leverageTier(issue, quoteIssue));
    std::uint32_t liquidationBonusBps = 500;  // default 0.5%
    if (sleTier)
        liquidationBonusBps = sleTier->getFieldU32(sfLiquidationBonusBps);

    Number const notional =
        slePosition->at(~sfNotionalValue).value_or(Number(0));

    // Calculate liquidation bonus for the liquidator
    // Only compute bonus when remainingCollateral is positive
    Number liquidationBonus(0);
    if (remainingCollateral > Number(0))
    {
        Number const bonusFromCollateral =
            remainingCollateral * Number(liquidationBonusBps) / Number(100000);
        Number const maxCap = notional * Number(100) / Number(100000);
        liquidationBonus = std::min(bonusFromCollateral, maxCap);
        if (liquidationBonus < Number(0))
            liquidationBonus = Number(0);
    }

    // Calculate surplus after bonus
    Number const surplus = remainingCollateral - liquidationBonus;

    // Pay liquidation bonus to the liquidator (caller of this transaction)
    // Bonus is deducted from the position's collateral and credited to liquidator
    if (liquidationBonus > Number(0))
    {
        auto sleLiquidator = sb.peek(keylet::account(account_));
        if (sleLiquidator)
        {
            if (isXRP(quoteIssue))
            {
                // For XRP: credit liquidator's balance
                // Note: this XRP comes from the margin system's pool
                auto const balance =
                    sleLiquidator->getFieldAmount(sfBalance);
                STAmount const bonusAmount(
                    quoteIssue,
                    static_cast<std::uint64_t>(
                        liquidationBonus.mantissa() > 0
                            ? liquidationBonus.mantissa()
                            : 0),
                    liquidationBonus.exponent());
                sleLiquidator->setFieldAmount(
                    sfBalance, balance + bonusAmount);
                sb.update(sleLiquidator);
            }
            else
            {
                // For IOU: transfer from collateral issuer to liquidator
                STAmount const bonusAmount(
                    quoteIssue,
                    static_cast<std::uint64_t>(
                        liquidationBonus.mantissa() > 0
                            ? liquidationBonus.mantissa()
                            : 0),
                    liquidationBonus.exponent());
                [[maybe_unused]] auto const ter = accountSend(
                    sb, quoteIssue.account, account_, bonusAmount, j_);
            }
        }
    }

    // Return surplus to position owner
    if (surplus > Number(0))
    {
        // Credit back to margin account collateral balance
        Number collateralBalance =
            sleMarginAcct->at(~sfCollateralBalance).value_or(Number(0));
        collateralBalance = collateralBalance + surplus;
        sleMarginAcct->at(sfCollateralBalance) =
            STNumber{sfCollateralBalance, collateralBalance};
    }

    // Insurance vault: absorb bad debt and route accumulated fees
    auto sleInsurance =
        sb.peek(keylet::insuranceVault(issue, quoteIssue));
    if (sleInsurance)
    {
        Number insuranceBalance =
            sleInsurance->at(~sfInsuranceBalance).value_or(Number(0));

        // Bad debt absorption
        if (surplus < Number(0))
        {
            Number const badDebt = -surplus;
            if (insuranceBalance >= badDebt)
                insuranceBalance = insuranceBalance - badDebt;
            // If insufficient, bad debt is socialized
        }

        // Route accumulated trading fees from OptionPair
        Number accumulatedFees =
            slePair->at(~sfAccumulatedFees).value_or(Number(0));
        if (accumulatedFees > Number(0))
        {
            insuranceBalance = insuranceBalance + accumulatedFees;
            slePair->at(sfAccumulatedFees) =
                STNumber{sfAccumulatedFees, Number(0)};
            sb.update(slePair);
        }

        sleInsurance->at(sfInsuranceBalance) =
            STNumber{sfInsuranceBalance, insuranceBalance};
        sb.update(sleInsurance);
    }

    // Remove the linked option offer if it exists
    if (slePosition->isFieldPresent(sfOptionOfferID))
    {
        uint256 const offerID = slePosition->getFieldH256(sfOptionOfferID);
        auto sleOffer = sb.peek(Keylet{ltOPTION_OFFER, offerID});
        if (sleOffer)
        {
            // Clear the margin position link
            // The offer becomes a zombie that will expire naturally
            sleOffer->makeFieldAbsent(sfMarginPositionID);
            sb.update(sleOffer);
        }
    }

    // Delete the margin position from the owner directory
    if (slePosition->isFieldPresent(sfOwnerNode))
    {
        sb.dirRemove(
            keylet::ownerDir(positionOwner),
            slePosition->getFieldU64(sfOwnerNode),
            slePosition->key(),
            true);
    }
    adjustOwnerCount(sb, sb.peek(keylet::account(positionOwner)), -1, j_);

    // Erase the margin position
    sb.erase(slePosition);
    sb.update(sleMarginAcct);

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
