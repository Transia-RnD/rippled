#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>

#include <lmdb.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xrpl {

namespace dex {

inline void
encodeBE(uint32_t v, uint8_t* out)
{
    out[0] = (v >> 24) & 0xFF;
    out[1] = (v >> 16) & 0xFF;
    out[2] = (v >> 8) & 0xFF;
    out[3] = v & 0xFF;
}

inline uint32_t
decodeBE(uint8_t const* in)
{
    return (uint32_t(in[0]) << 24) | (uint32_t(in[1]) << 16) |
        (uint32_t(in[2]) << 8) | uint32_t(in[3]);
}

inline void
encodeDouble(double v, uint8_t* out)
{
    std::memcpy(out, &v, 8);
}

inline double
decodeDouble(uint8_t const* in)
{
    double v;
    std::memcpy(&v, in, 8);
    return v;
}

constexpr uint32_t
intervalSeconds(uint8_t iv)
{
    switch (iv)
    {
        case 0:
            return 60;
        case 1:
            return 300;
        case 2:
            return 3600;
        case 3:
            return 86400;
    }
    return 60;
}

constexpr uint32_t
bucketTimestamp(uint32_t ts, uint32_t ivSec)
{
    return (ts / ivSec) * ivSec;
}

struct TradeKey
{
    static constexpr size_t kFixedSuffix = 4 + 4 + 4;  // ts + seq + txIdx
    std::vector<uint8_t> data;

    TradeKey(
        std::string_view bookKey,
        uint32_t timestamp,
        uint32_t ledgerSeq,
        uint32_t txIndex)
    {
        data.resize(bookKey.size() + 1 + kFixedSuffix);
        std::memcpy(data.data(), bookKey.data(), bookKey.size());
        data[bookKey.size()] = '\0';
        size_t off = bookKey.size() + 1;
        encodeBE(timestamp, data.data() + off);
        encodeBE(ledgerSeq, data.data() + off + 4);
        encodeBE(txIndex, data.data() + off + 8);
    }

    static TradeKey
    prefix(std::string_view bookKey, uint32_t timestamp)
    {
        TradeKey k(bookKey, timestamp, 0, 0);
        return k;
    }

    MDB_val
    val()
    {
        return {data.size(), data.data()};
    }
};

struct CandleKey
{
    static constexpr size_t kFixedSuffix = 1 + 4;  // interval + bucket_ts
    std::vector<uint8_t> data;

    CandleKey(std::string_view bookKey, uint8_t interval, uint32_t bucketTs)
    {
        data.resize(bookKey.size() + 1 + kFixedSuffix);
        std::memcpy(data.data(), bookKey.data(), bookKey.size());
        data[bookKey.size()] = '\0';
        size_t off = bookKey.size() + 1;
        data[off] = interval;
        encodeBE(bucketTs, data.data() + off + 1);
    }

    MDB_val
    val()
    {
        return {data.size(), data.data()};
    }
};

struct CandleValue
{
    static constexpr size_t kSize = 68;
    uint8_t data[kSize];

    CandleValue() { std::memset(data, 0, kSize); }

    CandleValue(
        double open,
        double high,
        double low,
        double close,
        double volBase,
        double volQuote,
        uint32_t txCount,
        double buyVolBase,
        double sellVolBase)
    {
        encodeDouble(open, data);
        encodeDouble(high, data + 8);
        encodeDouble(low, data + 16);
        encodeDouble(close, data + 24);
        encodeDouble(volBase, data + 32);
        encodeDouble(volQuote, data + 40);
        encodeBE(txCount, data + 48);
        encodeDouble(buyVolBase, data + 52);
        encodeDouble(sellVolBase, data + 60);
    }

    static CandleValue
    fromData(void const* ptr)
    {
        CandleValue v;
        std::memcpy(v.data, ptr, kSize);
        return v;
    }

    double
    open() const
    {
        return decodeDouble(data);
    }
    double
    high() const
    {
        return decodeDouble(data + 8);
    }
    double
    low() const
    {
        return decodeDouble(data + 16);
    }
    double
    close() const
    {
        return decodeDouble(data + 24);
    }
    double
    volumeBase() const
    {
        return decodeDouble(data + 32);
    }
    double
    volumeQuote() const
    {
        return decodeDouble(data + 40);
    }
    uint32_t
    txCount() const
    {
        return decodeBE(data + 48);
    }
    double
    buyVolumeBase() const
    {
        return decodeDouble(data + 52);
    }
    double
    sellVolumeBase() const
    {
        return decodeDouble(data + 60);
    }

