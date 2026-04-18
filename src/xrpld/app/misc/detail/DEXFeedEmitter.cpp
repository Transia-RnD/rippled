#include <xrpld/app/misc/DEXFeedEmitter.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/DEXTimeSeriesReader.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/rpc/BookChanges.h>

#include <xrpl/basics/BasicConfig.h>
#include <xrpl/basics/Log.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/write.hpp>

#include <filesystem>
#include <mutex>
#include <vector>

namespace xrpl {

DEXFeedConfig
parseDEXFeedConfig(Section const& section)
{
    DEXFeedConfig cfg;
    set(cfg.enabled, "enabled", section);
    cfg.socketPath = get<std::string>(section, "socket_path", "");
    set(cfg.includeAMMState, "include_amm_state", section);
    return cfg;
}

namespace {

Json::Value
extractAMMState(std::shared_ptr<ReadView const> const& ledger)
{
    Json::Value ammArray(Json::arrayValue);

    for (auto const& tx : ledger->txs)
    {
        if (!tx.first || !tx.second)
            continue;

        for (auto const& node : tx.second->getFieldArray(sfAffectedNodes))
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

            Json::Value entry(Json::objectValue);

            if (ff.isFieldPresent(sfAccount))
                entry["account"] = toBase58(ff.getAccountID(sfAccount));

            if (ff.isFieldPresent(sfLPTokenBalance))
                entry["lpt_balance"] =
                    ff.getFieldAmount(sfLPTokenBalance).getJson(JsonOptions::none);

            if (ff.isFieldPresent(sfTradingFee))
                entry["trading_fee"] = ff.getFieldU16(sfTradingFee);

            if (node.isFieldPresent(sfPreviousFields))
            {
                auto const& pfBase = node.peekAtField(sfPreviousFields);
                auto const& pf = pfBase.downcast<STObject>();

                if (pf.isFieldPresent(sfLPTokenBalance))
                    entry["prev_lpt_balance"] =
                        pf.getFieldAmount(sfLPTokenBalance).getJson(JsonOptions::none);
            }

            ammArray.append(entry);
        }
    }

    return ammArray;
}

class DEXFeedEmitterImpl final : public DEXFeedEmitter
{
    DEXFeedConfig const config_;
    Application& app_;
    beast::Journal const journal_;

    boost::asio::io_context ioc_;
    boost::asio::local::stream_protocol::acceptor acceptor_;

    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<boost::asio::local::stream_protocol::socket>>
        clients_;

    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::thread backfillThread_;

    std::string
    buildMessage(std::shared_ptr<ReadView const> const& ledger)
    {
        Json::Value jv = RPC::computeBookChanges(ledger);

        if (config_.includeAMMState)
        {
            Json::Value ammState = extractAMMState(ledger);
            if (ammState.size() > 0)
                jv["amm_changes"] = std::move(ammState);
        }

        Json::FastWriter writer;
        return writer.write(jv);
    }

