#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/app/misc/DEXTimeSeriesStore.h>

#include <xrpl/basics/Log.h>

#include <lmdb.h>

#include <algorithm>
#include <cstring>

namespace xrpl {

namespace {

constexpr uint32_t kMaxLimit = 10'000;

using namespace dex;

class DEXTimeSeriesReaderImpl final : public DEXTimeSeriesReader
{
    DEXTimeSeriesStore* store_;
    [[maybe_unused]] beast::Journal journal_;

public:
    DEXTimeSeriesReaderImpl(DEXTimeSeriesStore* store, beast::Journal journal)
        : store_(store), journal_(journal)
    {
    }

    std::optional<uint32_t>
    getLastIndexedSeq() override
    {
        if (!store_ || !store_->isOpen())
            return std::nullopt;

        auto val = store_->getMeta("last_indexed_seq");
        if (!val || val->empty())
            return std::nullopt;
        return static_cast<uint32_t>(std::stoul(*val));
    }

    std::vector<DEXCandle>
    getCandles(
        std::string const& bookKey,
        DEXInterval iv,
        uint32_t startTime,
        uint32_t endTime,
        uint32_t limit) override
    {
        std::vector<DEXCandle> result;
        if (!store_ || !store_->isOpen())
            return result;

        limit = std::min(limit, kMaxLimit);
        if (limit == 0)
            return result;

        uint8_t const interval = static_cast<uint8_t>(iv);

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn, MDB_RDONLY) != 0)
            return result;

        MDB_cursor* cursor = nullptr;
        if (mdb_cursor_open(txn, store_->dbi(DexDB::Candles), &cursor) != 0)
        {
            mdb_txn_abort(txn);
            return result;
        }

        CandleKey startKey(bookKey, interval, startTime);
        auto sk = startKey.val();
        MDB_val key = sk;
        MDB_val data;

        int rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
        while (rc == 0 && result.size() < limit)
        {
            if (key.mv_size < bookKey.size() + 1 + CandleKey::kFixedSuffix)
                break;

            auto const* p = static_cast<uint8_t const*>(key.mv_data);

            if (std::memcmp(p, bookKey.data(), bookKey.size()) != 0 ||
                p[bookKey.size()] != '\0')
                break;

            size_t off = bookKey.size() + 1;
            uint8_t keyIv = p[off];
            uint32_t bucketTs = decodeBE(p + off + 1);

            if (keyIv != interval)
                break;

            if (bucketTs > endTime)
                break;

            if (data.mv_size == CandleValue::kSize)
            {
                auto cv = CandleValue::fromData(data.mv_data);
                DEXCandle c;
                c.open = cv.open();
                c.high = cv.high();
                c.low = cv.low();
                c.close = cv.close();
                c.volumeBase = cv.volumeBase();
                c.volumeQuote = cv.volumeQuote();
                c.txCount = cv.txCount();
                c.timestamp = bucketTs;
                c.buyVolumeBase = cv.buyVolumeBase();
                c.sellVolumeBase = cv.sellVolumeBase();
                result.push_back(c);
            }

            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return result;
    }

    std::vector<DEXTrade>
    getTrades(
        std::string const& bookKey,
        uint32_t startTime,
        uint32_t endTime,
        uint32_t limit) override
    {
        std::vector<DEXTrade> result;
        if (!store_ || !store_->isOpen())
            return result;

        limit = std::min(limit, kMaxLimit);
        if (limit == 0)
            return result;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn, MDB_RDONLY) != 0)
            return result;

        MDB_cursor* cursor = nullptr;
        if (mdb_cursor_open(txn, store_->dbi(DexDB::Trades), &cursor) != 0)
        {
            mdb_txn_abort(txn);
            return result;
        }

        TradeKey startKey = TradeKey::prefix(bookKey, startTime);
        auto sk = startKey.val();
        MDB_val key = sk;
        MDB_val data;

        int rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
        while (rc == 0 && result.size() < limit)
        {
            if (key.mv_size < bookKey.size() + 1 + TradeKey::kFixedSuffix)
                break;

            auto const* p = static_cast<uint8_t const*>(key.mv_data);

            if (std::memcmp(p, bookKey.data(), bookKey.size()) != 0 ||
                p[bookKey.size()] != '\0')
                break;

            size_t off = bookKey.size() + 1;
            uint32_t timestamp = decodeBE(p + off);
            uint32_t ledgerSeq = decodeBE(p + off + 4);
            uint32_t txIndex = decodeBE(p + off + 8);

            if (timestamp > endTime)
                break;

            auto tv = TradeValue::decode(data.mv_data, data.mv_size);
            DEXTrade t;
            t.bookKey = bookKey;
            t.ledgerSeq = ledgerSeq;
            t.txIndex = txIndex;
            t.timestamp = timestamp;
            t.rate = tv.rate;
            t.volumeBase = tv.volumeBase;
            t.volumeQuote = tv.volumeQuote;
            t.side = tv.side;
            t.source = tv.source;
            t.taker = tv.taker;
            t.txHash = tv.txHash;
            result.push_back(std::move(t));

            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return result;
    }

