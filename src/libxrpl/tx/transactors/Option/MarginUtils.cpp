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
    std::uint32_t const marginMode = marginAccount->getFieldU32(sfMarginMode);
    AccountID const account = marginAccount->getAccountID(sfAccount);

    if (marginMode == 1)  // Cross-margin
    {
        Number equity =
            calculateAccountEquity(view, account, marginAccount, markPrice);

        // Sum total maintenance margin across all positions
        Number totalMaintenance(0);
        Dir const ownerDir(view, keylet::ownerDir(account));
        for (auto const& sle : ownerDir)
        {
            if (sle->getType() != ltMARGIN_POSITION)
                continue;
            if (sle->getFieldH256(sfMarginAccountID) != marginAccount->key())
                continue;

            Number notional =
                sle->at(~sfNotionalValue).value_or(Number(0));
            // Use a default 5% maintenance margin (500 bps * 10 = 5000 1/10 bps)
            // In practice, this should come from the leverage tier
            totalMaintenance = totalMaintenance +
                calculateMaintenanceMargin(notional, 5000);
        }

        return equity >= totalMaintenance;
    }
    else  // Isolated margin - check per-position
    {
        // For isolated, the caller should check individual positions
        // This is a fallback that checks all positions
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
            Number positionEquity = allocatedMargin + pnl;

            Number notional =
                sle->at(~sfNotionalValue).value_or(Number(0));
            Number maintenance =
                calculateMaintenanceMargin(notional, 5000);

            if (positionEquity < maintenance)
                return false;
        }
        return true;
    }
}

Number
calculateAccountEquity(
    ReadView const& view,
    AccountID const& account,
    std::shared_ptr<SLE const> const& marginAccount,
    Number markPrice)
{
    Number equity =
        marginAccount->at(~sfCollateralBalance).value_or(Number(0));

    // Iterate all margin positions linked to this margin account
    Dir const ownerDir(view, keylet::ownerDir(account));
    for (auto const& sle : ownerDir)
    {
        if (sle->getType() != ltMARGIN_POSITION)
            continue;

        // Only include positions linked to this margin account
        if (sle->getFieldH256(sfMarginAccountID) != marginAccount->key())
            continue;

        equity = equity + calculateUnrealizedPnl(sle, markPrice);
    }

    return equity;
}

Number
getMarkPrice(
    ReadView const& view,
    Asset const& baseAsset,
    Asset const& quoteAsset)
{
    // Get the currencies to match against oracle price data
    Issue const baseIssue = baseAsset.get<Issue>();
    Issue const quoteIssue = quoteAsset.get<Issue>();

    // Collect prices from oracle entries owned by the asset issuers.
    // We scan the owner directories of both issuers for ltORACLE entries
    // that contain price data for our asset pair.
    std::vector<STAmount> prices;

    auto collectFromIssuer = [&](AccountID const& issuer) {
        Dir const ownerDir(view, keylet::ownerDir(issuer));
        for (auto const& sle : ownerDir)
        {
            if (sle->getType() != ltORACLE)
                continue;

            // Check the price data series for our asset pair
            if (!sle->isFieldPresent(sfPriceDataSeries))
                continue;

            auto const& series = sle->getFieldArray(sfPriceDataSeries);
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

                // Extract price with scale
                auto const price = entry.getFieldU64(sfAssetPrice);
                int const scale = entry.isFieldPresent(sfScale)
                    ? -static_cast<int>(entry.getFieldU8(sfScale))
                    : 0;

                prices.push_back(STAmount{noIssue(), price, scale});
            }
        }
    };

    // Scan both asset issuers for oracle entries
    if (!isXRP(baseIssue))
        collectFromIssuer(baseIssue.account);
    if (!isXRP(quoteIssue) && quoteIssue.account != baseIssue.account)
        collectFromIssuer(quoteIssue.account);

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
        // Average of two middle values
        STAmount const two{noIssue(), 2, 0};
        STAmount const sum = prices[middle - 1] + prices[middle];
        return Number(divide(sum, two, noIssue()));
    }
}

}  // namespace margin
}  // namespace xrpl
