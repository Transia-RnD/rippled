#include <xrpld/app/misc/DEXTimeSeriesWriter.h>
#include <xrpld/app/misc/DEXTimeSeriesStore.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/rpc/BookChanges.h>

#include <xrpl/basics/BasicConfig.h>
#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/jss.h>

#include <lmdb.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace xrpl {

DEXTimeSeriesConfig
parseDEXTimeSeriesConfig(Section const& section)
{
    DEXTimeSeriesConfig cfg;
    set(cfg.enabled, "enabled", section);
    cfg.startSequence = get<std::string>(section, "start_sequence", "0");
    set(cfg.tickRetentionHours, "tick_retention_hours", section);
    set(cfg.candle1mRetentionHours, "candle_1m_retention_hours", section);
    set(cfg.candle5mRetentionHours, "candle_5m_retention_hours", section);
    set(cfg.candle1hRetentionHours, "candle_1h_retention_hours", section);
    set(cfg.candle1dRetentionHours, "candle_1d_retention_hours", section);
    set(cfg.ammSnapshotRetentionHours, "amm_snapshot_retention_hours", section);
    set(cfg.backfillBatchSize, "backfill_batch_size", section);
    set(cfg.backfillPauseMs, "backfill_pause_ms", section);
    set(cfg.purgeIntervalLedgers, "purge_interval_ledgers", section);
    set(cfg.mapSizeGB, "map_size_gb", section);
    cfg.dbPath = get<std::string>(section, "db_path", "dex_timeseries");
    return cfg;
}