    void
    doAccept()
    {
        while (running_)
        {
            try
            {
                auto sock = std::make_shared<
                    boost::asio::local::stream_protocol::socket>(ioc_);
                acceptor_.accept(*sock);

                struct timeval tv;
                tv.tv_sec = 5;
                tv.tv_usec = 0;
                setsockopt(
                    sock->native_handle(),
                    SOL_SOCKET,
                    SO_SNDTIMEO,
                    &tv,
                    sizeof(tv));

                std::lock_guard const lock(clientsMutex_);
                clients_.push_back(std::move(sock));

                JLOG(journal_.info())
                    << "DEXFeed: client connected, total="
                    << clients_.size();
            }
            catch (boost::system::system_error const& e)
            {
                if (running_)
                {
                    JLOG(journal_.warn())
                        << "DEXFeed: accept error: " << e.what();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
    }

    void
    broadcast(std::string const& msg)
    {
        decltype(clients_) snapshot;
        {
            std::lock_guard const lock(clientsMutex_);
            snapshot = clients_;
        }

        std::vector<std::shared_ptr<boost::asio::local::stream_protocol::socket>>
            dead;

        for (auto const& sock : snapshot)
        {
            try
            {
                boost::asio::write(*sock, boost::asio::buffer(msg));
            }
            catch (boost::system::system_error const&)
            {
                dead.push_back(sock);
            }
        }

        if (!dead.empty())
        {
            std::lock_guard const lock(clientsMutex_);
            for (auto const& d : dead)
            {
                auto it = std::find(clients_.begin(), clients_.end(), d);
                if (it != clients_.end())
                {
                    JLOG(journal_.debug()) << "DEXFeed: client disconnected";
                    clients_.erase(it);
                }
            }
        }
    }

    void
    doBackfill()
    {
        // Wait for the node to sync
        std::this_thread::sleep_for(std::chrono::seconds(10));

        auto& reader = app_.getDEXTimeSeriesReader();
        auto lastIndexed = reader.getLastIndexedSeq();
        if (!lastIndexed)
            return;

        auto& lm = app_.getLedgerMaster();
        auto const validated = lm.getValidatedLedger();
        if (!validated)
            return;

        uint32_t const validatedSeq = validated->header().seq;
        uint32_t const startSeq = *lastIndexed + 1;

        if (startSeq >= validatedSeq)
            return;

        uint32_t const gap = validatedSeq - startSeq;
        JLOG(journal_.info())
            << "DEXFeed: backfilling " << gap << " ledgers ("
            << startSeq << " -> " << validatedSeq << ")";

        for (uint32_t seq = startSeq; seq <= validatedSeq && running_; ++seq)
        {
            // Wait for at least one client before sending backfill data
            {
                std::lock_guard const lock(clientsMutex_);
                if (clients_.empty())
                {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    --seq;
                    continue;
                }
            }

            auto ledger = lm.getLedgerBySeq(seq);
            if (!ledger)
            {
                JLOG(journal_.debug())
                    << "DEXFeed: ledger " << seq << " not available, skipping backfill";
                break;
            }

            std::string const msg = buildMessage(ledger);
            broadcast(msg);

            if (seq % 1000 == 0)
            {
                JLOG(journal_.info())
                    << "DEXFeed: backfilled to ledger " << seq
                    << " (" << (validatedSeq - seq) << " remaining)";
            }

            // Rate limit to avoid overwhelming the sidecar
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        JLOG(journal_.info()) << "DEXFeed: backfill complete";
    }

public:
    DEXFeedEmitterImpl(
        DEXFeedConfig const& config,
        Application& app,
        beast::Journal journal)
        : config_(config)
        , app_(app)
        , journal_(journal)
        , acceptor_(ioc_)
    {
    }

    ~DEXFeedEmitterImpl() override
    {
        stop();
    }

    void
    start() override
    {
        if (config_.socketPath.empty())
        {
            JLOG(journal_.error())
                << "DEXFeed: socket_path not configured";
            return;
        }

        std::error_code ec;
        std::filesystem::remove(config_.socketPath, ec);

        boost::asio::local::stream_protocol::endpoint ep(config_.socketPath);
        acceptor_.open(ep.protocol());
        acceptor_.bind(ep);
        acceptor_.listen();

        running_ = true;
        acceptThread_ = std::thread([this] { doAccept(); });
        backfillThread_ = std::thread([this] { doBackfill(); });

        JLOG(journal_.info())
            << "DEXFeed: listening on " << config_.socketPath;
    }

    void
    stop() override
    {
        if (!running_.exchange(false))
            return;

        boost::system::error_code ec;
        acceptor_.close(ec);

        if (acceptThread_.joinable())
            acceptThread_.join();
        if (backfillThread_.joinable())
            backfillThread_.join();

        {
            std::lock_guard const lock(clientsMutex_);
            clients_.clear();
        }

        std::error_code fsEc;
        std::filesystem::remove(config_.socketPath, fsEc);

        JLOG(journal_.info()) << "DEXFeed: stopped";
    }

    void
    emit(std::shared_ptr<ReadView const> const& ledger) override
    {
        {
            std::lock_guard const lock(clientsMutex_);
            if (clients_.empty())
                return;
        }

        broadcast(buildMessage(ledger));
    }
};

class DEXFeedEmitterNoop final : public DEXFeedEmitter
{
public:
    void start() override {}
    void stop() override {}
    void emit(std::shared_ptr<ReadView const> const&) override {}
};

}  // namespace

std::unique_ptr<DEXFeedEmitter>
make_DEXFeedEmitter(
    DEXFeedConfig const& config,
    Application& app,
    beast::Journal journal)
{
    if (!config.enabled)
        return std::make_unique<DEXFeedEmitterNoop>();
    return std::make_unique<DEXFeedEmitterImpl>(config, app, journal);
}

}  // namespace xrpl
