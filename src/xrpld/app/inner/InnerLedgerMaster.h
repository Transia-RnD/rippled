#pragma once

#include <xrpld/app/inner/InnerCxLedger.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/shamap/Family.h>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace xrpl {
namespace inner {

class InnerLedgerMaster
{
public:
    static constexpr std::size_t kMaxRetainedLedgers = 256;

    InnerLedgerMaster(Family& family, beast::Journal journal);

    std::shared_ptr<InnerLedger const>
    getClosedLedger() const;

    std::shared_ptr<InnerLedger const>
    getValidatedLedger() const;

    std::uint32_t
    getValidLedgerIndex() const;

    bool
    haveLedger(uint256 const& hash) const;

    std::optional<InnerCxLedger>
    getLedger(uint256 const& hash) const;

    std::shared_ptr<InnerLedger const>
    getLedgerBySeq(std::uint32_t seq) const;

    void
    storeLedger(std::shared_ptr<InnerLedger const> ledger);

    void
    setValidated(std::shared_ptr<InnerLedger const> ledger);

    void
    setClosed(std::shared_ptr<InnerLedger const> ledger);

    // Checkpoint management
    std::uint32_t
    getLastCheckpointSeq() const;

    uint256
    getLastCheckpointHash() const;

    void
    setCheckpoint(std::uint32_t outerSeq, uint256 const& outerHash);

    bool
    rollbackToCheckpoint();

    // Genesis
    std::shared_ptr<InnerLedger>
    buildGenesisLedger();

private:
    void
    prune();

    mutable std::mutex mutable_mutex_;
    Family& family_;
    beast::Journal j_;

    std::shared_ptr<InnerLedger const> closedLedger_;
    std::shared_ptr<InnerLedger const> validatedLedger_;
    std::deque<std::shared_ptr<InnerLedger const>> history_;

    std::uint32_t lastCheckpointOuterSeq_{0};
    uint256 lastCheckpointOuterHash_;
    std::shared_ptr<InnerLedger const> lastCheckpointLedger_;
};

}  // namespace inner
}  // namespace xrpl
