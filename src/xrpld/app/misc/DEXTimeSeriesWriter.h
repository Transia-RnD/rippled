#pragma once

#include <xrpl/basics/BasicConfig.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/ReadView.h>

#include <cstdint>
#include <memory>
#include <string>

namespace xrpl {

class Application;
class DEXTimeSeriesStore;

struct DEXTimeSeriesConfig
{
    bool enabled = false;
    std::string startSequence = "0";  // "0", "full", or ledger seq
    int32_t tickRetentionHours = 24;
    int32_t candle1mRetentionHours = 168;
    int32_t candle5mRetentionHours = 720;
    int32_t candle1hRetentionHours = 8760;
    int32_t candle1dRetentionHours = 0;  // keep forever
    int32_t ammSnapshotRetentionHours = 8760;
    uint32_t backfillBatchSize = 500;
    uint32_t backfillPauseMs = 10;
    uint32_t purgeIntervalLedgers = 1000;
    uint64_t mapSizeGB = 40;
    std::string dbPath = "dex_timeseries";
};

DEXTimeSeriesConfig
parseDEXTimeSeriesConfig(Section const& section);

class DEXTimeSeriesWriter
{
public:
    virtual ~DEXTimeSeriesWriter() = default;

    virtual bool
    open(std::string const& dataDir) = 0;

    virtual void
    close() = 0;

    virtual void
    start() = 0;

    virtual void
    stop() = 0;

    virtual void
    index(std::shared_ptr<ReadView const> const& ledger) = 0;

    virtual void
    flush() = 0;

    virtual DEXTimeSeriesStore*
    getStore() = 0;
};

std::unique_ptr<DEXTimeSeriesWriter>
make_DEXTimeSeriesWriter(
    DEXTimeSeriesConfig const& config,
    Application& app,
    beast::Journal journal);

}  // namespace xrpl
