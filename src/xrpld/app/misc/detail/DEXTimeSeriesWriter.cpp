#include <xrpld/app/misc/DEXTimeSeriesWriter.h>
#include <xrpld/app/misc/DEXTimeSeriesStore.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/rpc/BookChanges.h>
#include <xrpld/rpc/detail/TrustLine.h>

#include <xrpl/basics/BasicConfig.h>
#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/ledger/helpers/AMMHelpers.h>
#include <xrpl/ledger/helpers/DirectoryHelpers.h>
#include <xrpl/ledger/helpers/TokenHelpers.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/jss.h>

#include <xrpl/json/json_reader.h>
#include <xrpl/net/HTTPClient.h>

#include <boost/asio/io_context.hpp>

#include <lmdb.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
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
    cfg.bootstrapUrl = get<std::string>(section, "bootstrap_url", "");
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

double
lookupCandleClose(
    DEXTimeSeriesStore& store,
    MDB_txn* txn,
    std::string_view bookKey,
    uint8_t interval,
    uint32_t bucketTs)
{
    CandleKey ck(bookKey, interval, bucketTs);
    auto key = ck.val();
    MDB_val existing;
    int rc = store.get(txn, DexDB::Candles, &key, &existing);
    if (rc == 0 && existing.mv_size == CandleValue::kSize)
    {
        auto cv = CandleValue::fromData(existing.mv_data);
        return cv.close();
    }
    return 0.0;
}

double
safeStod(std::string const& s) noexcept
{
    try
    {
        return std::stod(s);
    }
    catch (...)
    {
        return 0.0;
    }
}

double
stamountToDouble(STAmount const& amt)
{
    if (amt.native())
        return static_cast<double>(amt.xrp().drops());
    return safeStod(amt.getText());
}

struct Rolling24hMetrics
{
    double volBase = 0.0;
    double volQuote = 0.0;
    double high = 0.0;
    double low = 0.0;
    uint32_t txCount = 0;
};

Rolling24hMetrics
computeRollingMetrics(
    DEXTimeSeriesStore& store,
    MDB_txn* txn,
    std::string_view bookKey,
    uint8_t candleInterval,
    uint32_t numBuckets,
    uint32_t now)
{
    Rolling24hMetrics m;
    uint32_t const ivSec = intervalSeconds(candleInterval);
    uint32_t const currentBucket = bucketTimestamp(now, ivSec);

    for (uint32_t i = 0; i < numBuckets; ++i)
    {
        uint32_t bucket = currentBucket - i * ivSec;
        CandleKey ck(bookKey, candleInterval, bucket);
        auto key = ck.val();
        MDB_val data;
        int rc = store.get(txn, DexDB::Candles, &key, &data);
        if (rc == 0 && data.mv_size == CandleValue::kSize)
        {
            auto cv = CandleValue::fromData(data.mv_data);
            m.volBase += cv.volumeBase();
            m.volQuote += cv.volumeQuote();
            m.txCount += cv.txCount();
            if (cv.high() > m.high)
                m.high = cv.high();
            if (m.low == 0.0 || cv.low() < m.low)
                m.low = cv.low();
        }
    }
    return m;
}

void
updateSummary(
    DEXTimeSeriesStore& store,
    MDB_txn* txn,
    std::string const& bookKey,
    double lastPrice,
    uint32_t ledgerTime)
{
    auto m24h = computeRollingMetrics(store, txn, bookKey, 2, 24, ledgerTime);

    auto priceAt = [&](uint32_t secsAgo) -> double {
        uint32_t targetTs = ledgerTime > secsAgo ? ledgerTime - secsAgo : 0;
        uint8_t iv;
        if (secsAgo <= 300)
            iv = 0;
        else if (secsAgo <= 3600)
            iv = 1;
        else if (secsAgo <= 86400)
            iv = 2;
        else
            iv = 3;
        uint32_t bucket = bucketTimestamp(targetTs, intervalSeconds(iv));
        return lookupCandleClose(store, txn, bookKey, iv, bucket);
    };

    auto pctChange = [&](double ref) -> double {
        if (ref > 0.0)
            return (lastPrice - ref) / ref;
        return 0.0;
    };

    double pc5m = pctChange(priceAt(300));
    double pc1h = pctChange(priceAt(3600));
    double pc24h = pctChange(priceAt(86400));
    double pc7d = pctChange(priceAt(604800));
    double pc30d = pctChange(priceAt(2592000));

    SummaryValue sv;
    sv.set(
        lastPrice, pc5m, pc1h, pc24h, pc7d, pc30d,
        m24h.volBase, m24h.volQuote, m24h.high, m24h.low,
        m24h.txCount, ledgerTime);
    MDB_val sKey = {bookKey.size(), const_cast<char*>(bookKey.data())};
    auto val = sv.val();
    store.put(txn, DexDB::Summaries, &sKey, &val);
}

struct BookKeyParts
{
    std::string issuer;
    std::string currency;
    bool xrpIsBase = false;
};