    MDB_val
    val()
    {
        return {kSize, data};
    }
};

struct TradeValue
{
    static constexpr size_t kFixed = 8 + 8 + 8 + 1 + 1;  // 26 bytes
    double rate;
    double volumeBase;
    double volumeQuote;
    uint8_t side;
    uint8_t source;
    std::string taker;
    std::string txHash;

    std::vector<uint8_t>
    encode() const
    {
        std::vector<uint8_t> buf(kFixed + taker.size() + 1 + txHash.size());
        encodeDouble(rate, buf.data());
        encodeDouble(volumeBase, buf.data() + 8);
        encodeDouble(volumeQuote, buf.data() + 16);
        buf[24] = side;
        buf[25] = source;
        std::memcpy(buf.data() + kFixed, taker.data(), taker.size());
        buf[kFixed + taker.size()] = '\0';
        std::memcpy(
            buf.data() + kFixed + taker.size() + 1,
            txHash.data(),
            txHash.size());
        return buf;
    }

    static TradeValue
    decode(void const* ptr, size_t len)
    {
        auto const* p = static_cast<uint8_t const*>(ptr);
        TradeValue v;
        v.rate = decodeDouble(p);
        v.volumeBase = decodeDouble(p + 8);
        v.volumeQuote = decodeDouble(p + 16);
        v.side = p[24];
        v.source = p[25];
        auto const* takerStart =
            reinterpret_cast<char const*>(p + kFixed);
        auto const* takerEnd = static_cast<char const*>(
            std::memchr(takerStart, '\0', len - kFixed));
        if (takerEnd)
        {
            v.taker.assign(takerStart, takerEnd);
            v.txHash.assign(
                takerEnd + 1,
                reinterpret_cast<char const*>(p) + len);
        }
        return v;
    }
};

struct AMMSnapshotKey
{
    static constexpr size_t kAcctSize = 35;
    static constexpr size_t kSize = kAcctSize + 4;
    uint8_t data[kSize];

    AMMSnapshotKey(std::string_view account, uint32_t ledgerSeq)
    {
        std::memset(data, 0, kSize);
        size_t copyLen = std::min(account.size(), size_t(kAcctSize));
        std::memcpy(data, account.data(), copyLen);
        encodeBE(ledgerSeq, data + kAcctSize);
    }

    static AMMSnapshotKey
    prefix(std::string_view account)
    {
        return AMMSnapshotKey(account, 0);
    }

    MDB_val
    val()
    {
        return {kSize, data};
    }
};

struct AMMSnapshotValue
{
    static constexpr size_t kSize = 8 + 8 + 8 + 2 + 1 + 8 + 8 + 8 + 4;  // 55
    uint8_t data[kSize];

    AMMSnapshotValue()
    {
        std::memset(data, 0, kSize);
    }

    AMMSnapshotValue(
        double a1Bal,
        double a2Bal,
        double lptBal,
        uint16_t tradingFee,
        uint8_t curveType,
        double tvlXrp,
        double vol24h,
        double fees24h,
        uint32_t timestamp)
    {
        encodeDouble(a1Bal, data);
        encodeDouble(a2Bal, data + 8);
        encodeDouble(lptBal, data + 16);
        data[24] = (tradingFee >> 8) & 0xFF;
        data[25] = tradingFee & 0xFF;
        data[26] = curveType;
        encodeDouble(tvlXrp, data + 27);
        encodeDouble(vol24h, data + 35);
        encodeDouble(fees24h, data + 43);
        encodeBE(timestamp, data + 51);
    }

    static AMMSnapshotValue
    fromData(void const* ptr)
    {
        AMMSnapshotValue v;
        std::memcpy(v.data, ptr, kSize);
        return v;
    }

    double
    asset1Balance() const
    {
        return decodeDouble(data);
    }
    double
    asset2Balance() const
    {
        return decodeDouble(data + 8);
    }
    double
    lptBalance() const
    {
        return decodeDouble(data + 16);
    }
    uint16_t
    tradingFee() const
    {
        return (uint16_t(data[24]) << 8) | data[25];
    }
    uint8_t
    curveType() const
    {
        return data[26];
    }
    double
    tvlXrp() const
    {
        return decodeDouble(data + 27);
    }
    double
    volume24hXrp() const
    {
        return decodeDouble(data + 35);
    }
    double
    fees24hXrp() const
    {
        return decodeDouble(data + 43);
    }
    uint32_t
    timestamp() const
    {
        return decodeBE(data + 51);
    }

    MDB_val
    val()
    {
        return {kSize, data};
    }
};

struct SummaryValue
{
    static constexpr size_t kSize = 92;
    uint8_t data[kSize];

    SummaryValue() { std::memset(data, 0, kSize); }

