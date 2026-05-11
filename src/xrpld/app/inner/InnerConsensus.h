#pragma once

#include <xrpld/app/inner/InnerCxLedger.h>
#include <xrpld/app/inner/InnerCxPeerPos.h>
#include <xrpld/app/inner/InnerCxTx.h>
#include <xrpld/app/inner/InnerState.h>
#include <xrpld/consensus/Consensus.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/overlay/Overlay.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/core/JobQueue.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/PublicKey.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace xrpl {

class Application;
class STValidation;
class ValidatorKeys;
class ValidatorList;

namespace inner {

struct InnerChainConfig;
class InnerLedgerMaster;

class InnerConsensus
{
public:
    struct WithdrawalInfo
    {
        AccountID destination;
        std::uint64_t amount{0};
        std::uint32_t innerSeq{0};
        std::uint32_t chainId{0};
    };

    using WithdrawalCallback = std::function<void(WithdrawalInfo)>;

private:
    class Adaptor
    {
        Application& app_;
        InnerLedgerMaster& ledgerMaster_;
        InnerTxQueue& txQueue_;
        InnerTransactorRegistry const& registry_;
        beast::Journal const j_;
        ValidatorKeys const& validatorKeys_;

        std::uint64_t const valCookie_;
        std::uint32_t const chainId_;
        ConsensusParms parms_;

        NetClock::time_point lastValidationTime_;

        std::atomic<bool> validating_{false};
        std::atomic<std::size_t> prevProposers_{0};
        std::atomic<std::chrono::milliseconds> prevRoundTime_{
            std::chrono::milliseconds{0}};
        std::atomic<ConsensusMode> mode_{ConsensusMode::Observing};

        std::atomic<std::uint64_t> totalTxApplied_{0};
        std::atomic<std::uint64_t> totalTxFailed_{0};
        std::atomic<std::uint32_t> totalRounds_{0};

        mutable std::mutex valMutex_;
        struct ValEntry
        {
            std::uint32_t seq{0};
            hash_set<NodeID> validators;
        };
        std::map<uint256, ValEntry> validationsPerLedger_;
        std::map<NodeID, std::pair<uint256, std::uint32_t>> latestByNode_;
        std::uint32_t latestValSeq_{0};

        mutable std::mutex txSetMutex_;
        std::map<uint256, InnerTxSet> txSetCache_;

        std::vector<std::string> configValidatorKeys_;

        WithdrawalCallback withdrawalCallback_;

    public:
        using Ledger_t = InnerCxLedger;
        using NodeID_t = NodeID;
        using NodeKey_t = PublicKey;
        using TxSet_t = InnerTxSet;
        using PeerPosition_t = InnerCxPeerPos;

        using Result = ConsensusResult<Adaptor>;

        Adaptor(
            Application& app,
            InnerLedgerMaster& ledgerMaster,
            InnerTxQueue& txQueue,
            InnerTransactorRegistry const& registry,
            ValidatorKeys const& validatorKeys,
            InnerChainConfig const& config,
            beast::Journal journal);

        bool
        validating() const
        {
            return validating_;
        }

        std::size_t
        prevProposers() const
        {
            return prevProposers_;
        }

        std::chrono::milliseconds
        prevRoundTime() const
        {
            return prevRoundTime_;
        }

        ConsensusMode
        mode() const
        {
            return mode_;
        }

        std::uint64_t
        totalTxApplied() const
        {
            return totalTxApplied_;
        }

        std::uint64_t
        totalTxFailed() const
        {
            return totalTxFailed_;
        }

        std::uint32_t
        totalRounds() const
        {
            return totalRounds_;
        }

        void
        setWithdrawalCallback(InnerConsensus::WithdrawalCallback cb)
        {
            withdrawalCallback_ = std::move(cb);
        }

        bool
        preStartRound(
            InnerCxLedger const& prevLedger,
            hash_set<NodeID> const& nowTrusted);

        bool
        haveValidated() const;

        std::uint32_t
        getValidLedgerIndex() const;

        std::pair<std::size_t, hash_set<NodeKey_t>>
        getQuorumKeys() const;

        std::size_t
        laggards(
            InnerCxLedger::Seq const seq,
            hash_set<NodeKey_t>& trustedKeys) const;

        bool
        validator() const;

        void
        updateOperatingMode(std::size_t const positions) const;

        void
        cacheTxSet(InnerTxSet const& txSet);

        std::optional<InnerTxSet>
        getCachedTxSet(uint256 const& id) const;

        ConsensusParms const&
        parms() const
        {
            return parms_;
        }