    std::vector<DEXAMMSnapshot>
    getAMMHistory(
        std::string const& account,
        uint32_t startSeq,
        uint32_t endSeq,
        uint32_t limit) override
    {
        std::vector<DEXAMMSnapshot> result;
        if (!store_ || !store_->isOpen())
            return result;

        limit = std::min(limit, kMaxLimit);
        if (limit == 0)
            return result;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn, MDB_RDONLY) != 0)
            return result;

        MDB_cursor* cursor = nullptr;
        if (mdb_cursor_open(
                txn, store_->dbi(DexDB::AMMSnapshots), &cursor) != 0)
        {
            mdb_txn_abort(txn);
            return result;
        }

        AMMSnapshotKey startKey(account, startSeq);
        auto sk = startKey.val();
        MDB_val key = sk;
        MDB_val data;

        int rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
        while (rc == 0 && result.size() < limit)
        {
            if (key.mv_size != AMMSnapshotKey::kSize)
                break;

            auto const* p = static_cast<uint8_t const*>(key.mv_data);

            std::string keyAccount(
                reinterpret_cast<char const*>(p),
                strnlen(reinterpret_cast<char const*>(p),
                        AMMSnapshotKey::kAcctSize));

            if (keyAccount != account)
                break;

            uint32_t ledgerSeq = decodeBE(p + AMMSnapshotKey::kAcctSize);

            if (ledgerSeq > endSeq)
                break;

            if (data.mv_size == AMMSnapshotValue::kSize)
            {
                auto sv = AMMSnapshotValue::fromData(data.mv_data);
                DEXAMMSnapshot s;
                s.account = account;
                s.ledgerSeq = ledgerSeq;
                s.timestamp = sv.timestamp();
                s.asset1Balance = sv.asset1Balance();
                s.asset2Balance = sv.asset2Balance();
                s.lptBalance = sv.lptBalance();
                s.tradingFee = sv.tradingFee();
                s.curveType = sv.curveType();
                s.tvlXrp = sv.tvlXrp();
                s.volume24hXrp = sv.volume24hXrp();
                s.fees24hXrp = sv.fees24hXrp();
                result.push_back(s);
            }

            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return result;
    }

    std::optional<DEXTokenSummary>
    getTokenSummary(std::string const& bookKey) override
    {
        if (!store_ || !store_->isOpen())
            return std::nullopt;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn, MDB_RDONLY) != 0)
            return std::nullopt;

        MDB_val key = {
            bookKey.size(), const_cast<char*>(bookKey.data())};
        MDB_val data;

        int rc = store_->get(txn, DexDB::Summaries, &key, &data);
        std::optional<DEXTokenSummary> result;

        if (rc == 0 && data.mv_size == SummaryValue::kSize)
        {
            auto sv = SummaryValue::fromData(data.mv_data);
            DEXTokenSummary ts;
            ts.bookKey = bookKey;
            ts.lastPrice = sv.lastPrice();
            ts.priceChange5m = sv.priceChange5m();
            ts.priceChange1h = sv.priceChange1h();
            ts.priceChange24h = sv.priceChange24h();
            ts.priceChange7d = sv.priceChange7d();
            ts.priceChange30d = sv.priceChange30d();
            ts.volume24hBase = sv.volume24hBase();
            ts.volume24hQuote = sv.volume24hQuote();
            ts.high24h = sv.high24h();
            ts.low24h = sv.low24h();
            ts.tradeCount24h = sv.tradeCount24h();
            result = ts;
        }

        mdb_txn_abort(txn);
        return result;
    }

    std::vector<DEXTokenSummary>
    getPairs(std::string const& sort, uint32_t limit) override
    {
        std::vector<DEXTokenSummary> result;
        if (!store_ || !store_->isOpen())
            return result;

        limit = std::min(limit, kMaxLimit);
        if (limit == 0)
            return result;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn, MDB_RDONLY) != 0)
            return result;

        MDB_cursor* cursor = nullptr;
        if (mdb_cursor_open(txn, store_->dbi(DexDB::Summaries), &cursor) != 0)
        {
            mdb_txn_abort(txn);
            return result;
        }

        MDB_val key, data;
        int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
        while (rc == 0)
        {
            if (data.mv_size == SummaryValue::kSize)
            {
                auto sv = SummaryValue::fromData(data.mv_data);
                DEXTokenSummary ts;
                ts.bookKey = std::string(
                    static_cast<char const*>(key.mv_data), key.mv_size);
                ts.lastPrice = sv.lastPrice();
                ts.priceChange5m = sv.priceChange5m();
                ts.priceChange1h = sv.priceChange1h();
                ts.priceChange24h = sv.priceChange24h();
                ts.priceChange7d = sv.priceChange7d();
                ts.priceChange30d = sv.priceChange30d();
                ts.volume24hBase = sv.volume24hBase();
                ts.volume24hQuote = sv.volume24hQuote();
                ts.high24h = sv.high24h();
                ts.low24h = sv.low24h();
                ts.tradeCount24h = sv.tradeCount24h();
                result.push_back(std::move(ts));
            }
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);

        if (sort == "volume")
        {
            std::sort(result.begin(), result.end(), [](auto& a, auto& b) {
                return a.volume24hQuote > b.volume24hQuote;
            });
        }
        else if (sort == "trades")
        {
            std::sort(result.begin(), result.end(), [](auto& a, auto& b) {
                return a.tradeCount24h > b.tradeCount24h;
            });
        }
        else if (sort == "change")
        {
            std::sort(result.begin(), result.end(), [](auto& a, auto& b) {
                return a.priceChange24h > b.priceChange24h;
            });
        }

        if (result.size() > limit)
            result.resize(limit);

        return result;
    }

    std::vector<DEXPoolInfo>
    getPools(std::string const& sort, uint32_t limit) override
    {
        std::vector<DEXPoolInfo> result;
        if (!store_ || !store_->isOpen())
            return result;

        limit = std::min(limit, kMaxLimit);
        if (limit == 0)
            return result;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn, MDB_RDONLY) != 0)
            return result;

        MDB_cursor* cursor = nullptr;
        if (mdb_cursor_open(txn, store_->dbi(DexDB::AMMPools), &cursor) != 0)
        {
            mdb_txn_abort(txn);
            return result;
        }

        MDB_val key, data;
        int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
        while (rc == 0)
        {
            auto const* p = static_cast<uint8_t const*>(data.mv_data);
            size_t len = data.mv_size;

            if (len >= 8 + 8 + 8 + 2 + 1 + 8 + 8 + 8 + 8 + 4 + 4)
            {
                DEXPoolInfo pi;
                pi.account = std::string(
                    static_cast<char const*>(key.mv_data), key.mv_size);
                pi.asset1Balance = decodeDouble(p);
                pi.asset2Balance = decodeDouble(p + 8);
                pi.lptBalance = decodeDouble(p + 16);
                pi.tradingFee =
                    (uint16_t(p[24]) << 8) | p[25];
                pi.curveType = p[26];
                pi.tvlXrp = decodeDouble(p + 27);
                pi.volume24hXrp = decodeDouble(p + 35);
                pi.fees24hXrp = decodeDouble(p + 43);
                pi.apr = decodeDouble(p + 51);
                pi.ledgerSeq = decodeBE(p + 59);
                pi.timestamp = decodeBE(p + 63);
                result.push_back(std::move(pi));
            }
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);

        if (sort == "tvl")
        {
            std::sort(result.begin(), result.end(), [](auto& a, auto& b) {
                return a.tvlXrp > b.tvlXrp;
            });
        }
        else if (sort == "volume")
        {
            std::sort(result.begin(), result.end(), [](auto& a, auto& b) {
                return a.volume24hXrp > b.volume24hXrp;
            });
        }
        else if (sort == "apr")
        {
            std::sort(result.begin(), result.end(), [](auto& a, auto& b) {
                return a.apr > b.apr;
            });
        }

        if (result.size() > limit)
            result.resize(limit);

        return result;
    }
};

