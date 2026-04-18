#pragma once

#include "TimeSeriesStore.h"
#include "Types.h"

#include <map>
#include <mutex>

namespace dex {

class OHLCVAggregator
{
public:
    explicit OHLCVAggregator(TimeSeriesStore& store);

    void processTicks(
        uint32_t ledgerSeq,
        uint32_t ledgerTime,
        std::vector<Tick> const& ticks);

    void processAMMChanges(
        uint32_t ledgerSeq,
        uint32_t ledgerTime,
        std::vector<AMMSnapshot> const& changes);

    void flush();

private:
    TimeSeriesStore& store_;
    std::mutex mutex_;

    // In-flight 1-minute candle buckets: bookKey -> Candle
    std::map<std::string, Candle> currentCandles_;

    void mergeTick(std::string const& bookKey, Tick const& tick, uint32_t ledgerTime);
    void mergeIntoHigherInterval(std::string const& bookKey, Interval iv, Candle const& source);
    void flushBucket(std::string const& bookKey, Candle const& candle);
};

}  // namespace dex
