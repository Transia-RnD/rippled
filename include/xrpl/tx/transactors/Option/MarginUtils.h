#pragma once

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

#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/Asset.h>
#include <xrpl/basics/Number.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/UintTypes.h>

namespace xrpl {
namespace margin {

/**
 * @brief Calculate initial margin required for a position.
 *
 * @param notional The notional value of the position
 * @param leverage The leverage multiplier (e.g. 5 for 5x)
 * @return Number The required initial margin (notional / leverage)
 */
Number
calculateInitialMargin(Number notional, std::uint32_t leverage);

/**
 * @brief Calculate maintenance margin for a position.
 *
 * @param notional The notional value of the position
 * @param maintenanceMarginBps Maintenance margin rate in 1/10 basis points
 * @return Number The maintenance margin requirement
 */
Number
calculateMaintenanceMargin(
    Number notional,
    std::uint32_t maintenanceMarginBps);

/**
 * @brief Calculate the liquidation price for a position.
 *
 * For longs: liqPrice = entryPrice * (1 - 1/leverage + maintenanceRate)
 * For shorts: liqPrice = entryPrice * (1 + 1/leverage - maintenanceRate)
 *
 * @param entryPrice The price at which the position was opened
 * @param positionSide 0 = long, 1 = short
 * @param leverage The leverage multiplier
 * @param maintenanceMarginBps Maintenance margin rate in 1/10 basis points
 * @return Number The liquidation trigger price
 */
Number
calculateLiquidationPrice(
    Number entryPrice,
    std::uint32_t positionSide,
    std::uint32_t leverage,
    std::uint32_t maintenanceMarginBps);

/**
 * @brief Calculate unrealized PnL for a single margin position.
 *
 * For longs: PnL = (markPrice - entryPrice) * positionSize
 * For shorts: PnL = (entryPrice - markPrice) * positionSize
 *
 * @param position The margin position SLE
 * @param markPrice Current mark price from oracle
 * @return Number The unrealized profit or loss
 */
Number
calculateUnrealizedPnl(
    std::shared_ptr<SLE const> const& position,
    Number markPrice);

/**
 * @brief Check if a margin account is healthy (above maintenance margin).
 *
 * For isolated margin: checks single position equity vs maintenance
 * For cross margin: checks total account equity vs total maintenance
 *
 * @param view Ledger view for reading positions
 * @param marginAccount The margin account SLE
 * @param markPrice Current mark price from oracle
 * @return true if margin is healthy, false if liquidatable
 */
bool
isMarginHealthy(
    ReadView const& view,
    std::shared_ptr<SLE const> const& marginAccount,
    Number markPrice);

/**
 * @brief Calculate total account equity for cross-margin.
 *
 * equity = collateralBalance + sum(unrealizedPnl for all positions)
 *
 * @param view Ledger view for reading positions
 * @param account The account ID
 * @param marginAccount The margin account SLE
 * @param markPrice Current mark price from oracle
 * @return Number Total account equity
 */
Number
calculateAccountEquity(
    ReadView const& view,
    AccountID const& account,
    std::shared_ptr<SLE const> const& marginAccount,
    Number markPrice);

/**
 * @brief Get aggregate mark price from the oracle system.
 *
 * Reads oracle entries for the given asset pair and computes
 * the median price, similar to the GetAggregatePrice RPC.
 *
 * @param view Ledger view for reading oracle entries
 * @param baseAsset Base asset of the pair
 * @param quoteAsset Quote asset of the pair
 * @return Number The aggregated mark price (0 if no valid oracles)
 */
Number
getMarkPrice(
    ReadView const& view,
    Asset const& baseAsset,
    Asset const& quoteAsset);

}  // namespace margin
}  // namespace xrpl