std::optional<BookKeyParts>
parseBookKeyForBootstrap(std::string const& bookKey)
{
    auto pipe = bookKey.find('|');
    if (pipe == std::string::npos)
        return std::nullopt;

    std::string sideA = bookKey.substr(0, pipe);
    std::string sideB = bookKey.substr(pipe + 1);

    std::string iouSide;
    bool xrpIsBase = false;

    if (sideA == "XRP_drops" || sideA == "XRP")
    {
        iouSide = sideB;
        xrpIsBase = true;
    }
    else if (sideB == "XRP_drops" || sideB == "XRP")
    {
        iouSide = sideA;
        xrpIsBase = false;
    }
    else
    {
        return std::nullopt;
    }

    auto slash = iouSide.find('/');
    if (slash == std::string::npos)
        return std::nullopt;

    return BookKeyParts{
        iouSide.substr(0, slash),
        iouSide.substr(slash + 1),
        xrpIsBase};
}

void
bootstrapBookFromApi(
    DEXTimeSeriesStore& store,
    std::string const& bootstrapUrl,
    std::string const& bookKey,
    beast::Journal journal)
{
    auto parts = parseBookKeyForBootstrap(bookKey);
    if (!parts)
    {
        JLOG(journal.debug())
            << "DEXTimeSeries: bootstrap skip non-XRP pair: " << bookKey;
        return;
    }

    static constexpr std::array<std::pair<char const*, uint8_t>, 4> intervals = {{
        {"1d", 3}, {"1h", 2}, {"5m", 1}, {"1m", 0}
    }};

    for (auto const& [ivStr, ivIdx] : intervals)
    {
        std::string path =
            "/v1/iou/market_data/" + parts->issuer + "_" +
            parts->currency + "/XRP?interval=" + ivStr + "&limit=1000";

        // Parse host from bootstrapUrl
        std::string host = bootstrapUrl;
        bool ssl = false;
        unsigned short port = 80;

        if (host.find("https://") == 0)
        {
            host = host.substr(8);
            ssl = true;
            port = 443;
        }
        else if (host.find("http://") == 0)
        {
            host = host.substr(7);
        }

        auto colonPos = host.find(':');
        if (colonPos != std::string::npos)
        {
            port = static_cast<unsigned short>(
                std::stoi(host.substr(colonPos + 1)));
            host = host.substr(0, colonPos);
        }

        std::string responseBody;
        bool success = false;

        try
        {
            boost::asio::io_context ioc;
            HTTPClient::get(
                ssl,
                ioc,
                host,
                port,
                path,
                4 * 1024 * 1024,
                std::chrono::seconds(30),
                [&](boost::system::error_code const& ec,
                    int status,
                    std::string const& data) -> bool {
                    if (!ec && status == 200)
                    {
                        responseBody = data;
                        success = true;
                    }
                    return true;
                },
                journal);
            ioc.run();
        }
        catch (...)
        {
            continue;
        }

        if (!success || responseBody.empty())
            continue;

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(responseBody, root) || !root.isArray())
            continue;

        MDB_txn* txn = nullptr;
        if (store.beginTxn(&txn) != 0)
            continue;

        uint32_t written = 0;
        for (Json::UInt i = 0; i < root.size(); ++i)
        {
            auto const& c = root[i];
            if (!c.isMember("open") || !c.isMember("close") ||
                !c.isMember("high") || !c.isMember("low"))
                continue;

            double open = c["open"].asDouble();
            double high = c["high"].asDouble();
            double low = c["low"].asDouble();
            double close = c["close"].asDouble();
            double volBase = c.isMember("volume") ? c["volume"].asDouble() : 0.0;
            double volQuote = c.isMember("volume_quote")
                ? c["volume_quote"].asDouble() : 0.0;
            uint32_t txCount = c.isMember("count")
                ? c["count"].asUInt() : 0;

            uint32_t ts = 0;
            if (c.isMember("timestamp"))
            {
                if (c["timestamp"].isUInt() || c["timestamp"].isInt())
                    ts = c["timestamp"].asUInt();
            }
            if (ts == 0)
                continue;

            uint32_t bucket = bucketTimestamp(ts, intervalSeconds(ivIdx));
            CandleKey ck(bookKey, ivIdx, bucket);
            auto key = ck.val();

            MDB_val existing;
            if (store.get(txn, DexDB::Candles, &key, &existing) == 0)
                continue;

            CandleValue cv(
                open, high, low, close, volBase, volQuote, txCount, 0.0, 0.0);
            auto val = cv.val();
            store.put(txn, DexDB::Candles, &key, &val);
            ++written;
        }

        mdb_txn_commit(txn);

        JLOG(journal.info())
            << "DEXTimeSeries: bootstrapped " << written << " candles ("
            << ivStr << ") for " << bookKey;
    }

    MDB_txn* txn = nullptr;
    if (store.beginTxn(&txn) == 0)
    {
        std::string metaKey = "bs:" + bookKey;
        store.setMeta(txn, metaKey, "1");
        mdb_txn_commit(txn);
    }
}