namespace {

using namespace dex;

void
upsertCandle(
    DEXTimeSeriesStore& store,
    MDB_txn* txn,
    std::string_view bookKey,
    uint8_t interval,
    uint32_t bucketTs,
    double open,
    double high,
    double low,
    double close,
    double volBase,
    double volQuote,
    int txCount,
    double buyVol,
    double sellVol)
{
    CandleKey ck(bookKey, interval, bucketTs);
    auto key = ck.val();
    MDB_val existing;

    int rc = store.get(txn, DexDB::Candles, &key, &existing);
    if (rc == 0 && existing.mv_size == CandleValue::kSize)
    {
        auto prev = CandleValue::fromData(existing.mv_data);
        CandleValue merged(
            prev.open(),
            std::max(prev.high(), high),
            std::min(prev.low(), low),
            close,
            prev.volumeBase() + volBase,
            prev.volumeQuote() + volQuote,
            prev.txCount() + txCount,
            prev.buyVolumeBase() + buyVol,
            prev.sellVolumeBase() + sellVol);
        auto val = merged.val();
        store.put(txn, DexDB::Candles, &key, &val);
    }
    else
    {
        CandleValue cv(
            open, high, low, close, volBase, volQuote, txCount, buyVol, sellVol);
        auto val = cv.val();
        store.put(txn, DexDB::Candles, &key, &val);
    }
}

void
indexLedgerImpl(
    DEXTimeSeriesStore& store,
    MDB_txn* txn,
    std::shared_ptr<ReadView const> const& ledger,
    DEXTimeSeriesConfig const& config,
    beast::Journal const& journal)
{
    uint32_t const ledgerSeq = ledger->header().seq;
    uint32_t const ledgerTime =
        ledger->header().closeTime.time_since_epoch().count();

    Json::Value const bookChanges = RPC::computeBookChanges(ledger);

    int tickIdx = 0;
    auto const& changes = bookChanges[jss::changes];
    for (Json::UInt i = 0; i < changes.size(); ++i)
    {
        auto const& change = changes[i];

        std::string bookKey;
        if (change.isMember(jss::currency_a))
            bookKey = change[jss::currency_a].asString();
        else if (change.isMember("mpt_issuance_id_a"))
            bookKey = change["mpt_issuance_id_a"].asString();

        bookKey += "|";

        if (change.isMember(jss::currency_b))
            bookKey += change[jss::currency_b].asString();
        else if (change.isMember("mpt_issuance_id_b"))
            bookKey += change["mpt_issuance_id_b"].asString();

        double const rate = std::stod(change[jss::close].asString());
        double const high = std::stod(change[jss::high].asString());
        double const low = std::stod(change[jss::low].asString());
        double const open = std::stod(change[jss::open].asString());
        double const volumeA = std::stod(change[jss::volume_a].asString());
        double const volumeB = std::stod(change[jss::volume_b].asString());

        int const txIdx = tickIdx++;

        if (config.tickRetentionHours >= 0)
        {
            TradeKey tk(bookKey, ledgerTime, ledgerSeq, txIdx);
            auto key = tk.val();

            TradeValue tv;
            tv.rate = rate;
            tv.volumeBase = volumeA;
            tv.volumeQuote = volumeB;
            tv.side = 0;
            tv.source = 0;
            tv.taker = "";
            tv.txHash = "";
            auto encoded = tv.encode();
            MDB_val val = {encoded.size(), encoded.data()};
            store.put(txn, DexDB::Trades, &key, &val);
        }

        for (uint8_t iv = 0; iv <= 3; ++iv)
        {
            int32_t retention = 0;
            switch (iv)
            {
                case 0:
                    retention = config.candle1mRetentionHours;
                    break;
                case 1:
                    retention = config.candle5mRetentionHours;
                    break;
                case 2:
                    retention = config.candle1hRetentionHours;
                    break;
                case 3:
                    retention = config.candle1dRetentionHours;
                    break;
            }
            if (retention < 0)
                continue;

            uint32_t const bucket =
                bucketTimestamp(ledgerTime, intervalSeconds(iv));
            upsertCandle(
                store,
                txn,
                bookKey,
                iv,
                bucket,
                open,
                high,
                low,
                rate,
                volumeA,
                volumeB,
                1,
                0.0,
                0.0);
        }
    }

    if (config.ammSnapshotRetentionHours >= 0)
    {
        for (auto const& tx : ledger->txs)
        {
            if (!tx.first || !tx.second)
                continue;

            for (auto const& node :
                 tx.second->getFieldArray(sfAffectedNodes))
            {
                uint16_t const nodeType = node.getFieldU16(sfLedgerEntryType);
                if (nodeType != ltAMM)
                    continue;

                SField const& metaType = node.getFName();
                if (metaType != sfModifiedNode && metaType != sfCreatedNode)
                    continue;

                if (!node.isFieldPresent(sfFinalFields))
                    continue;

                auto const& ffBase = node.peekAtField(sfFinalFields);
                auto const& ff = ffBase.downcast<STObject>();

                std::string account;
                if (ff.isFieldPresent(sfAccount))
                    account = toBase58(ff.getAccountID(sfAccount));

                if (account.empty())
                    continue;

                std::string lptBalance;
                if (ff.isFieldPresent(sfLPTokenBalance))
                {
                    auto const& amt = ff.getFieldAmount(sfLPTokenBalance);
                    lptBalance = amt.getText();
                }

                uint16_t tradingFee = 0;
                if (ff.isFieldPresent(sfTradingFee))
                    tradingFee = ff.getFieldU16(sfTradingFee);

                double lptBal = 0.0;
                try
                {
                    if (!lptBalance.empty())
                        lptBal = std::stod(lptBalance);
                }
                catch (...)
                {
                }

                AMMSnapshotKey sk(account, ledgerSeq);
                auto key = sk.val();
                AMMSnapshotValue sv(
                    0.0,
                    0.0,
                    lptBal,
                    tradingFee,
                    0,
                    0.0,
                    0.0,
                    0.0,
                    ledgerTime);
                auto val = sv.val();
                store.put(txn, DexDB::AMMSnapshots, &key, &val);
            }
        }
    }

    store.setMeta(txn, "last_indexed_seq", std::to_string(ledgerSeq));
}

class DEXTimeSeriesWriterImpl final : public DEXTimeSeriesWriter
{
    DEXTimeSeriesConfig const config_;
    Application& app_;
    beast::Journal const journal_;
    std::unique_ptr<DEXTimeSeriesStore> store_;
    uint32_t ledgersSincePurge_ = 0;

    std::atomic<bool> running_{false};
    std::thread backfillThread_;

public:
    DEXTimeSeriesWriterImpl(
        DEXTimeSeriesConfig const& config,
        Application& app,
        beast::Journal journal)
        : config_(config)
        , app_(app)
        , journal_(journal)
        , store_(std::make_unique<DEXTimeSeriesStore>(journal))
    {
    }

    ~DEXTimeSeriesWriterImpl() override
    {
        stop();
        close();
    }

    bool
    open(std::string const& dataDir) override
    {
        std::string base = dataDir;
        if (base.empty())
            base = std::filesystem::temp_directory_path().string();
        std::string path = base + "/" + config_.dbPath;
        std::filesystem::create_directories(path);

        if (!store_->open(path, config_.mapSizeGB))
        {
            JLOG(journal_.error()) << "DEXTimeSeries: failed to open LMDB";
            return false;
        }

        JLOG(journal_.info()) << "DEXTimeSeries: LMDB opened at " << path;
        return true;
    }

    void
    close() override
    {
        if (store_)
            store_->close();
    }

    void
    start() override
    {
        if (!store_ || !store_->isOpen())
            return;
        if (config_.startSequence == "0")
            return;
        running_ = true;
        backfillThread_ = std::thread([this] { doBackfill(); });
    }

    void
    stop() override
    {
        if (!running_.exchange(false))
            return;
        if (backfillThread_.joinable())
            backfillThread_.join();
    }

