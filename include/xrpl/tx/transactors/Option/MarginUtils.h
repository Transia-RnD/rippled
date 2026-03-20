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
 * @brief Check if all positions in a margin account are healthy.
 *
 * Checks each position's equity (allocatedMargin - funding + PnL)
 * against its maintenance margin requirement.
 *
 * @param view Ledger view for reading positions
 * @param marginAccount The margin account SLE
 * @param markPrice Current mark price from oracle
 * @return true if all positions are healthy, false if any is liquidatable
 */
bool
isMarginHealthy(
    ReadView const& view,
    std::shared_ptr<SLE const> const& marginAccount,
    Number markPrice);

/**
 * @brief Get aggregate mark price from the oracle system.
 *
 * Reads OracleEntries stored on the OptionPair SLE, performs
 * direct O(1) keylet::oracle lookups, and computes the median
 * price — same pattern as the get_aggregate_price RPC.
 *
 * @param view Ledger view for reading oracle entries
 * @param slePair The OptionPair SLE containing OracleEntries
 * @return Number The aggregated mark price (0 if no valid oracles)
 */
Number
getMarkPrice(
    ReadView const& view,
    std::shared_ptr<SLE const> const& slePair);

/**
 * @brief Get funding rate from leverage tier (default 100 = 0.01%/hr).
 */
std::uint32_t
getFundingRateBps(
    ReadView const& view,
    Issue const& base,
    Issue const& quote);

/**
 * @brief Calculate accumulated funding owed since last funding time.
 *
 * hoursElapsed = floor((now - lastFundingTime) / 3600)
 * funding = notional * fundingRateBps * hoursElapsed / 100000
 * Result is capped at allocatedMargin.
 *
 * Returns 0 if lastFundingTime is 0 (legacy positions).
 */
Number
calculateAccumulatedFunding(
    std::shared_ptr<SLE const> const& position,
    std::uint32_t currentTime,
    std::uint32_t fundingRateBps);

/**
 * @brief Deduct accumulated funding from position and route to OptionPair fees.
 *
 * Calculates lazy funding, deducts from sfAllocatedMargin,
 * routes to OptionPair sfAccumulatedFees, and returns the net margin
 * (allocatedMargin - funding).
 */
Number
deductFundingAndRelease(
    ApplyView& view,
    std::shared_ptr<SLE> const& position,
    std::uint32_t currentTime);

}  // namespace margin
}  // namespace xrpl