void
writePoolState(
    DEXTimeSeriesStore& store,
    MDB_txn* txn,
    std::string const& account,
    double a1Bal,
    double a2Bal,
    double lptBal,
    uint16_t tradingFee,
    uint8_t curveType,
    uint32_t ledgerSeq,
    uint32_t ledgerTime)
{
    static constexpr size_t kPoolSize = 8+8+8+2+1+8+8+8+8+4+4;
    uint8_t buf[kPoolSize];
    encodeDouble(a1Bal, buf);
    encodeDouble(a2Bal, buf + 8);
    encodeDouble(lptBal, buf + 16);
    buf[24] = (tradingFee >> 8) & 0xFF;
    buf[25] = tradingFee & 0xFF;
    buf[26] = curveType;
    encodeDouble(0.0, buf + 27);   // tvlXrp (placeholder)
    encodeDouble(0.0, buf + 35);   // volume24hXrp
    encodeDouble(0.0, buf + 43);   // fees24hXrp
    encodeDouble(0.0, buf + 51);   // apr
    encodeBE(ledgerSeq, buf + 59);
    encodeBE(ledgerTime, buf + 63);
    MDB_val key = {account.size(), const_cast<char*>(account.data())};
    MDB_val val = {kPoolSize, buf};
    store.put(txn, DexDB::AMMPools, &key, &val);
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

    if (changes.size() > 0)
    {
        JLOG(journal.trace())
            << "DEXTimeSeries: indexing ledger " << ledgerSeq
            << " ts=" << ledgerTime
            << " book_changes=" << changes.size();
    }
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

        if (bookKey.size() > 512)
            continue;

        double const rate = safeStod(change[jss::close].asString());
        double const high = safeStod(change[jss::high].asString());
        double const low = safeStod(change[jss::low].asString());
        double const open = safeStod(change[jss::open].asString());
        double const volumeA = safeStod(change[jss::volume_a].asString());
        double const volumeB = safeStod(change[jss::volume_b].asString());

        int const txIdx = tickIdx++;

        JLOG(journal.trace())
            << "DEXTimeSeries: trade seq=" << ledgerSeq
            << " book=" << bookKey
            << " rate=" << rate
            << " volA=" << volumeA << " volB=" << volumeB;

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

        updateSummary(store, txn, bookKey, rate, ledgerTime);
    }

    if (config.ammSnapshotRetentionHours >= 0)
    {
        for (auto const& tx : ledger->txs)
        {
            if (!tx.first || !tx.second)
                continue;

            if (!tx.second->isFieldPresent(sfAffectedNodes))
                continue;

            for (auto const& node :
                 tx.second->getFieldArray(sfAffectedNodes))
            {
                if (!node.isFieldPresent(sfLedgerEntryType))
                    continue;
                uint16_t const nodeType = node.getFieldU16(sfLedgerEntryType);
                if (nodeType != ltAMM)
                    continue;

                SField const& metaType = node.getFName();
                if (metaType != sfModifiedNode && metaType != sfCreatedNode)
                    continue;

                SField const& fieldsKey =
                    node.isFieldPresent(sfFinalFields) ? sfFinalFields
                                                       : sfNewFields;
                if (!node.isFieldPresent(fieldsKey))
                    continue;

                auto const& ffBase = node.peekAtField(fieldsKey);
                auto const& ff = ffBase.downcast<STObject>();

                if (!ff.isFieldPresent(sfAccount) ||
                    !ff.isFieldPresent(sfAsset) ||
                    !ff.isFieldPresent(sfAsset2))
                    continue;

                auto const ammAccountID = ff.getAccountID(sfAccount);
                std::string account = toBase58(ammAccountID);

                auto const& asset1 = ff[sfAsset];
                auto const& asset2 = ff[sfAsset2];

                auto const [bal1, bal2] = ammPoolHolds(
                    *ledger,
                    ammAccountID,
                    asset1,
                    asset2,
                    fhIGNORE_FREEZE,
                    ahIGNORE_AUTH,
                    journal);
                double a1Bal = stamountToDouble(bal1);
                double a2Bal = stamountToDouble(bal2);

                double lptBal = 0.0;
                if (ff.isFieldPresent(sfLPTokenBalance))
                    lptBal = safeStod(
                        ff.getFieldAmount(sfLPTokenBalance).getText());

                uint16_t tradingFee = 0;
                if (ff.isFieldPresent(sfTradingFee))
                    tradingFee = ff.getFieldU16(sfTradingFee);

                uint8_t curveType = 0;

                JLOG(journal.trace())
                    << "DEXTimeSeries: AMM snapshot seq=" << ledgerSeq
                    << " account=" << account
                    << " a1=" << a1Bal << " a2=" << a2Bal
                    << " lpt=" << lptBal << " fee=" << tradingFee;

                AMMSnapshotKey sk(account, ledgerSeq);
                auto key = sk.val();
                AMMSnapshotValue sv(
                    a1Bal,
                    a2Bal,
                    lptBal,
                    tradingFee,
                    curveType,
                    0.0,
                    0.0,
                    0.0,
                    ledgerTime);
                auto val = sv.val();
                store.put(txn, DexDB::AMMSnapshots, &key, &val);

                writePoolState(
                    store, txn, account,
                    a1Bal, a2Bal, lptBal, tradingFee, curveType,
                    ledgerSeq, ledgerTime);
            }
        }
    }

    // Incremental token info: apply deltas from AffectedNodes metadata
    // instead of full trust-line scans. O(changed_lines) per ledger.
    struct TokenDelta
    {
        double supplyDelta = 0.0;
        double frozenDelta = 0.0;
        double lockedDelta = 0.0;
        int32_t holdersDelta = 0;
        int32_t trustLinesDelta = 0;
    };

    std::map<std::string, TokenDelta> tokenDeltas;
    std::set<std::string> newTokens;

    for (auto const& tx : ledger->txs)
    {
        if (!tx.first || !tx.second)
            continue;
        if (!tx.second->isFieldPresent(sfAffectedNodes))
            continue;

        for (auto const& node :
             tx.second->getFieldArray(sfAffectedNodes))
        {
            if (!node.isFieldPresent(sfLedgerEntryType))
                continue;
            uint16_t const nodeType =
                node.getFieldU16(sfLedgerEntryType);

            SField const& metaType = node.getFName();
            bool const isCreated = (metaType == sfCreatedNode);
            bool const isDeleted = (metaType == sfDeletedNode);
            bool const isModified = (metaType == sfModifiedNode);
            if (!isCreated && !isDeleted && !isModified)
                continue;

            if (nodeType == ltRIPPLE_STATE)
            {
                SField const* fieldsKey = nullptr;
                if (isCreated && node.isFieldPresent(sfNewFields))
                    fieldsKey = &sfNewFields;
                else if (node.isFieldPresent(sfFinalFields))
                    fieldsKey = &sfFinalFields;
                else
                    continue;

                auto const& ff =
                    node.peekAtField(*fieldsKey).downcast<STObject>();
                if (!ff.isFieldPresent(sfBalance) ||
                    !ff.isFieldPresent(sfHighLimit) ||
                    !ff.isFieldPresent(sfLowLimit))
                    continue;

                auto const& bal = ff.getFieldAmount(sfBalance);
                if (bal.holds<MPTIssue>())
                    continue;
                auto const& issue = bal.get<Issue>();
                if (isXRP(issue.currency))
                    continue;

                std::string const highAcct =
                    toBase58(ff.getFieldAmount(sfHighLimit).getIssuer());
                std::string const lowAcct =
                    toBase58(ff.getFieldAmount(sfLowLimit).getIssuer());
                std::string const currStr = to_string(issue.currency);

                uint32_t const newFlags = ff.isFieldPresent(sfFlags)
                    ? ff.getFieldU32(sfFlags)
                    : 0;
                double const newRawBal = safeStod(bal.getText());

                for (int side = 0; side < 2; ++side)
                {
                    bool const issuerIsHigh = (side == 0);
                    std::string const tk =
                        (issuerIsHigh ? highAcct : lowAcct) + "/" + currStr;

                    MDB_val chk = {
                        tk.size(), const_cast<char*>(tk.data())};
                    MDB_val dummy;
                    if (store.get(
                            txn, DexDB::TokenInfo, &chk, &dummy) != 0)
                        continue;

                    auto& delta = tokenDeltas[tk];

                    bool const newFrozen = issuerIsHigh
                        ? (newFlags & lsfHighFreeze) != 0
                        : (newFlags & lsfLowFreeze) != 0;
                    double const newIssuerBal =
                        issuerIsHigh ? -newRawBal : newRawBal;
                    bool const newIsHolder = (newIssuerBal < 0);
                    double const newAmt =
                        newIsHolder ? -newIssuerBal : 0.0;
                    double const newSupply =
                        (!newFrozen && newIsHolder) ? newAmt : 0.0;
                    double const newFrozenAmt =
                        (newFrozen && newIsHolder) ? newAmt : 0.0;

                    if (isCreated)
                    {
                        delta.trustLinesDelta += 1;
                        delta.supplyDelta += newSupply;
                        delta.frozenDelta += newFrozenAmt;
                        if (newIsHolder)
                            delta.holdersDelta += 1;
                    }
                    else if (isDeleted)
                    {
                        delta.trustLinesDelta -= 1;
                        delta.supplyDelta -= newSupply;
                        delta.frozenDelta -= newFrozenAmt;
                        if (newIsHolder)
                            delta.holdersDelta -= 1;
                    }
                    else
                    {
                        double oldRawBal = newRawBal;
                        uint32_t oldFlags = newFlags;

                        if (node.isFieldPresent(sfPreviousFields))
                        {
                            auto const& pf =
                                node.peekAtField(sfPreviousFields)
                                    .downcast<STObject>();
                            if (pf.isFieldPresent(sfBalance))
                                oldRawBal = safeStod(
                                    pf.getFieldAmount(sfBalance)
                                        .getText());
                            if (pf.isFieldPresent(sfFlags))
                                oldFlags = pf.getFieldU32(sfFlags);
                        }

                        bool const oldFrozen = issuerIsHigh
                            ? (oldFlags & lsfHighFreeze) != 0
                            : (oldFlags & lsfLowFreeze) != 0;
                        double const oldIssuerBal =
                            issuerIsHigh ? -oldRawBal : oldRawBal;
                        bool const oldIsHolder = (oldIssuerBal < 0);
                        double const oldAmt =
                            oldIsHolder ? -oldIssuerBal : 0.0;
                        double const oldSupply =
                            (!oldFrozen && oldIsHolder) ? oldAmt : 0.0;
                        double const oldFrozenAmt =
                            (oldFrozen && oldIsHolder) ? oldAmt : 0.0;

                        delta.supplyDelta += (newSupply - oldSupply);
                        delta.frozenDelta +=
                            (newFrozenAmt - oldFrozenAmt);
                        if (newIsHolder && !oldIsHolder)
                            delta.holdersDelta += 1;
                        if (!newIsHolder && oldIsHolder)
                            delta.holdersDelta -= 1;
                    }
                }
            }
            else if (nodeType == ltESCROW)
            {
                SField const* fieldsKey = nullptr;
                if (isCreated && node.isFieldPresent(sfNewFields))
                    fieldsKey = &sfNewFields;
                else if (node.isFieldPresent(sfFinalFields))
                    fieldsKey = &sfFinalFields;
                else
                    continue;

                auto const& ff =
                    node.peekAtField(*fieldsKey).downcast<STObject>();
                if (!ff.isFieldPresent(sfAmount))
                    continue;
                auto const& amt = ff.getFieldAmount(sfAmount);
                if (amt.holds<MPTIssue>() || amt.native())
                    continue;
                auto const& issue = amt.get<Issue>();
                std::string const tk =
                    toBase58(issue.account) + "/" +
                    to_string(issue.currency);

                MDB_val chk = {
                    tk.size(), const_cast<char*>(tk.data())};
                MDB_val dummy;
                if (store.get(txn, DexDB::TokenInfo, &chk, &dummy) != 0)
                    continue;

                double const escrowAmt = safeStod(amt.getText());
                auto& delta = tokenDeltas[tk];

                if (isCreated)
                    delta.lockedDelta += escrowAmt;
                else if (isDeleted)
                    delta.lockedDelta -= escrowAmt;
            }
        }
    }

    // Bootstrap: first trade for a new token seeds the cache via full scan
    for (Json::UInt i = 0; i < changes.size(); ++i)
    {
        auto const& change = changes[i];
        if (!change.isMember(jss::currency_a) ||
            !change.isMember(jss::currency_b))
            continue;

        std::string const sideA = change[jss::currency_a].asString();
        std::string const sideB = change[jss::currency_b].asString();

        std::string tokenKey;
        if (sideA == "XRP_drops" && sideB.find('/') != std::string::npos)
            tokenKey = sideB;
        else if (
            sideB == "XRP_drops" && sideA.find('/') != std::string::npos)
            tokenKey = sideA;
        else
            continue;

        MDB_val chk = {
            tokenKey.size(), const_cast<char*>(tokenKey.data())};
        MDB_val dummy;
        if (store.get(txn, DexDB::TokenInfo, &chk, &dummy) != 0)
            newTokens.insert(tokenKey);
    }

    // Apply incremental deltas to tracked tokens
    for (auto const& [tokenKey, delta] : tokenDeltas)
    {
        MDB_val tiKey = {
            tokenKey.size(), const_cast<char*>(tokenKey.data())};
        MDB_val existing;
        if (store.get(txn, DexDB::TokenInfo, &tiKey, &existing) != 0 ||
            existing.mv_size != dex::TokenInfoValue::kSize)
            continue;

        auto cached = dex::TokenInfoValue::fromData(existing.mv_data);

        double newSupply =
            std::max(0.0, cached.supply() + delta.supplyDelta);
        double newFrozen =
            std::max(0.0, cached.frozenSupply() + delta.frozenDelta);
        double newLocked =
            std::max(0.0, cached.lockedSupply() + delta.lockedDelta);
        uint32_t newHolders = static_cast<uint32_t>(std::max(
            0,
            static_cast<int32_t>(cached.holders()) +
                delta.holdersDelta));
        uint32_t newTrustLines = static_cast<uint32_t>(std::max(
            0,
            static_cast<int32_t>(cached.trustLines()) +
                delta.trustLinesDelta));

        dex::TokenInfoValue tiv;
        tiv.set(newSupply, newFrozen, newLocked, newHolders,
                newTrustLines, ledgerSeq);
        auto tival = tiv.val();
        store.put(txn, DexDB::TokenInfo, &tiKey, &tival);

        JLOG(journal.trace())
            << "DEXTimeSeries: delta update " << tokenKey
            << " supply=" << newSupply << " holders=" << newHolders;
    }

    // Full scan only for brand-new tokens (one-time bootstrap)
    for (auto const& tokenKey : newTokens)
    {
        auto slash = tokenKey.find('/');
        if (slash == std::string::npos)
            continue;

        std::string const issuerStr = tokenKey.substr(0, slash);
        std::string const currencyStr = tokenKey.substr(slash + 1);

        auto const issuerID = parseBase58<AccountID>(issuerStr);
        if (!issuerID)
            continue;

        Currency currency;
        if (!to_currency(currency, currencyStr))
            continue;

        if (!ledger->exists(keylet::account(*issuerID)))
            continue;

        double supply = 0.0;
        double frozenSupply = 0.0;
        double lockedSupply = 0.0;
        uint32_t holders = 0;
        uint32_t trustLines = 0;

        forEachItem(
            *ledger,
            *issuerID,
            [&](std::shared_ptr<SLE const> const& sle) {
                if (sle->getType() == ltESCROW)
                {
                    auto const& escrow = sle->getFieldAmount(sfAmount);
                    if (escrow.holds<MPTIssue>())
                        return;
                    if (escrow.get<Issue>().currency != currency)
                        return;
                    lockedSupply += safeStod(escrow.getText());
                    return;
                }

                auto rs = PathFindTrustLine::makeItem(*issuerID, sle);
                if (!rs)
                    return;

                auto const& bal = rs->getBalance();
                if (bal.get<Issue>().currency != currency)
                    return;

                ++trustLines;

                int const balSign = bal.signum();
                if (balSign == 0)
                    return;

                if (balSign < 0)
                {
                    double const amount = safeStod((-bal).getText());
                    ++holders;
                    if (rs->getFreeze())
                        frozenSupply += amount;
                    else
                        supply += amount;
                }
            });

        dex::TokenInfoValue tiv;
        tiv.set(supply, frozenSupply, lockedSupply, holders, trustLines,
                ledgerSeq);
        MDB_val tiKey = {
            tokenKey.size(), const_cast<char*>(tokenKey.data())};
        auto tival = tiv.val();
        store.put(txn, DexDB::TokenInfo, &tiKey, &tival);

        JLOG(journal.info())
            << "DEXTimeSeries: bootstrapped token info for " << tokenKey
            << " supply=" << supply << " holders=" << holders
            << " trustLines=" << trustLines;
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
    std::thread bootstrapThread_;

    std::mutex cvMutex_;
    std::condition_variable firstIndexCv_;
    std::atomic<bool> firstIndexReceived_{false};

    std::mutex bootstrapMutex_;
    std::condition_variable bootstrapCv_;
    std::deque<std::string> bootstrapQueue_;
    std::set<std::string> knownBooks_;

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
        {
            JLOG(journal_.info()) << "DEXTimeSeries: closing writer";
            store_->close();
        }
    }

    void
    start() override
    {
        if (!store_ || !store_->isOpen())
        {
            JLOG(journal_.warn())
                << "DEXTimeSeries: start() called but store not open";
            return;
        }
        if (config_.startSequence == "0")
        {
            JLOG(journal_.info())
                << "DEXTimeSeries: start_sequence=0, backfill disabled";
            return;
        }
        JLOG(journal_.info())
            << "DEXTimeSeries: starting backfill thread"
            << " start_sequence=" << config_.startSequence;
        running_ = true;
        backfillThread_ = std::thread([this] { doBackfill(); });

        if (!config_.bootstrapUrl.empty())
        {
            JLOG(journal_.info())
                << "DEXTimeSeries: bootstrap enabled from "
                << config_.bootstrapUrl;
            bootstrapThread_ = std::thread([this] { doBootstrapLoop(); });
        }
    }

    void
    stop() override
    {
        if (!running_.exchange(false))
            return;
        JLOG(journal_.info()) << "DEXTimeSeries: stopping threads";
        {
            std::lock_guard lk(cvMutex_);
            firstIndexCv_.notify_one();
        }
        {
            std::lock_guard lk(bootstrapMutex_);
            bootstrapCv_.notify_one();
        }
        if (backfillThread_.joinable())
            backfillThread_.join();
        if (bootstrapThread_.joinable())
            bootstrapThread_.join();
        JLOG(journal_.info()) << "DEXTimeSeries: threads stopped";
    }

    void
    index(std::shared_ptr<ReadView const> const& ledger) override
    {
        if (!store_ || !store_->isOpen())
            return;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn) != 0)
        {
            JLOG(journal_.error())
                << "DEXTimeSeries: index() failed to begin txn for ledger "
                << ledger->header().seq;
            return;
        }

        std::vector<std::string> newBooks;
        try
        {
            indexLedgerImpl(*store_, txn, ledger, config_, journal_);

            if (!config_.bootstrapUrl.empty())
            {
                Json::Value const bookChanges =
                    RPC::computeBookChanges(ledger);
                auto const& changes = bookChanges[jss::changes];
                for (Json::UInt i = 0; i < changes.size(); ++i)
                {
                    auto const& change = changes[i];
                    std::string bk;
                    if (change.isMember(jss::currency_a))
                        bk = change[jss::currency_a].asString();
                    else if (change.isMember("mpt_issuance_id_a"))
                        bk = change["mpt_issuance_id_a"].asString();
                    bk += "|";
                    if (change.isMember(jss::currency_b))
                        bk += change[jss::currency_b].asString();
                    else if (change.isMember("mpt_issuance_id_b"))
                        bk += change["mpt_issuance_id_b"].asString();

                    std::lock_guard lk(bootstrapMutex_);
                    if (knownBooks_.insert(bk).second)
                        newBooks.push_back(bk);
                }
            }

            mdb_txn_commit(txn);
        }
        catch (std::exception const& e)
        {
            mdb_txn_abort(txn);
            JLOG(journal_.warn())
                << "DEXTimeSeries: index error: " << e.what();
            return;
        }

        for (auto const& bk : newBooks)
            enqueueBootstrap(bk);

        if (!firstIndexReceived_.exchange(true))
        {
            std::lock_guard lk(cvMutex_);
            firstIndexCv_.notify_one();
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
        {
            int rc = mdb_env_sync(store_->env(), 1);
            if (rc != 0)
            {
                JLOG(journal_.warn())
                    << "DEXTimeSeries: flush (mdb_env_sync) failed: "
                    << mdb_strerror(rc);
            }
            else
            {
                JLOG(journal_.trace()) << "DEXTimeSeries: flushed to disk";
            }
        }
    }

    DEXTimeSeriesStore*
    getStore() override
    {
        return store_.get();
    }

