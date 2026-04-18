#include "TimeSeriesStore.h"

#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace dex {

namespace {

void
putU32BE(std::string& out, uint32_t v)
{
    char buf[4];
    buf[0] = static_cast<char>((v >> 24) & 0xFF);
    buf[1] = static_cast<char>((v >> 16) & 0xFF);
    buf[2] = static_cast<char>((v >> 8) & 0xFF);
    buf[3] = static_cast<char>(v & 0xFF);
    out.append(buf, 4);
}

uint32_t
getU32BE(char const* p)
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
        static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

void
putF64(std::string& out, double v)
{
    char buf[8];
    std::memcpy(buf, &v, 8);
    out.append(buf, 8);
}

double
getF64(char const* p)
{
    double v;
    std::memcpy(&v, p, 8);
    return v;
}

void
putU16BE(std::string& out, uint16_t v)
{
    char buf[2];
    buf[0] = static_cast<char>((v >> 8) & 0xFF);
    buf[1] = static_cast<char>(v & 0xFF);
    out.append(buf, 2);
}

uint16_t
getU16BE(char const* p)
{
    return (static_cast<uint16_t>(static_cast<uint8_t>(p[0])) << 8) |
        static_cast<uint16_t>(static_cast<uint8_t>(p[1]));
}

rocksdb::ColumnFamilyOptions
fifoOptions(uint64_t maxBytes)
{
    rocksdb::ColumnFamilyOptions opts;
    opts.compaction_style = rocksdb::kCompactionStyleFIFO;
    opts.compaction_options_fifo.max_table_files_size = maxBytes;
    return opts;
}

}  // namespace

TimeSeriesStore::TimeSeriesStore(StoreConfig const& config)
    : config_(config)
{
}

TimeSeriesStore::~TimeSeriesStore()
{
    close();
}

bool
TimeSeriesStore::open()
{
    rocksdb::DBOptions dbOpts;
    dbOpts.create_if_missing = true;
    dbOpts.create_missing_column_families = true;

    std::vector<rocksdb::ColumnFamilyDescriptor> cfDescs;
    cfDescs.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
    cfDescs.emplace_back("ticks", fifoOptions(config_.ticksMaxBytes));
    cfDescs.emplace_back("candles_1m", fifoOptions(config_.candles1mMaxBytes));
    cfDescs.emplace_back("candles_5m", fifoOptions(config_.candles5mMaxBytes));
    cfDescs.emplace_back("candles_1h", fifoOptions(config_.candles1hMaxBytes));
    cfDescs.emplace_back("candles_1d", fifoOptions(config_.candles1dMaxBytes));
    cfDescs.emplace_back("amm_state", fifoOptions(config_.ammStateMaxBytes));
    cfDescs.emplace_back("meta", rocksdb::ColumnFamilyOptions());

    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    rocksdb::DB* rawDb = nullptr;
    auto status = rocksdb::DB::Open(dbOpts, config_.dbPath, cfDescs, &handles, &rawDb);
    if (!status.ok())
    {
        std::cerr << "Failed to open RocksDB: " << status.ToString() << "\n";
        return false;
    }

    db_.reset(rawDb);

    cfDefault_ = handles[0];
    cfTicks_ = handles[1];
    cfCandles1m_ = handles[2];
    cfCandles5m_ = handles[3];
    cfCandles1h_ = handles[4];
    cfCandles1d_ = handles[5];
    cfAMMState_ = handles[6];
    cfMeta_ = handles[7];

    return true;
}

void
TimeSeriesStore::close()
{
    if (!db_)
        return;

    auto handles = {cfDefault_, cfTicks_, cfCandles1m_, cfCandles5m_, cfCandles1h_,
                    cfCandles1d_, cfAMMState_, cfMeta_};
    for (auto* h : handles)
    {
        if (h)
            db_->DestroyColumnFamilyHandle(h);
    }

    cfDefault_ = cfTicks_ = cfCandles1m_ = cfCandles5m_ = cfCandles1h_ = nullptr;
    cfCandles1d_ = cfAMMState_ = cfMeta_ = nullptr;
    db_.reset();
}