    private:
        friend class Consensus<Adaptor>;
        friend class InnerConsensus;

        std::optional<InnerCxLedger>
        acquireLedger(uint256 const& hash);

        void
        share(InnerCxPeerPos const& peerPos);

        void
        share(InnerCxTx const& tx);

        std::optional<InnerTxSet>
        acquireTxSet(InnerTxSet::ID const& setId);

        bool
        hasOpenTransactions() const;

        std::size_t
        proposersValidated(uint256 const& h) const;

        std::size_t
        proposersFinished(
            InnerCxLedger const& ledger,
            uint256 const& h) const;

        void
        propose(InnerCxPeerPos::Proposal const& proposal);

        void
        share(InnerTxSet const& txns);

        uint256
        getPrevLedger(
            uint256 ledgerID,
            InnerCxLedger const& ledger,
            ConsensusMode mode);

        void
        onModeChange(ConsensusMode before, ConsensusMode after);

        Result
        onClose(
            InnerCxLedger const& ledger,
            NetClock::time_point const& closeTime,
            ConsensusMode mode);

        void
        onAccept(
            Result const& result,
            InnerCxLedger const& prevLedger,
            NetClock::duration const& closeResolution,
            ConsensusCloseTimes const& rawCloseTimes,
            ConsensusMode const& mode,
            json::Value&& consensusJson,
            bool const validating);

        void
        onForceAccept(
            Result const& result,
            InnerCxLedger const& prevLedger,
            NetClock::duration const& closeResolution,
            ConsensusCloseTimes const& rawCloseTimes,
            ConsensusMode const& mode,
            json::Value&& consensusJson);

        void
        notify(
            protocol::NodeEvent ne,
            InnerCxLedger const& ledger,
            bool haveCorrectLCL);

        void
        doValidation(
            InnerLedgerHeader const& header,
            uint256 const& consensusHash);
    };

public:
    InnerConsensus(
        Application& app,
        InnerLedgerMaster& ledgerMaster,
        InnerTxQueue& txQueue,
        InnerTransactorRegistry const& registry,
        Consensus<Adaptor>::clock_type const& clock,
        ValidatorKeys const& validatorKeys,
        InnerChainConfig const& config,
        beast::Journal journal);

    InnerConsensus(InnerConsensus const&) = delete;
    InnerConsensus& operator=(InnerConsensus const&) = delete;

    bool
    validating() const
    {
        return adaptor_.validating();
    }

    std::size_t
    prevProposers() const
    {
        return adaptor_.prevProposers();
    }

    std::chrono::milliseconds
    prevRoundTime() const
    {
        return adaptor_.prevRoundTime();
    }

    ConsensusMode
    mode() const
    {
        return adaptor_.mode();
    }

    void
    setWithdrawalCallback(WithdrawalCallback cb)
    {
        adaptor_.setWithdrawalCallback(std::move(cb));
    }

    ConsensusPhase
    phase() const
    {
        return consensus_.phase();
    }

    std::uint64_t
    totalTxApplied() const
    {
        return adaptor_.totalTxApplied();
    }

    std::uint64_t
    totalTxFailed() const
    {
        return adaptor_.totalTxFailed();
    }

    std::uint32_t
    totalRounds() const
    {
        return adaptor_.totalRounds();
    }

    json::Value
    getJson(bool full) const;

    void
    startRound(
        NetClock::time_point const& now,
        InnerCxLedger::ID const& prevLgrId,
        InnerCxLedger const& prevLgr,
        hash_set<NodeID> const& nowUntrusted,
        hash_set<NodeID> const& nowTrusted);

    void
    timerEntry(NetClock::time_point const& now);

    void
    gotTxSet(NetClock::time_point const& now, InnerTxSet const& txSet);

    InnerCxLedger::ID
    prevLedgerID() const
    {
        std::scoped_lock const _{mutex_};
        return consensus_.prevLedgerID();
    }

    void
    simulate(
        NetClock::time_point const& now,
        std::optional<std::chrono::milliseconds> consensusDelay);

    bool
    peerProposal(
        NetClock::time_point const& now,
        InnerCxPeerPos const& newProposal);

    void
    handleValidation(std::shared_ptr<STValidation> const& val);

    void
    cacheTxSet(InnerTxSet const& txSet)
    {
        adaptor_.cacheTxSet(txSet);
    }

    void
    startNextRound();

    ConsensusParms const&
    parms() const
    {
        return adaptor_.parms();
    }

private:
    mutable std::recursive_mutex mutex_;
    Adaptor adaptor_;
    Consensus<Adaptor> consensus_;
};

}  // namespace inner
}  // namespace xrpl
