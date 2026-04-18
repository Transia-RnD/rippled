#include "OHLCVAggregator.h"

#include <iostream>

namespace dex {

OHLCVAggregator::OHLCVAggregator(TimeSeriesStore& store)
    : store_(store)
{
}

void
OHLCVAggregator::processTicks(
    uint32_t ledgerSeq,
    uint32_t ledgerTime,
    std::vector<Tick> const& ticks)
{
    std::lock_guard const lock(mutex_);

    for (auto const& tick : ticks)
    {
        store_.writeTick(tick);
        mergeTick(tick.bookKey, tick, ledgerTime);
    }

    store_.setLastIndexedSeq(ledgerSeq);
}

void
OHLCVAggregator::processAMMChanges(
    uint32_t /* ledgerSeq */,
    uint32_t /* ledgerTime */,
    std::vector<AMMSnapshot> const& changes)
{
    std::lock_guard const lock(mutex_);

    for (auto const& snap : changes)
    {
        store_.writeAMMSnapshot(snap);
    }
}

void
OHLCVAggregator::mergeTick(
    std::string const& bookKey,
    Tick const& tick,
    uint32_t ledgerTime)
{
    uint32_t const bucket = bucketTimestamp(ledgerTime, Interval::OneMinute);
    std::string const key = bookKey + ":" + std::to_string(bucket);

    auto it = currentCandles_.find(key);
    if (it == currentCandles_.end())
    {
        // New bucket — flush previous bucket for this book if it exists
        std::string const prevBucket = bookKey + ":" +
            std::to_string(bucket - intervalSeconds(Interval::OneMinute));
        auto prev = currentCandles_.find(prevBucket);
        if (prev != currentCandles_.end())
        {
            flushBucket(bookKey, prev->second);
            currentCandles_.erase(prev);
        }

        Candle c;
        c.open = tick.rate;
        c.high = tick.rate;
        c.low = tick.rate;
        c.close = tick.rate;
        c.volumeA = tick.volumeA;
        c.volumeB = tick.volumeB;
        c.txCount = 1;
        c.timestamp = ledgerTime;
        currentCandles_[key] = c;
    }
    else
    {
        auto& c = it->second;
        if (tick.rate > c.high)
            c.high = tick.rate;
        if (tick.rate < c.low)
            c.low = tick.rate;
        c.close = tick.rate;
        c.volumeA += tick.volumeA;
        c.volumeB += tick.volumeB;
        c.txCount++;
    }
}

void
OHLCVAggregator::mergeIntoHigherInterval(
    std::string const& bookKey,
    Interval iv,
    Candle const& source)
{
    uint32_t const bucket = bucketTimestamp(source.timestamp, iv);

    // Read existing candle from RocksDB for this bucket
    auto existing = store_.getCandles(bookKey, iv, bucket, bucket, 1);
    if (!existing.empty())
    {
        // Merge: keep existing open, update high/low/close, sum volumes
        Candle merged = existing[0];
        if (source.high > merged.high)
            merged.high = source.high;
        if (source.low < merged.low)
            merged.low = source.low;
        merged.close = source.close;
        merged.volumeA += source.volumeA;
        merged.volumeB += source.volumeB;
        merged.txCount += source.txCount;
        merged.timestamp = source.timestamp;
        store_.writeCandle(bookKey, iv, merged);
    }
    else
    {
        // First 1m candle in this higher-interval bucket
        Candle c = source;
        c.timestamp = source.timestamp;
        store_.writeCandle(bookKey, iv, c);
    }
}

void
OHLCVAggregator::flushBucket(std::string const& bookKey, Candle const& candle)
{
    store_.writeCandle(bookKey, Interval::OneMinute, candle);

    // Always merge into all higher intervals
    mergeIntoHigherInterval(bookKey, Interval::FiveMinute, candle);
    mergeIntoHigherInterval(bookKey, Interval::OneHour, candle);
    mergeIntoHigherInterval(bookKey, Interval::OneDay, candle);
}

void
OHLCVAggregator::flush()
{
    std::lock_guard const lock(mutex_);

    for (auto const& [key, candle] : currentCandles_)
    {
        auto sep = key.find(':');
        if (sep != std::string::npos)
        {
            std::string bookKey = key.substr(0, sep);
            flushBucket(bookKey, candle);
        }
    }
    currentCandles_.clear();
}

}  // namespace dex
