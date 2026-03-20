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

#include <xrpl/tx/transactors/Option/MarginUtils.h>

#include <xrpl/ledger/Dir.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STNumber.h>

#include <algorithm>
#include <vector>

namespace xrpl {
namespace margin {

Number
calculateInitialMargin(Number notional, std::uint32_t leverage)
{
    if (leverage == 0)
        return notional;  // No leverage = full collateral
    return notional / Number(leverage);
}

Number
calculateMaintenanceMargin(Number notional, std::uint32_t maintenanceMarginBps)
{
    // maintenanceMarginBps is in 1/10 basis points
    // 1 basis point = 0.01%, so 1/10 bps = 0.001%
    // 10000 bps = 100%, so 100000 (1/10 bps) = 100%
    return notional * Number(maintenanceMarginBps) / Number(100000);
}

Number
calculateLiquidationPrice(
    Number entryPrice,
    std::uint32_t positionSide,
    std::uint32_t leverage,
    std::uint32_t maintenanceMarginBps)
{
    if (leverage == 0)
        return Number(0);  // No leverage = no liquidation

    Number const initialMarginRate = Number(1) / Number(leverage);
    Number const maintenanceRate =
        Number(maintenanceMarginBps) / Number(100000);

    if (positionSide == 0)  // Long
    {
        // liqPrice = entryPrice * (1 - initialMarginRate + maintenanceRate)
        return entryPrice *
            (Number(1) - initialMarginRate + maintenanceRate);
    }
    else  // Short
    {
        // liqPrice = entryPrice * (1 + initialMarginRate - maintenanceRate)
        return entryPrice *
            (Number(1) + initialMarginRate - maintenanceRate);
    }
}

Number
calculateUnrealizedPnl(
    std::shared_ptr<SLE const> const& position,
    Number markPrice)
{
    Number const entryPrice =
        position->at(~sfEntryPrice).value_or(Number(0));
    Number const positionSize =
        position->at(~sfPositionSize).value_or(Number(0));
    std::uint32_t const positionSide = position->getFieldU32(sfPositionSide);

    if (positionSide == 0)  // Long
    {
        return (markPrice - entryPrice) * positionSize;
    }
    else  // Short
    {
        return (entryPrice - markPrice) * positionSize;
    }
}

bool
isMarginHealthy(
    ReadView const& view,
    std::shared_ptr<SLE const> const& marginAccount,
    Number markPrice)
{
    AccountID const account = marginAccount->getAccountID(sfAccount);

    // Check each position individually (isolated margin)
    std::uint32_t const now =
        view.parentCloseTime().time_since_epoch().count();
    Dir const ownerDir(view, keylet::ownerDir(account));
    for (auto const& sle : ownerDir)
    {
        if (sle->getType() != ltMARGIN_POSITION)
            continue;
        if (sle->getFieldH256(sfMarginAccountID) != marginAccount->key())
            continue;

        Number allocatedMargin =
            sle->at(~sfAllocatedMargin).value_or(Number(0));
        Number pnl = calculateUnrealizedPnl(sle, markPrice);

        // Deduct accumulated funding
        Issue const posIssue =
            sle->getFieldIssue(sfAsset).get<Issue>();
        Issue const quoteIssue =
            marginAccount->getFieldIssue(sfCollateralAsset).get<Issue>();
        Number accFunding = calculateAccumulatedFunding(
            sle, now, getFundingRateBps(view, posIssue, quoteIssue));
        Number positionEquity = (allocatedMargin - accFunding) + pnl;

        Number notional =
            sle->at(~sfNotionalValue).value_or(Number(0));
        // Read maintenance margin from leverage tier (default 5%)
        std::uint32_t maintenanceBps = 5000;
        auto const sleTier =
            view.read(keylet::leverageTier(posIssue, quoteIssue));
        if (sleTier && sleTier->isFieldPresent(sfMaintenanceMarginBps))
            maintenanceBps =
                sleTier->getFieldU32(sfMaintenanceMarginBps);
        Number maintenance =
            calculateMaintenanceMargin(notional, maintenanceBps);

        if (positionEquity < maintenance)
            return false;
    }
    return true;
}

Number
getMarkPrice(
    ReadView const& view,
    std::shared_ptr<SLE const> const& slePair)
{
    if (!slePair || !slePair->isFieldPresent(sfOracleEntries))
        return Number(0);

    // Read base/quote assets from the OptionPair
    Issue const baseIssue = slePair->getFieldIssue(sfAsset).get<Issue>();
    Issue const quoteIssue = slePair->getFieldIssue(sfAsset2).get<Issue>();

    // Collect prices via direct O(1) oracle lookups
    std::vector<STAmount> prices;

    auto const& oracleEntries = slePair->getFieldArray(sfOracleEntries);
    for (auto const& oracleRef : oracleEntries)
    {
        auto const account = oracleRef.getAccountID(sfAccount);
        auto const docID = oracleRef.getFieldU32(sfOracleDocumentID);

        auto const sleOracle = view.read(keylet::oracle(account, docID));
        if (!sleOracle || !sleOracle->isFieldPresent(sfPriceDataSeries))
            continue;

        auto const& series = sleOracle->getFieldArray(sfPriceDataSeries);
        for (auto const& entry : series)
        {
            if (!entry.isFieldPresent(sfBaseAsset) ||
                !entry.isFieldPresent(sfQuoteAsset) ||
                !entry.isFieldPresent(sfAssetPrice))
                continue;

            Currency const entryBase =
                entry.getFieldCurrency(sfBaseAsset).value();
            Currency const entryQuote =
                entry.getFieldCurrency(sfQuoteAsset).value();

            if (entryBase != baseIssue.currency ||
                entryQuote != quoteIssue.currency)
                continue;

            auto const price = entry.getFieldU64(sfAssetPrice);
            int const scale = entry.isFieldPresent(sfScale)
                ? -static_cast<int>(entry.getFieldU8(sfScale))
                : 0;

            prices.push_back(STAmount{noIssue(), price, scale});
        }
    }

    if (prices.empty())
        return Number(0);

    // Compute median price (same algorithm as GetAggregatePrice RPC)
    std::sort(prices.begin(), prices.end());

    auto const size = prices.size();
    auto const middle = size / 2;

    if (size % 2 == 1)
    {
        return Number(prices[middle]);
    }
    else
    {
        STAmount const two{noIssue(), 2, 0};
        STAmount const sum = prices[middle - 1] + prices[middle];
        return Number(divide(sum, two, noIssue()));
    }
}

std::uint32_t
getFundingRateBps(
    ReadView const& view,
    Issue const& base,
    Issue const& quote)
{
    std::uint32_t fundingRateBps = 100;  // default 0.01%/hr
    auto const sleTier = view.read(keylet::leverageTier(base, quote));
    if (sleTier && sleTier->isFieldPresent(sfFundingRateBps))
        fundingRateBps = sleTier->getFieldU32(sfFundingRateBps);
    return fundingRateBps;
}

Number
calculateAccumulatedFunding(
    std::shared_ptr<SLE const> const& position,
    std::uint32_t currentTime,
    std::uint32_t fundingRateBps)
{
    std::uint32_t const lastFunding =
        position->at(~sfLastFundingTime).value_or(0);

    // Legacy positions (lastFundingTime=0) or clock edge case
    if (lastFunding == 0 || currentTime <= lastFunding)
        return Number(0);

    std::uint32_t const elapsed = currentTime - lastFunding;
    std::uint32_t const hoursElapsed = elapsed / 3600;

    if (hoursElapsed == 0)
        return Number(0);

    Number const notional =
        position->at(~sfNotionalValue).value_or(Number(0));
    Number const funding =
        notional * Number(fundingRateBps) * Number(hoursElapsed) / Number(100000);

    // Cap at allocated margin
    Number const allocatedMargin =
        position->at(~sfAllocatedMargin).value_or(Number(0));
    return std::min(funding, allocatedMargin);
}

Number
deductFundingAndRelease(
    ApplyView& view,
    std::shared_ptr<SLE> const& position,
    std::uint32_t currentTime)
{
    Number const allocatedMargin =
        position->at(~sfAllocatedMargin).value_or(Number(0));

    // Look up funding rate
    Issue const issue = position->getFieldIssue(sfAsset).get<Issue>();
    uint256 const optionPairID = position->getFieldH256(sfOptionPairID);
    auto const slePair = view.read(Keylet{ltOPTION_PAIR, optionPairID});
    if (!slePair)
    {
        // No pair found — return full margin without funding deduction
        position->at(sfAllocatedMargin) =
            STNumber{sfAllocatedMargin, Number(0)};
        view.update(position);
        return allocatedMargin;
    }

    Issue const quoteIssue = slePair->getFieldIssue(sfAsset2).get<Issue>();
    std::uint32_t const fundingRateBps =
        getFundingRateBps(view, issue, quoteIssue);

    Number const funding =
        calculateAccumulatedFunding(position, currentTime, fundingRateBps);
    Number const netMargin = allocatedMargin - funding;

    // Zero out allocated margin on position (satisfies deletion invariant)
    position->at(sfAllocatedMargin) =
        STNumber{sfAllocatedMargin, Number(0)};
    view.update(position);

    // Route funding to OptionPair accumulated fees
    if (funding > Number(0))
    {
        auto slePairMut = view.peek(Keylet{ltOPTION_PAIR, optionPairID});
        if (slePairMut)
        {
            Number currentFees =
                slePairMut->at(~sfAccumulatedFees).value_or(Number(0));
            currentFees = currentFees + funding;
            slePairMut->at(sfAccumulatedFees) =
                STNumber{sfAccumulatedFees, currentFees};
            view.update(slePairMut);
        }
    }

    return netMargin;
}

}  // namespace margin
}  // namespace xrpl