private:
    void
    enqueueBootstrap(std::string const& bookKey)
    {
        std::string metaKey = "bs:" + bookKey;
        auto val = store_->getMeta(metaKey);
        if (val && *val == "1")
            return;

        {
            std::lock_guard lk(bootstrapMutex_);
            bootstrapQueue_.push_back(bookKey);
        }
        bootstrapCv_.notify_one();

        JLOG(journal_.debug())
            << "DEXTimeSeries: queued bootstrap for " << bookKey;
    }

    void
    doBootstrapLoop()
    {
        JLOG(journal_.info()) << "DEXTimeSeries: bootstrap thread started";

        while (running_)
        {
            std::string bookKey;
            {
                std::unique_lock lk(bootstrapMutex_);
                bootstrapCv_.wait(lk, [this] {
                    return !bootstrapQueue_.empty() || !running_;
                });
                if (!running_)
                    break;
                bookKey = std::move(bootstrapQueue_.front());
                bootstrapQueue_.pop_front();
            }

            try
            {
                bootstrapBookFromApi(
                    *store_, config_.bootstrapUrl, bookKey, journal_);
            }
            catch (std::exception const& e)
            {
                JLOG(journal_.warn())
                    << "DEXTimeSeries: bootstrap error for " << bookKey
                    << ": " << e.what();
            }
        }

        JLOG(journal_.info()) << "DEXTimeSeries: bootstrap thread stopped";
    }

    void
    doBackfill()
    {
        JLOG(journal_.info())
            << "DEXTimeSeries: backfill thread waiting for first indexed ledger";

        {
            std::unique_lock lk(cvMutex_);
            firstIndexCv_.wait(lk, [this] {
                return firstIndexReceived_.load() || !running_;
            });
        }

        if (!running_ || !store_ || !store_->isOpen())
            return;

        if (config_.startSequence == "0")
        {
            JLOG(journal_.info())
                << "DEXTimeSeries: start_sequence=0, no backfill";
            return;
        }

        auto& lm = app_.getLedgerMaster();

        uint32_t cursor = 0;
        {
            auto stored = store_->getMeta("backfill_cursor");
            if (stored && !stored->empty())
                cursor = static_cast<uint32_t>(std::stoul(*stored));
        }

        if (cursor == 0)
        {
            while (running_)
            {
                auto const validated = lm.getValidatedLedger();
                if (validated)
                {
                    cursor = validated->header().seq - 1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        uint32_t const targetFloor = (config_.startSequence == "full")
            ? lm.getEarliestFetch()
            : static_cast<uint32_t>(std::stoul(config_.startSequence));

        JLOG(journal_.info())
            << "DEXTimeSeries: backfill started — cursor=" << cursor
            << ", floor=" << targetFloor;

        uint32_t indexed = 0;
        uint32_t waited = 0;

        while (running_ && cursor >= targetFloor)
        {
            auto ledger = lm.getLedgerBySeq(cursor);
            if (!ledger)
            {
                if (waited == 0)
                {
                    MDB_txn* txn = nullptr;
                    if (store_->beginTxn(&txn) == 0)
                    {
                        store_->setMeta(txn, "backfill_status", "waiting");
                        store_->setMeta(
                            txn,
                            "backfill_cursor",
                            std::to_string(cursor));
                        mdb_txn_commit(txn);
                    }
                }
                ++waited;
                std::this_thread::sleep_for(std::chrono::seconds(10));
                continue;
            }

            waited = 0;

            MDB_txn* txn = nullptr;
            if (store_->beginTxn(&txn) != 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

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
                    << "DEXTimeSeries: backfill error at seq " << cursor
                    << ": " << e.what();
            }

            if (cursor == 0)
                break;
            --cursor;

            if (indexed % 1000 == 0)
            {
                JLOG(journal_.info())
                    << "DEXTimeSeries: backfill cursor=" << cursor
                    << " indexed=" << indexed;

                MDB_txn* mtxn = nullptr;
                if (store_->beginTxn(&mtxn) == 0)
                {
                    store_->setMeta(mtxn, "backfill_status", "running");
                    store_->setMeta(
                        mtxn,
                        "backfill_cursor",
                        std::to_string(cursor));
                    mdb_txn_commit(mtxn);
                }
            }

            if (indexed % config_.backfillBatchSize == 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.backfillPauseMs));
        }

        if (running_)
        {
            MDB_txn* txn = nullptr;
            if (store_->beginTxn(&txn) == 0)
            {
                store_->setMeta(txn, "backfill_status", "complete");
                store_->setMeta(
                    txn, "backfill_cursor", std::to_string(cursor));
                mdb_txn_commit(txn);
            }
            JLOG(journal_.info())
                << "DEXTimeSeries: backfill complete — indexed=" << indexed
                << ", cursor=" << cursor;
        }
    }

    void
    purgeOldData(uint32_t now)
    {
        if (!store_ || !store_->isOpen())
            return;

        MDB_txn* txn = nullptr;
        if (store_->beginTxn(&txn) != 0)
        {
            JLOG(journal_.warn())
                << "DEXTimeSeries: purge failed to begin txn";
            return;
        }

        JLOG(journal_.debug())
            << "DEXTimeSeries: starting purge cycle, now=" << now;

        uint32_t tradesPurged = 0;
        uint32_t candlesPurged = 0;
        uint32_t ammPurged = 0;

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
                    bool deleted = false;
                    if (nul && (key.mv_size - (nul - p + 1)) >= 4)
                    {
                        uint32_t ts = dex::decodeBE(nul + 1);
                        if (ts >= cutoff)
                            break;
                        mdb_cursor_del(cursor, 0);
                        ++tradesPurged;
                        deleted = true;
                    }
                    rc = mdb_cursor_get(
                        cursor, &key, &data,
                        deleted ? MDB_GET_CURRENT : MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
            else
            {
                JLOG(journal_.warn())
                    << "DEXTimeSeries: purge failed to open Trades cursor";
            }
        }

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
                    bool deleted = false;
                    if (nul && (key.mv_size - (nul - p + 1)) >= 5)
                    {
                        uint8_t iv = *(nul + 1);
                        uint32_t ts = dex::decodeBE(nul + 2);
                        if (iv == interval && ts < cutoff)
                        {
                            mdb_cursor_del(cursor, 0);
                            ++candlesPurged;
                            deleted = true;
                        }
                    }
                    rc = mdb_cursor_get(
                        cursor, &key, &data,
                        deleted ? MDB_GET_CURRENT : MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
            else
            {
                JLOG(journal_.warn())
                    << "DEXTimeSeries: purge failed to open Candles cursor"
                    << " interval=" << int(interval);
            }
        };

        purgeCandles(0, config_.candle1mRetentionHours);
        purgeCandles(1, config_.candle5mRetentionHours);
        purgeCandles(2, config_.candle1hRetentionHours);
        purgeCandles(3, config_.candle1dRetentionHours);

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
                    bool deleted = false;
                    if (data.mv_size >= AMMSnapshotValue::kSize)
                    {
                        auto sv =
                            AMMSnapshotValue::fromData(data.mv_data);
                        if (sv.timestamp() < cutoff)
                        {
                            mdb_cursor_del(cursor, 0);
                            ++ammPurged;
                            deleted = true;
                        }
                    }
                    rc = mdb_cursor_get(
                        cursor, &key, &data,
                        deleted ? MDB_GET_CURRENT : MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
            else
            {
                JLOG(journal_.warn())
                    << "DEXTimeSeries: purge failed to open AMMSnapshots cursor";
            }
        }

        int rc = mdb_txn_commit(txn);
        if (rc != 0)
        {
            JLOG(journal_.error())
                << "DEXTimeSeries: purge commit failed: " << mdb_strerror(rc);
        }
        else
        {
            JLOG(journal_.info())
                << "DEXTimeSeries: purge complete — trades=" << tradesPurged
                << " candles=" << candlesPurged
                << " amm_snapshots=" << ammPurged;
        }
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
