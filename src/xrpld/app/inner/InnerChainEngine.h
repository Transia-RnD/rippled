#pragma once

#include <xrpld/app/inner/InnerConsensus.h>
#include <xrpld/app/inner/InnerCxLedger.h>
#include <xrpld/app/inner/InnerLedgerMaster.h>
#include <xrpld/app/inner/InnerState.h>
#include <xrpld/app/misc/ValidatorList.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/core/JobQueue.h>
#include <xrpl/protocol/messages.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xrpl {

class Application;
class Config;
class ValidatorKeys;

namespace inner {

struct InnerChainConfig
{
    bool enabled{false};
    std::string name;
    std::uint32_t networkId{0};
    std::chrono::milliseconds ledgerCloseMs{1000};
    std::chrono::milliseconds granularity{500};
    std::uint32_t stateRootInterval{10};
    std::string nodeDbPath;

    std::vector<std::string> validatorKeys;
    std::vector<std::string> validatorListKeys;
};

std::vector<InnerChainConfig>
parseInnerChainConfigs(Config const& config);

// Backward compat — returns first enabled chain config or disabled default
InnerChainConfig
parseInnerChainConfig(Config const& config);

class InnerChainEngine
{
public:
    InnerChainEngine(
        Application& app,
        InnerChainConfig config,
        Stopwatch const& clock,
        ValidatorKeys const& validatorKeys,
        beast::Journal journal);

    ~InnerChainEngine();

    InnerChainEngine(InnerChainEngine const&) = delete;
    InnerChainEngine& operator=(InnerChainEngine const&) = delete;

    bool
    start();

    void
    stop();

    bool
    beginConsensus(uint256 const& networkClosed);

    void
    timerEntry();

    void
    onPeerProposal(
        NetClock::time_point const& now,
        std::shared_ptr<protocol::TMProposeSet> const& m);

    void
    onPeerValidation(
        std::shared_ptr<protocol::TMValidation> const& m);

    void
    onPeerTransaction(
        std::shared_ptr<protocol::TMTransaction> const& m);

    InnerChainConfig const&
    config() const
    {
        return config_;
    }

    std::uint32_t
    networkId() const
    {
        return config_.networkId;
    }

    InnerLedgerMaster&
    ledgerMaster()
    {
        return ledgerMaster_;
    }

    InnerLedgerMaster const&
    ledgerMaster() const
    {
        return ledgerMaster_;
    }

    InnerConsensus&
    consensus()
    {
        return *consensus_;
    }

    ValidatorList&
    validators()
    {
        return *validators_;
    }

    bool
    isRunning() const
    {
        return running_;
    }

    struct StateRootInfo
    {
        uint256 stateRoot;
        std::uint32_t innerSeq{0};
        std::uint32_t chainId{0};
    };

    struct WithdrawalRecord
    {
        AccountID destination;
        std::uint64_t amount{0};
        std::uint32_t innerSeq{0};
        std::uint32_t chainId{0};
    };

    std::optional<StateRootInfo>
    getStateRootForCommit(std::uint32_t outerSeq) const;

    void
    queueWithdrawal(WithdrawalRecord record);

    std::vector<WithdrawalRecord>
    drainWithdrawalsForCommit(std::uint32_t outerSeq);

    bool
    submit(InnerTx tx);

    hash_set<NodeID>
    getTrustedNodeIDs() const;

    std::optional<InnerAccountFields>
    getAccount(AccountID const& id) const;

    bool
    creditAccount(AccountID const& dest, std::uint64_t amount);

    json::Value
    getJson(bool full) const;

private:
    void
    handleTxSetMessage(Slice data);

    void
    setHeartbeatTimer();

    void
    processHeartbeat();

    std::string
    tag() const;

    Application& app_;
    InnerChainConfig config_;
    beast::Journal j_;

    ManifestCache validatorManifests_;
    ManifestCache publisherManifests_;
    std::unique_ptr<ValidatorList> validators_;

    InnerLedgerMaster ledgerMaster_;
    InnerTxQueue txQueue_;
    InnerTransactorRegistry registry_;
    std::unique_ptr<InnerConsensus> consensus_;

    boost::asio::steady_timer heartbeatTimer_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point startTime_;

    mutable std::mutex withdrawalMutex_;
    std::vector<WithdrawalRecord> pendingWithdrawals_;
};

class InnerChainRegistry
{
public:
    InnerChainRegistry() = default;

    InnerChainRegistry(InnerChainRegistry const&) = delete;
    InnerChainRegistry& operator=(InnerChainRegistry const&) = delete;

    void
    add(std::unique_ptr<InnerChainEngine> engine);

    InnerChainEngine*
    get(std::uint32_t networkId);

    InnerChainEngine const*
    get(std::uint32_t networkId) const;

    InnerChainEngine*
    first();

    InnerChainEngine const*
    first() const;

    std::size_t
    size() const
    {
        return engines_.size();
    }

    bool
    empty() const
    {
        return engines_.empty();
    }

    bool
    startAll();

    void
    stopAll();

    template <typename Fn>
    void
    forEach(Fn&& fn)
    {
        for (auto& [id, engine] : engines_)
            fn(*engine);
    }

    template <typename Fn>
    void
    forEach(Fn&& fn) const
    {
        for (auto const& [id, engine] : engines_)
            fn(*engine);
    }

    json::Value
    getJson(bool full) const;

private:
    std::map<std::uint32_t, std::unique_ptr<InnerChainEngine>> engines_;
    std::vector<std::uint32_t> order_;
};

}  // namespace inner
}  // namespace xrpl
