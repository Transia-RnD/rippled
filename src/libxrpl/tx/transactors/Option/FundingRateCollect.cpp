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

#include <xrpl/tx/transactors/Option/FundingRateCollect.h>
#include <xrpl/tx/transactors/Option/MarginUtils.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
FundingRateCollect::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "FundingRateCollect: invalid flags.";
        return temINVALID_FLAG;
    }

    return tesSUCCESS;
}

TER
FundingRateCollect::preclaim(PreclaimContext const& ctx)
{
    uint256 const marginPositionID = ctx.tx[sfMarginPositionID];

    // Verify margin position exists
    auto const slePosition =
        ctx.view.read(Keylet{ltMARGIN_POSITION, marginPositionID});
    if (!slePosition)
    {
        JLOG(ctx.j.debug())
            << "FundingRateCollect: margin position does not exist.";
        return tecNO_ENTRY;
    }

    // Check that enough time has passed since last funding
    // Funding interval: 1 hour (3600 seconds)
    std::uint32_t const lastFunding =
        slePosition->at(~sfLastFundingTime).value_or(0);
    std::uint32_t const now = ctx.view.parentCloseTime().time_since_epoch().count();

    if (now - lastFunding < 3600)
    {
        JLOG(ctx.j.debug())
            << "FundingRateCollect: too soon since last funding.";
        return tecNO_PERMISSION;
    }

    return tesSUCCESS;
}

TER
FundingRateCollect::doApply()
{
    Sandbox sb(&ctx_.view());

    uint256 const marginPositionID = ctx_.tx[sfMarginPositionID];

    auto slePosition =
        sb.peek(Keylet{ltMARGIN_POSITION, marginPositionID});
    if (!slePosition)
        return tecNO_ENTRY;

    // Get the margin account
    uint256 const marginAccountID =
        slePosition->getFieldH256(sfMarginAccountID);
    auto sleMarginAcct =
        sb.peek(Keylet{ltMARGIN_ACCOUNT, marginAccountID});
    if (!sleMarginAcct)
        return tecNO_ENTRY;

    // Get the leverage tier to find the funding rate
    Issue const issue =
        slePosition->getFieldIssue(sfAsset).get<Issue>();
    uint256 const optionPairID =
        slePosition->getFieldH256(sfOptionPairID);
    auto const slePair =
        sb.read(Keylet{ltOPTION_PAIR, optionPairID});
    if (!slePair)
        return tecNO_ENTRY;

    Issue const quoteIssue =
        slePair->getFieldIssue(sfAsset2).get<Issue>();

    auto const sleTier =
        sb.read(keylet::leverageTier(issue, quoteIssue));

    // Default funding rate: 10 bps (0.01%) per hour
    // This is the base interest rate, similar to Hyperliquid's 0.01% per 8h
    std::uint32_t fundingRateBps = 100;  // 0.01% in 1/10 bps

    // Calculate funding payment
    // fundingPayment = notional * fundingRate / 100000
    Number const notional =
        slePosition->at(~sfNotionalValue).value_or(Number(0));
    Number const fundingPayment =
        notional * Number(fundingRateBps) / Number(100000);

    // Deduct funding from the position's allocated margin
    Number allocatedMargin =
        slePosition->at(~sfAllocatedMargin).value_or(Number(0));

    if (allocatedMargin < fundingPayment)
    {
        // Position cannot pay funding - could trigger liquidation
        // For now, deduct whatever is available
        Number const actualPayment = allocatedMargin;
        allocatedMargin = Number(0);
        slePosition->at(sfAllocatedMargin) =
            STNumber{sfAllocatedMargin, allocatedMargin};
        slePosition->at(sfLastFundingPayment) =
            STNumber{sfLastFundingPayment, actualPayment};
    }
    else
    {
        allocatedMargin = allocatedMargin - fundingPayment;
        slePosition->at(sfAllocatedMargin) =
            STNumber{sfAllocatedMargin, allocatedMargin};
        slePosition->at(sfLastFundingPayment) =
            STNumber{sfLastFundingPayment, fundingPayment};
    }

    // Route the funding payment to the OptionPair accumulated fees
    auto slePairMut = sb.peek(Keylet{ltOPTION_PAIR, optionPairID});
    if (slePairMut)
    {
        Number currentFees =
            slePairMut->at(~sfAccumulatedFees).value_or(Number(0));
        currentFees = currentFees + fundingPayment;
        slePairMut->at(sfAccumulatedFees) =
            STNumber{sfAccumulatedFees, currentFees};
        sb.update(slePairMut);
    }

    // Update last funding timestamp
    std::uint32_t const now =
        sb.parentCloseTime().time_since_epoch().count();
    slePosition->setFieldU32(sfLastFundingTime, now);

    sb.update(slePosition);
    sb.update(sleMarginAcct);

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