    void
    index(std::shared_ptr<ReadView const> const& ledger) override
    {
        if (!store_ || !store_->isOpen())
            return;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn) != 0)
            return;

        try
        {
            indexLedgerImpl(*store_, txn, ledger, config_, journal_);
            mdb_txn_commit(txn);
        }
        catch (std::exception const& e)
        {
            mdb_txn_abort(txn);
            JLOG(journal_.warn())
                << "DEXTimeSeries: index error: " << e.what();
            return;
        }

        uint32_t const ledgerSeq = ledger->header().seq;
        uint32_t const ledgerTime =
            ledger->header().closeTime.time_since_epoch().count();

        if (ledgerSeq % 100 == 0)
        {
            JLOG(journal_.info())
                << "DEXTimeSeries: indexed ledger " << ledgerSeq;
        }

        if (++ledgersSincePurge_ >= config_.purgeIntervalLedgers)
        {
            ledgersSincePurge_ = 0;
            purgeOldData(ledgerTime);
        }
    }

    void
    flush() override
    {
        if (store_ && store_->isOpen() && store_->env())
            mdb_env_sync(store_->env(), 1);
    }

    DEXTimeSeriesStore*
    getStore() override
    {
        return store_.get();
    }

private:
    void
    doBackfill()
    {
        std::this_thread::sleep_for(std::chrono::seconds(15));

        if (!running_ || !store_ || !store_->isOpen())
            return;

        auto& lm = app_.getLedgerMaster();

        auto const validated = lm.getValidatedLedger();
        if (!validated)
        {
            JLOG(journal_.warn())
                << "DEXTimeSeries: no validated ledger, skipping backfill";
            return;
        }

        uint32_t const validatedSeq = validated->header().seq;

        uint32_t startSeq = 0;
        auto lastSeqStr = store_->getMeta("last_indexed_seq");
        if (lastSeqStr && !lastSeqStr->empty())
            startSeq = static_cast<uint32_t>(std::stoul(*lastSeqStr));

        if (startSeq == 0)
        {
            if (config_.startSequence == "0")
            {
                JLOG(journal_.info())
                    << "DEXTimeSeries: start_sequence=0, no backfill";
                return;
            }
            else if (config_.startSequence == "full")
            {
                startSeq = lm.getEarliestFetch();
                if (startSeq == 0)
                    startSeq = 1;
                JLOG(journal_.info())
                    << "DEXTimeSeries: full backfill from ledger " << startSeq;
            }
            else
            {
                startSeq =
                    static_cast<uint32_t>(std::stoul(config_.startSequence));
                JLOG(journal_.info())
                    << "DEXTimeSeries: backfill from ledger " << startSeq;
            }
        }
        else
        {
            startSeq += 1;
        }

        if (startSeq >= validatedSeq)
        {
            JLOG(journal_.info())
                << "DEXTimeSeries: already up to date at ledger "
                << (startSeq - 1);
            return;
        }

        uint32_t const gap = validatedSeq - startSeq;
        JLOG(journal_.info())
            << "DEXTimeSeries: backfilling " << gap << " ledgers ("
            << startSeq << " -> " << validatedSeq << ")";

        // Set backfill status
        {
            MDB_txn* txn = nullptr;
            if (store_->beginTxn(&txn) == 0)
            {
                store_->setMeta(txn, "backfill_status", "running");
                store_->setMeta(
                    txn,
                    "backfill_start_seq",
                    std::to_string(startSeq));
                store_->setMeta(
                    txn,
                    "backfill_target_seq",
                    std::to_string(validatedSeq));
                mdb_txn_commit(txn);
            }
        }

        uint32_t indexed = 0;
        uint32_t skipped = 0;

        for (uint32_t seq = startSeq; seq <= validatedSeq && running_; ++seq)
        {
            auto ledger = lm.getLedgerBySeq(seq);
            if (!ledger)
            {
                ++skipped;
                continue;
            }

            MDB_txn* txn = nullptr;
            if (store_->beginTxn(&txn) != 0)
                continue;

            try
            {
                indexLedgerImpl(*store_, txn, ledger, config_, journal_);
                mdb_txn_commit(txn);
                ++indexed;
            }
            catch (std::exception const& e)
            {
                mdb_txn_abort(txn);
                JLOG(journal_.warn())
                    << "DEXTimeSeries: backfill error at ledger " << seq
                    << ": " << e.what();
                continue;
            }

            if (indexed % 1000 == 0)
            {
                uint32_t const remaining = validatedSeq - seq;
                JLOG(journal_.info())
                    << "DEXTimeSeries: backfilled to ledger " << seq << " ("
                    << indexed << " indexed, " << skipped << " skipped, "
                    << remaining << " remaining)";
            }

            if (indexed % config_.backfillBatchSize == 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.backfillPauseMs));
        }

        // Set backfill complete
        {
            MDB_txn* txn = nullptr;
            if (store_->beginTxn(&txn) == 0)
            {
                store_->setMeta(txn, "backfill_status", "complete");
                mdb_txn_commit(txn);
            }
        }

        JLOG(journal_.info()) << "DEXTimeSeries: backfill complete — "
                              << indexed << " ledgers indexed, " << skipped
                              << " unavailable";
    }

    void
    purgeOldData(uint32_t now)
    {
        if (!store_ || !store_->isOpen())
            return;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn) != 0)
            return;

        // For trades: key = bookKey\0 | timestamp(4) | ...
        // tsOffset varies by bookKey length, so we scan and decode
        // We use a simpler approach: scan all entries and check timestamp
        if (config_.tickRetentionHours > 0)
        {
            uint32_t const cutoff =
                now - (config_.tickRetentionHours * 3600);
            MDB_cursor* cursor = nullptr;
            if (mdb_cursor_open(
                    txn, store_->dbi(DexDB::Trades), &cursor) == 0)
            {
                MDB_val key, data;
                int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
                while (rc == 0)
                {
                    auto const* p =
                        static_cast<uint8_t const*>(key.mv_data);
                    auto const* nul = static_cast<uint8_t const*>(
                        std::memchr(p, '\0', key.mv_size));
                    if (nul && (key.mv_size - (nul - p + 1)) >= 4)
                    {
                        uint32_t ts = dex::decodeBE(nul + 1);
                        if (ts >= cutoff)
                            break;
                        mdb_cursor_del(cursor, 0);
                    }
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
        }

        // For candles: key = bookKey\0 | interval(1) | bucket_ts(4)
        auto purgeCandles = [&](uint8_t interval, int32_t retentionHours) {
            if (retentionHours <= 0)
                return;
            uint32_t const cutoff = now - (retentionHours * 3600);
            MDB_cursor* cursor = nullptr;
            if (mdb_cursor_open(
                    txn, store_->dbi(DexDB::Candles), &cursor) == 0)
            {
                MDB_val key, data;
                int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
                while (rc == 0)
                {
                    auto const* p =
                        static_cast<uint8_t const*>(key.mv_data);
                    auto const* nul = static_cast<uint8_t const*>(
                        std::memchr(p, '\0', key.mv_size));
                    if (nul && (key.mv_size - (nul - p + 1)) >= 5)
                    {
                        uint8_t iv = *(nul + 1);
                        uint32_t ts = dex::decodeBE(nul + 2);
                        if (iv == interval && ts < cutoff)
                        {
                            mdb_cursor_del(cursor, 0);
                        }
                        else if (iv > interval)
                        {
                            break;
                        }
                    }
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
        };

        purgeCandles(0, config_.candle1mRetentionHours);
        purgeCandles(1, config_.candle5mRetentionHours);
        purgeCandles(2, config_.candle1hRetentionHours);
        purgeCandles(3, config_.candle1dRetentionHours);

        // AMM snapshots: key = account(35) | ledger_seq(4)
        // We check timestamp from the value
        if (config_.ammSnapshotRetentionHours > 0)
        {
            uint32_t const cutoff =
                now - (config_.ammSnapshotRetentionHours * 3600);
            MDB_cursor* cursor = nullptr;
            if (mdb_cursor_open(
                    txn, store_->dbi(DexDB::AMMSnapshots), &cursor) == 0)
            {
                MDB_val key, data;
                int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
                while (rc == 0)
                {
                    if (data.mv_size >= AMMSnapshotValue::kSize)
                    {
                        auto sv =
                            AMMSnapshotValue::fromData(data.mv_data);
                        if (sv.timestamp() >= cutoff)
                            break;
                        mdb_cursor_del(cursor, 0);
                    }
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
        }

        mdb_txn_commit(txn);
        JLOG(journal_.debug()) << "DEXTimeSeries: purged old data";
    }
};

class DEXTimeSeriesWriterNoop final : public DEXTimeSeriesWriter
{
public:
    bool
    open(std::string const&) override
    {
        return false;
    }
    void
    close() override
    {
    }
    void
    start() override
    {
    }
    void
    stop() override
    {
    }
    void
    index(std::shared_ptr<ReadView const> const&) override
    {
    }
    void
    flush() override
    {
    }
    DEXTimeSeriesStore*
    getStore() override
    {
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<DEXTimeSeriesWriter>
make_DEXTimeSeriesWriter(
    DEXTimeSeriesConfig const& config,
    Application& app,
    beast::Journal journal)
{
    if (!config.enabled)
        return std::make_unique<DEXTimeSeriesWriterNoop>();
    return std::make_unique<DEXTimeSeriesWriterImpl>(config, app, journal);
}

}  // namespace xrpl
