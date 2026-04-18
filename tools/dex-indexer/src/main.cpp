#include "FeedConsumer.h"
#include "OHLCVAggregator.h"
#include "TimeSeriesStore.h"

#include <boost/program_options.hpp>

#include <csignal>
#include <iostream>

namespace po = boost::program_options;

static std::atomic<bool> g_running{true};

static void
signalHandler(int)
{
    g_running = false;
}

int
main(int argc, char* argv[])
{
    po::options_description desc("xrpld-dex-indexer options");
    desc.add_options()
        ("help,h", "Show help")
        ("socket,s", po::value<std::string>()->default_value("/var/run/xrpld/dex_feed.sock"),
            "Path to xrpld DEX feed Unix socket")
        ("db-path,d", po::value<std::string>()->default_value("./dex_timeseries"),
            "RocksDB storage directory")
        ("ticks-max-mb", po::value<uint64_t>()->default_value(512),
            "Max storage for raw ticks (MB)")
    ;

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    }
    catch (std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    if (vm.count("help"))
    {
        std::cout << desc << "\n";
        return 0;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    dex::StoreConfig storeCfg;
    storeCfg.dbPath = vm["db-path"].as<std::string>();
    storeCfg.ticksMaxBytes = vm["ticks-max-mb"].as<uint64_t>() * 1024 * 1024;

    dex::TimeSeriesStore store(storeCfg);
    if (!store.open())
    {
        std::cerr << "Failed to open RocksDB at " << storeCfg.dbPath << "\n";
        return 1;
    }

    auto const lastSeq = store.getLastIndexedSeq();
    std::cout << "Store opened. Last indexed seq: "
              << (lastSeq ? std::to_string(*lastSeq) : "none") << "\n";

    dex::OHLCVAggregator aggregator(store);

    std::string const socketPath = vm["socket"].as<std::string>();
    dex::FeedConsumer consumer(socketPath);
    consumer.setCallback(
        [&](uint32_t ledgerSeq,
            uint32_t ledgerTime,
            std::vector<dex::Tick> const& ticks,
            std::vector<dex::AMMSnapshot> const& ammChanges) {
            aggregator.processTicks(ledgerSeq, ledgerTime, ticks);
            aggregator.processAMMChanges(ledgerSeq, ledgerTime, ammChanges);

            if (ledgerSeq % 100 == 0)
            {
                std::cout << "Indexed ledger " << ledgerSeq
                          << " (" << ticks.size() << " trades, "
                          << ammChanges.size() << " AMM changes)\n";
            }
        });

    consumer.start();

    std::cout << "xrpld-dex-indexer running.\n"
              << "  Feed socket: " << socketPath << "\n"
              << "  DB: " << storeCfg.dbPath << "\n"
              << "  Queries served via rippled RPC: dex_candles, dex_ticks, dex_amm_history\n";

    while (g_running)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "\nShutting down...\n";
    consumer.stop();
    aggregator.flush();
    store.close();

    std::cout << "Done.\n";
    return 0;
}