    double
    lastPrice() const
    {
        return decodeDouble(data);
    }
    double
    priceChange5m() const
    {
        return decodeDouble(data + 8);
    }
    double
    priceChange1h() const
    {
        return decodeDouble(data + 16);
    }
    double
    priceChange24h() const
    {
        return decodeDouble(data + 24);
    }
    double
    priceChange7d() const
    {
        return decodeDouble(data + 32);
    }
    double
    priceChange30d() const
    {
        return decodeDouble(data + 40);
    }
    double
    volume24hBase() const
    {
        return decodeDouble(data + 48);
    }
    double
    volume24hQuote() const
    {
        return decodeDouble(data + 56);
    }
    double
    high24h() const
    {
        return decodeDouble(data + 64);
    }
    double
    low24h() const
    {
        return decodeDouble(data + 72);
    }
    uint32_t
    tradeCount24h() const
    {
        return decodeBE(data + 80);
    }
    uint32_t
    updatedTs() const
    {
        return decodeBE(data + 84);
    }

    void
    set(double lastPrice,
        double pc5m,
        double pc1h,
        double pc24h,
        double pc7d,
        double pc30d,
        double vol24hBase,
        double vol24hQuote,
        double high24h,
        double low24h,
        uint32_t tradeCount,
        uint32_t ts)
    {
        encodeDouble(lastPrice, data);
        encodeDouble(pc5m, data + 8);
        encodeDouble(pc1h, data + 16);
        encodeDouble(pc24h, data + 24);
        encodeDouble(pc7d, data + 32);
        encodeDouble(pc30d, data + 40);
        encodeDouble(vol24hBase, data + 48);
        encodeDouble(vol24hQuote, data + 56);
        encodeDouble(high24h, data + 64);
        encodeDouble(low24h, data + 72);
        encodeBE(tradeCount, data + 80);
        encodeBE(ts, data + 84);
    }

    static SummaryValue
    fromData(void const* ptr)
    {
        SummaryValue v;
        std::memcpy(v.data, ptr, kSize);
        return v;
    }

    MDB_val
    val()
    {
        return {kSize, data};
    }
};

struct TokenInfoValue
{
    static constexpr size_t kSize = 36;
    uint8_t data[kSize];

    TokenInfoValue() { std::memset(data, 0, kSize); }

    double supply() const { return decodeDouble(data); }
    double frozenSupply() const { return decodeDouble(data + 8); }
    double lockedSupply() const { return decodeDouble(data + 16); }
    uint32_t holders() const { return decodeBE(data + 24); }
    uint32_t trustLines() const { return decodeBE(data + 28); }
    uint32_t ledgerSeq() const { return decodeBE(data + 32); }

    void
    set(double supply,
        double frozenSupply,
        double lockedSupply,
        uint32_t holders,
        uint32_t trustLines,
        uint32_t ledgerSeq)
    {
        encodeDouble(supply, data);
        encodeDouble(frozenSupply, data + 8);
        encodeDouble(lockedSupply, data + 16);
        encodeBE(holders, data + 24);
        encodeBE(trustLines, data + 28);
        encodeBE(ledgerSeq, data + 32);
    }

    static TokenInfoValue
    fromData(void const* ptr)
    {
        TokenInfoValue v;
        std::memcpy(v.data, ptr, kSize);
        return v;
    }

    MDB_val
    val()
    {
        return {kSize, data};
    }
};

}  // namespace dex

enum class DexDB : int
{
    Trades = 0,
    Candles = 1,
    AMMSnapshots = 2,
    AMMPools = 3,
    Summaries = 4,
    Meta = 5,
    TokenInfo = 6,
    Count = 7
};

class DEXTimeSeriesStore
{
    MDB_env* env_ = nullptr;
    std::array<MDB_dbi, static_cast<int>(DexDB::Count)> dbis_{};
    beast::Journal journal_;
    bool open_ = false;

public:
    DEXTimeSeriesStore(beast::Journal journal) : journal_(journal) {}

    ~DEXTimeSeriesStore()
    {
        close();
    }

    DEXTimeSeriesStore(DEXTimeSeriesStore const&) = delete;
    DEXTimeSeriesStore& operator=(DEXTimeSeriesStore const&) = delete;

    bool
    open(std::string const& path, uint64_t mapSizeGB)
    {
        if (open_)
            return true;

        int rc = mdb_env_create(&env_);
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXStore: mdb_env_create failed: " << mdb_strerror(rc);
            return false;
        }

        rc = mdb_env_set_maxdbs(env_, static_cast<int>(DexDB::Count));
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXStore: set_maxdbs failed: " << mdb_strerror(rc);
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        uint64_t const mapSize = mapSizeGB * 1024ULL * 1024ULL * 1024ULL;
        mdb_env_set_mapsize(env_, mapSize);