rocksdb::ColumnFamilyHandle*
TimeSeriesStore::cfForInterval(Interval iv) const
{
    switch (iv)
    {
        case Interval::OneMinute:
            return cfCandles1m_;
        case Interval::FiveMinute:
            return cfCandles5m_;
        case Interval::OneHour:
            return cfCandles1h_;
        case Interval::OneDay:
            return cfCandles1d_;
    }
    return cfCandles1m_;
}

// --- Key encoding ---

std::string
TimeSeriesStore::tickKey(std::string const& bookKey, uint32_t seq, uint32_t txIdx) const
{
    std::string k;
    k.reserve(bookKey.size() + 1 + 4 + 2);
    k.append(bookKey);
    k.push_back('\0');
    putU32BE(k, seq);
    putU16BE(k, static_cast<uint16_t>(txIdx));
    return k;
}

std::string
TimeSeriesStore::candleKey(std::string const& bookKey, uint32_t bucketTs) const
{
    std::string k;
    k.reserve(bookKey.size() + 1 + 4);
    k.append(bookKey);
    k.push_back('\0');
    putU32BE(k, bucketTs);
    return k;
}

std::string
TimeSeriesStore::ammKey(std::string const& account, uint32_t seq) const
{
    std::string k;
    k.reserve(account.size() + 1 + 4);
    k.append(account);
    k.push_back('\0');
    putU32BE(k, seq);
    return k;
}

// --- Value encoding ---

void
TimeSeriesStore::encodeTick(Tick const& t, std::string& out) const
{
    out.clear();
    out.reserve(28);
    putF64(out, t.rate);
    putF64(out, t.volumeA);
    putF64(out, t.volumeB);
    putU32BE(out, t.timestamp);
}

Tick
TimeSeriesStore::decodeTick(std::string const& key, std::string_view val) const
{
    Tick t;
    auto sep = key.find('\0');
    t.bookKey = key.substr(0, sep);
    t.ledgerSeq = getU32BE(key.data() + sep + 1);
    t.txIndex = getU16BE(key.data() + sep + 5);
    t.rate = getF64(val.data());
    t.volumeA = getF64(val.data() + 8);
    t.volumeB = getF64(val.data() + 16);
    t.timestamp = getU32BE(val.data() + 24);
    return t;
}

void
TimeSeriesStore::encodeCandle(Candle const& c, std::string& out) const
{
    out.clear();
    out.reserve(52);
    putF64(out, c.open);
    putF64(out, c.high);
    putF64(out, c.low);
    putF64(out, c.close);
    putF64(out, c.volumeA);
    putF64(out, c.volumeB);
    putU32BE(out, c.txCount);
}

Candle
TimeSeriesStore::decodeCandle(std::string_view val) const
{
    Candle c;
    c.open = getF64(val.data());
    c.high = getF64(val.data() + 8);
    c.low = getF64(val.data() + 16);
    c.close = getF64(val.data() + 24);
    c.volumeA = getF64(val.data() + 32);
    c.volumeB = getF64(val.data() + 40);
    c.txCount = getU32BE(val.data() + 48);
    return c;
}

void
TimeSeriesStore::encodeAMMSnapshot(AMMSnapshot const& s, std::string& out) const
{
    out.clear();
    // Variable-length: write lengths then data
    putU16BE(out, static_cast<uint16_t>(s.asset1Balance.size()));
    out.append(s.asset1Balance);
    putU16BE(out, static_cast<uint16_t>(s.asset2Balance.size()));
    out.append(s.asset2Balance);
    putU16BE(out, static_cast<uint16_t>(s.lptBalance.size()));
    out.append(s.lptBalance);
    putU16BE(out, s.tradingFee);
    putU32BE(out, s.timestamp);
}

AMMSnapshot
TimeSeriesStore::decodeAMMSnapshot(std::string const& key, std::string_view val) const
{
    AMMSnapshot s;
    auto sep = key.find('\0');
    s.account = key.substr(0, sep);
    s.ledgerSeq = getU32BE(key.data() + sep + 1);

    size_t pos = 0;
    auto readStr = [&]() -> std::string {
        uint16_t len = getU16BE(val.data() + pos);
        pos += 2;
        std::string result(val.data() + pos, len);
        pos += len;
        return result;
    };

    s.asset1Balance = readStr();
    s.asset2Balance = readStr();
    s.lptBalance = readStr();
    s.tradingFee = getU16BE(val.data() + pos);
    pos += 2;
    s.timestamp = getU32BE(val.data() + pos);
    return s;
}