class DEXTimeSeriesReaderNoop final : public DEXTimeSeriesReader
{
public:
    std::optional<uint32_t>
    getLastIndexedSeq() override
    {
        return std::nullopt;
    }

    std::vector<DEXCandle>
    getCandles(
        std::string const&,
        DEXInterval,
        uint32_t,
        uint32_t,
        uint32_t) override
    {
        return {};
    }

    std::vector<DEXTrade>
    getTrades(std::string const&, uint32_t, uint32_t, uint32_t) override
    {
        return {};
    }

    std::vector<DEXAMMSnapshot>
    getAMMHistory(std::string const&, uint32_t, uint32_t, uint32_t) override
    {
        return {};
    }

    std::optional<DEXTokenSummary>
    getTokenSummary(std::string const&) override
    {
        return std::nullopt;
    }

    std::vector<DEXTokenSummary>
    getPairs(std::string const&, uint32_t) override
    {
        return {};
    }

    std::vector<DEXPoolInfo>
    getPools(std::string const&, uint32_t) override
    {
        return {};
    }
};

}  // namespace

std::unique_ptr<DEXTimeSeriesReader>
make_DEXTimeSeriesReader(DEXTimeSeriesStore* store, beast::Journal journal)
{
    if (!store)
        return std::make_unique<DEXTimeSeriesReaderNoop>();
    return std::make_unique<DEXTimeSeriesReaderImpl>(store, journal);
}

}  // namespace xrpl
