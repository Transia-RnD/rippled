#pragma once

#include "Types.h"

#include <rocksdb/db.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dex {

struct StoreConfig
{
    std::string dbPath = "./dex_timeseries";
    uint64_t ticksMaxBytes = 512 * 1024 * 1024;      // 512MB
    uint64_t candles1mMaxBytes = 256 * 1024 * 1024;   // 256MB
    uint64_t candles5mMaxBytes = 256 * 1024 * 1024;
    uint64_t candles1hMaxBytes = 512 * 1024 * 1024;
    uint64_t candles1dMaxBytes = 1024 * 1024 * 1024;  // 1GB
    uint64_t ammStateMaxBytes = 256 * 1024 * 1024;
};

class TimeSeriesStore
{
public:
    explicit TimeSeriesStore(StoreConfig const& config);
    ~TimeSeriesStore();

    TimeSeriesStore(TimeSeriesStore const&) = delete;
    TimeSeriesStore& operator=(TimeSeriesStore const&) = delete;

    bool open();
    void close();

    void writeTick(Tick const& tick);
    void writeCandle(std::string const& bookKey, Interval iv, Candle const& candle);
    void writeAMMSnapshot(AMMSnapshot const& snap);

    void setLastIndexedSeq(uint32_t seq);
    std::optional<uint32_t> getLastIndexedSeq();

    std::vector<Candle>
    getCandles(
        std::string const& bookKey,
        Interval iv,
        uint32_t startTime,
        uint32_t endTime,
        uint32_t limit = 1000);

    std::vector<Tick>
    getTicks(
        std::string const& bookKey,
        uint32_t startSeq,
        uint32_t endSeq,
        uint32_t limit = 1000);

    std::vector<AMMSnapshot>
    getAMMHistory(
        std::string const& account,
        uint32_t startSeq,
        uint32_t endSeq,
        uint32_t limit = 1000);

private:
    StoreConfig config_;
    std::unique_ptr<rocksdb::DB> db_;

    rocksdb::ColumnFamilyHandle* cfDefault_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfTicks_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfCandles1m_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfCandles5m_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfCandles1h_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfCandles1d_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfAMMState_ = nullptr;
    rocksdb::ColumnFamilyHandle* cfMeta_ = nullptr;

    rocksdb::ColumnFamilyHandle* cfForInterval(Interval iv) const;

    std::string tickKey(std::string const& bookKey, uint32_t seq, uint32_t txIdx) const;
    std::string candleKey(std::string const& bookKey, uint32_t bucketTs) const;
    std::string ammKey(std::string const& account, uint32_t seq) const;

    void encodeTick(Tick const& t, std::string& out) const;
    Tick decodeTick(std::string const& key, std::string_view val) const;

    void encodeCandle(Candle const& c, std::string& out) const;
    Candle decodeCandle(std::string_view val) const;

    void encodeAMMSnapshot(AMMSnapshot const& s, std::string& out) const;
    AMMSnapshot decodeAMMSnapshot(std::string const& key, std::string_view val) const;
};

}  // namespace dex