        rc = mdb_env_open(env_, path.c_str(), MDB_NOSYNC | MDB_WRITEMAP, 0664);
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXStore: mdb_env_open failed: " << mdb_strerror(rc);
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        MDB_txn* txn = nullptr;
        rc = mdb_txn_begin(env_, nullptr, 0, &txn);
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXStore: txn_begin failed: " << mdb_strerror(rc);
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        static constexpr char const* dbNames[] = {
            "DexTrades",
            "DexCandles",
            "DexAMMSnapshots",
            "DexAMMPools",
            "DexSummaries",
            "DexMeta",
            "DexTokenInfo"};

        for (int i = 0; i < static_cast<int>(DexDB::Count); ++i)
        {
            rc = mdb_dbi_open(txn, dbNames[i], MDB_CREATE, &dbis_[i]);
            if (rc != 0)
            {
                JLOG(journal_.error())
                    << "DEXStore: dbi_open failed for " << dbNames[i] << ": "
                    << mdb_strerror(rc);
                mdb_txn_abort(txn);
                mdb_env_close(env_);
                env_ = nullptr;
                return false;
            }
        }

        rc = mdb_txn_commit(txn);
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXStore: txn_commit failed: " << mdb_strerror(rc);
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }

        open_ = true;
        JLOG(journal_.info()) << "DEXStore: opened at " << path;
        return true;
    }

    void
    close()
    {
        if (env_)
        {
            JLOG(journal_.info()) << "DEXStore: closing database";
            mdb_env_close(env_);
            env_ = nullptr;
            open_ = false;
        }
    }

    bool
    isOpen() const
    {
        return open_;
    }

    MDB_env*
    env()
    {
        return env_;
    }

    MDB_dbi
    dbi(DexDB db) const
    {
        return dbis_[static_cast<int>(db)];
    }

    int
    beginTxn(MDB_txn** txn, unsigned int flags = 0)
    {
        int rc = mdb_txn_begin(env_, nullptr, flags, txn);
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXStore: beginTxn failed: " << mdb_strerror(rc);
        }
        return rc;
    }

    int
    put(MDB_txn* txn, DexDB db, MDB_val* key, MDB_val* data, unsigned int flags = 0)
    {
        int rc = mdb_put(txn, dbi(db), key, data, flags);
        if (rc != 0)
        {
            JLOG(journal_.warn())
                << "DEXStore: put failed on db " << static_cast<int>(db)
                << ": " << mdb_strerror(rc);
        }
        return rc;
    }

    int
    get(MDB_txn* txn, DexDB db, MDB_val* key, MDB_val* data)
    {
        return mdb_get(txn, dbi(db), key, data);
    }

    int
    del(MDB_txn* txn, DexDB db, MDB_val* key)
    {
        int rc = mdb_del(txn, dbi(db), key, nullptr);
        if (rc != 0 && rc != MDB_NOTFOUND)
        {
            JLOG(journal_.warn())
                << "DEXStore: del failed on db " << static_cast<int>(db)
                << ": " << mdb_strerror(rc);
        }
        return rc;
    }

    std::optional<std::string>
    getMeta(std::string const& key)
    {
        MDB_txn* txn = nullptr;
        if (beginTxn(&txn, MDB_RDONLY) != 0)
        {
            JLOG(journal_.warn())
                << "DEXStore: getMeta failed to begin txn for key=" << key;
            return std::nullopt;
        }

        MDB_val k = {key.size(), const_cast<char*>(key.data())};
        MDB_val v;
        int rc = mdb_get(txn, dbi(DexDB::Meta), &k, &v);
        std::optional<std::string> result;
        if (rc == 0)
            result = std::string(static_cast<char*>(v.mv_data), v.mv_size);
        else if (rc != MDB_NOTFOUND)
        {
            JLOG(journal_.warn())
                << "DEXStore: getMeta error for key=" << key
                << ": " << mdb_strerror(rc);
        }
        mdb_txn_abort(txn);
        return result;
    }

    bool
    setMeta(MDB_txn* txn, std::string const& key, std::string const& value)
    {
        MDB_val k = {key.size(), const_cast<char*>(key.data())};
        MDB_val v = {value.size(), const_cast<char*>(value.data())};
        int rc = mdb_put(txn, dbi(DexDB::Meta), &k, &v, 0);
        if (rc != 0)
        {
            JLOG(journal_.warn())
                << "DEXStore: setMeta failed for key=" << key
                << ": " << mdb_strerror(rc);
        }
        return rc == 0;
    }
};

}  // namespace xrpl
