#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dex {

struct Tick
{
    std::string bookKey;
    uint32_t ledgerSeq = 0;
    uint32_t txIndex = 0;
    double rate = 0.0;
    double volumeA = 0.0;
    double volumeB = 0.0;
    uint32_t timestamp = 0;
};

struct Candle
{
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volumeA = 0.0;
    double volumeB = 0.0;
    uint32_t txCount = 0;
    uint32_t timestamp = 0;
};

struct AMMSnapshot
{
    std::string account;
    uint32_t ledgerSeq = 0;
    std::string asset1Balance;
    std::string asset2Balance;
    std::string lptBalance;
    uint16_t tradingFee = 0;
    uint32_t timestamp = 0;
};

enum class Interval : uint8_t
{
    OneMinute = 1,
    FiveMinute = 2,
    OneHour = 3,
    OneDay = 4,
};

inline uint32_t
intervalSeconds(Interval iv)
{
    switch (iv)
    {
        case Interval::OneMinute:
            return 60;
        case Interval::FiveMinute:
            return 300;
        case Interval::OneHour:
            return 3600;
        case Interval::OneDay:
            return 86400;
    }
    return 60;
}

inline uint32_t
bucketTimestamp(uint32_t ts, Interval iv)
{
    uint32_t const secs = intervalSeconds(iv);
    return (ts / secs) * secs;
}

}  // namespace dex
