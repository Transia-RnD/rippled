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

#pragma once

#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/UintTypes.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ripple {

enum class StopType : uint8_t {
    STOP_LOSS_SELL = 1,    // Trigger sell when price <= stopPrice
    STOP_LOSS_BUY = 2,     // Trigger buy when price >= stopPrice
    TAKE_PROFIT_SELL = 3,  // Trigger sell when price >= stopPrice
    TAKE_PROFIT_BUY = 4    // Trigger buy when price <= stopPrice
};

struct StopLossOrder
{
    uint256 txHash;
    std::vector<uint8_t> signedTxBlob;
    double stopPrice;
    StopType stopType;
    AccountID account;
    Currency takerGetsCurrency;
    AccountID takerGetsIssuer;
    Currency takerPaysCurrency;
    AccountID takerPaysIssuer;
};

class StopLossDB
{
public:
    StopLossDB(const std::string& dbPath, unsigned int maxDBs, size_t mapSize);
    ~StopLossDB();

    bool
    init();
    void
    close();

    // Add a new stop-loss order
    bool
    addOrder(
        uint256 const& txHash,
        std::vector<uint8_t> const& signedBlob,
        double stopPrice,
        StopType stopType,
        STAmount const& takerGets,
        STAmount const& takerPays);

    // Get triggered orders based on current price for a specific market
    std::vector<StopLossOrder>
    getTriggeredOrders(
        double currentPrice,
        StopType stopType,
        Book const& market,
        size_t maxOrders = 1000);

    // Remove orders after execution
    void
    removeOrders(std::vector<uint256> const& txHashes);

    // Get order count for monitoring
    size_t
    getOrderCount() const;

    // Get all markets that have stop orders
    std::vector<Book>
    getMonitoredMarkets() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ripple