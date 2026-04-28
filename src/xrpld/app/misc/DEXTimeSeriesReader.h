#pragma once

#include <xrpl/beast/utility/Journal.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xrpl {

class DEXTimeSeriesStore;

struct DEXCandle
{
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volumeBase = 0.0;
    double volumeQuote = 0.0;
    uint32_t txCount = 0;
    uint32_t timestamp = 0;
    double buyVolumeBase = 0.0;
    double sellVolumeBase = 0.0;
};

struct DEXTrade
{
    std::string bookKey;
    uint32_t ledgerSeq = 0;
    uint32_t txIndex = 0;
    uint32_t timestamp = 0;
    double rate = 0.0;
    double volumeBase = 0.0;
    double volumeQuote = 0.0;
    uint8_t side = 0;
    uint8_t source = 0;
    std::string taker;
    std::string txHash;
};

struct DEXAMMSnapshot
{
    std::string account;
    uint32_t ledgerSeq = 0;
    uint32_t timestamp = 0;
    double asset1Balance = 0.0;
    double asset2Balance = 0.0;
    double lptBalance = 0.0;
    uint16_t tradingFee = 0;
    uint8_t curveType = 0;
    double tvlXrp = 0.0;
    double volume24hXrp = 0.0;
    double fees24hXrp = 0.0;
};

struct DEXTokenSummary
{
    std::string bookKey;
    double lastPrice = 0.0;
    double priceChange5m = 0.0;
    double priceChange1h = 0.0;
    double priceChange24h = 0.0;
    double priceChange7d = 0.0;
    double priceChange30d = 0.0;
    double volume24hBase = 0.0;
    double volume24hQuote = 0.0;
    double high24h = 0.0;
    double low24h = 0.0;
    uint32_t tradeCount24h = 0;
};

struct DEXPoolInfo
{
    std::string account;
    std::string asset1;
    std::string asset2;
    double asset1Balance = 0.0;
    double asset2Balance = 0.0;
    double lptBalance = 0.0;
    uint16_t tradingFee = 0;
    uint8_t curveType = 0;
    double tvlXrp = 0.0;
    double volume24hXrp = 0.0;
    double fees24hXrp = 0.0;
    double apr = 0.0;
    uint32_t ledgerSeq = 0;
    uint32_t timestamp = 0;
};

struct DEXTokenInfo
{
    double supply = 0.0;
    double frozenSupply = 0.0;
    double lockedSupply = 0.0;
    uint32_t holders = 0;
    uint32_t trustLines = 0;
    uint32_t ledgerSeq = 0;
};

enum class DEXInterval : uint8_t
{
    OneMinute = 0,
    FiveMinute = 1,
    OneHour = 2,
    OneDay = 3,
};

class DEXTimeSeriesReader
{
public:
    virtual ~DEXTimeSeriesReader() = default;

    virtual std::optional<uint32_t>
    getLastIndexedSeq() = 0;

    virtual std::vector<DEXCandle>
    getCandles(
        std::string const& bookKey,
        DEXInterval iv,
        uint32_t startTime,
        uint32_t endTime,
        uint32_t limit) = 0;

    virtual std::vector<DEXTrade>
    getTrades(
        std::string const& bookKey,
        uint32_t startTime,
        uint32_t endTime,
        uint32_t limit) = 0;

    virtual std::vector<DEXAMMSnapshot>
    getAMMHistory(
        std::string const& account,
        uint32_t startSeq,
        uint32_t endSeq,
        uint32_t limit) = 0;

    virtual std::optional<DEXTokenSummary>
    getTokenSummary(std::string const& bookKey) = 0;

    virtual std::vector<DEXTokenSummary>
    getPairs(std::string const& sort, uint32_t limit) = 0;

    virtual std::vector<DEXPoolInfo>
    getPools(std::string const& sort, uint32_t limit) = 0;

    virtual std::optional<DEXTokenInfo>
    getTokenInfo(std::string const& tokenKey) = 0;
};

std::unique_ptr<DEXTimeSeriesReader>
make_DEXTimeSeriesReader(DEXTimeSeriesStore* store, beast::Journal journal);

}  // namespace xrpl