// --- Write operations ---

void
TimeSeriesStore::writeTick(Tick const& tick)
{
    std::string key = tickKey(tick.bookKey, tick.ledgerSeq, tick.txIndex);
    std::string val;
    encodeTick(tick, val);
    db_->Put(rocksdb::WriteOptions(), cfTicks_, key, val);
}

void
TimeSeriesStore::writeCandle(
    std::string const& bookKey,
    Interval iv,
    Candle const& candle)
{
    uint32_t const bucket = bucketTimestamp(candle.timestamp, iv);
    std::string key = candleKey(bookKey, bucket);
    std::string val;
    encodeCandle(candle, val);
    db_->Put(rocksdb::WriteOptions(), cfForInterval(iv), key, val);
}

void
TimeSeriesStore::writeAMMSnapshot(AMMSnapshot const& snap)
{
    std::string key = ammKey(snap.account, snap.ledgerSeq);
    std::string val;
    encodeAMMSnapshot(snap, val);
    db_->Put(rocksdb::WriteOptions(), cfAMMState_, key, val);
}

void
TimeSeriesStore::setLastIndexedSeq(uint32_t seq)
{
    std::string val;
    putU32BE(val, seq);
    db_->Put(rocksdb::WriteOptions(), cfMeta_, "last_indexed_seq", val);
}

std::optional<uint32_t>
TimeSeriesStore::getLastIndexedSeq()
{
    std::string val;
    auto s = db_->Get(rocksdb::ReadOptions(), cfMeta_, "last_indexed_seq", &val);
    if (!s.ok() || val.size() < 4)
        return std::nullopt;
    return getU32BE(val.data());
}

// --- Read operations ---

std::vector<Candle>
TimeSeriesStore::getCandles(
    std::string const& bookKey,
    Interval iv,
    uint32_t startTime,
    uint32_t endTime,
    uint32_t limit)
{
    std::vector<Candle> result;
    std::string const startK = candleKey(bookKey, startTime);
    std::string const endK = candleKey(bookKey, endTime);
    std::string const prefix = bookKey + '\0';

    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(rocksdb::ReadOptions(), cfForInterval(iv)));

    it->Seek(startK);
    for (; it->Valid() && result.size() < limit; it->Next())
    {
        auto key = it->key().ToString();
        if (key.compare(0, prefix.size(), prefix) != 0)
            break;
        if (key > endK)
            break;

        Candle c = decodeCandle(it->value().ToStringView());
        auto sep = key.find('\0');
        c.timestamp = getU32BE(key.data() + sep + 1);
        result.push_back(c);
    }

    return result;
}

std::vector<Tick>
TimeSeriesStore::getTicks(
    std::string const& bookKey,
    uint32_t startSeq,
    uint32_t endSeq,
    uint32_t limit)
{
    std::vector<Tick> result;
    std::string const startK = tickKey(bookKey, startSeq, 0);
    std::string const endK = tickKey(bookKey, endSeq, 0xFFFF);
    std::string const prefix = bookKey + '\0';

    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(rocksdb::ReadOptions(), cfTicks_));

    it->Seek(startK);
    for (; it->Valid() && result.size() < limit; it->Next())
    {
        auto key = it->key().ToString();
        if (key.compare(0, prefix.size(), prefix) != 0)
            break;
        if (key > endK)
            break;

        result.push_back(decodeTick(key, it->value().ToStringView()));
    }

    return result;
}

std::vector<AMMSnapshot>
TimeSeriesStore::getAMMHistory(
    std::string const& account,
    uint32_t startSeq,
    uint32_t endSeq,
    uint32_t limit)
{
    std::vector<AMMSnapshot> result;
    std::string const startK = ammKey(account, startSeq);
    std::string const endK = ammKey(account, endSeq);
    std::string const prefix = account + '\0';

    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(rocksdb::ReadOptions(), cfAMMState_));

    it->Seek(startK);
    for (; it->Valid() && result.size() < limit; it->Next())
    {
        auto key = it->key().ToString();
        if (key.compare(0, prefix.size(), prefix) != 0)
            break;
        if (key > endK)
            break;

        result.push_back(decodeAMMSnapshot(key, it->value().ToStringView()));
    }

    return result;
}

}  // namespace dex
