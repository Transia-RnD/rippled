#include <xrpld/app/inner/InnerLedgerMaster.h>

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/LedgerTiming.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/shamap/SHAMap.h>

#include <algorithm>

namespace xrpl {
namespace inner {

InnerLedgerMaster::InnerLedgerMaster(Family& family, beast::Journal journal)
    : family_(family), j_(journal)
{
}

std::shared_ptr<InnerLedger const>
InnerLedgerMaster::getClosedLedger() const
{
    std::lock_guard lock(mutable_mutex_);
    return closedLedger_;
}

std::shared_ptr<InnerLedger const>
InnerLedgerMaster::getValidatedLedger() const
{
    std::lock_guard lock(mutable_mutex_);
    return validatedLedger_;
}

std::uint32_t
InnerLedgerMaster::getValidLedgerIndex() const
{
    std::lock_guard lock(mutable_mutex_);
    if (validatedLedger_)
        return validatedLedger_->header().seq;
    return 0;
}

bool
InnerLedgerMaster::haveLedger(uint256 const& hash) const
{
    std::lock_guard lock(mutable_mutex_);
    for (auto const& l : history_)
    {
        if (l->header().hash == hash)
            return true;
    }
    return false;
}

std::optional<InnerCxLedger>
InnerLedgerMaster::getLedger(uint256 const& hash) const
{
    std::lock_guard lock(mutable_mutex_);
    for (auto const& l : history_)
    {
        if (l->header().hash == hash)
            return InnerCxLedger(l);
    }
    return std::nullopt;
}

std::shared_ptr<InnerLedger const>
InnerLedgerMaster::getLedgerBySeq(std::uint32_t seq) const
{
    std::lock_guard lock(mutable_mutex_);
    if (validatedLedger_ && validatedLedger_->header().seq == seq)
        return validatedLedger_;
    if (closedLedger_ && closedLedger_->header().seq == seq)
        return closedLedger_;
    for (auto const& l : history_)
    {
        if (l->header().seq == seq)
            return l;
    }
    return nullptr;
}

void
InnerLedgerMaster::storeLedger(std::shared_ptr<InnerLedger const> ledger)
{
    std::lock_guard lock(mutable_mutex_);
    history_.push_back(std::move(ledger));
    prune();
}

void
InnerLedgerMaster::setValidated(std::shared_ptr<InnerLedger const> ledger)
{
    std::lock_guard lock(mutable_mutex_);
    validatedLedger_ = std::move(ledger);
}

void
InnerLedgerMaster::setClosed(std::shared_ptr<InnerLedger const> ledger)
{
    std::lock_guard lock(mutable_mutex_);
    closedLedger_ = std::move(ledger);
}

std::uint32_t
InnerLedgerMaster::getLastCheckpointSeq() const
{
    std::lock_guard lock(mutable_mutex_);
    return lastCheckpointOuterSeq_;
}

uint256
InnerLedgerMaster::getLastCheckpointHash() const
{
    std::lock_guard lock(mutable_mutex_);
    return lastCheckpointOuterHash_;
}

void
InnerLedgerMaster::setCheckpoint(
    std::uint32_t outerSeq,
    uint256 const& outerHash)
{
    std::lock_guard lock(mutable_mutex_);
    lastCheckpointOuterSeq_ = outerSeq;
    lastCheckpointOuterHash_ = outerHash;
    lastCheckpointLedger_ = validatedLedger_;
}

bool
InnerLedgerMaster::rollbackToCheckpoint()
{
    std::lock_guard lock(mutable_mutex_);
    if (!lastCheckpointLedger_)
        return false;

    closedLedger_ = lastCheckpointLedger_;
    validatedLedger_ = lastCheckpointLedger_;

    // Remove all ledgers after the checkpoint
    auto const checkpointSeq = lastCheckpointLedger_->header().seq;
    history_.erase(
        std::remove_if(
            history_.begin(),
            history_.end(),
            [checkpointSeq](auto const& l) {
                return l->header().seq > checkpointSeq;
            }),
        history_.end());

    JLOG(j_.warn()) << "InnerChain: rolled back to checkpoint seq "
                    << checkpointSeq;
    return true;
}

std::shared_ptr<InnerLedger>
InnerLedgerMaster::buildGenesisLedger()
{
    InnerLedgerHeader header;
    header.seq = 1;
    header.closeTimeResolution = kLEDGER_GENESIS_TIME_RESOLUTION;

    auto stateMap = std::make_shared<SHAMap>(SHAMapType::STATE, family_);
    auto txMap = std::make_shared<SHAMap>(SHAMapType::TRANSACTION, family_);

    header.stateHash = stateMap->getHash().asUint256();
    header.txHash = txMap->getHash().asUint256();

    // Compute ledger hash from header fields
    header.hash = sha512Half(
        header.stateHash, header.txHash, header.seq);

    auto genesis =
        std::make_shared<InnerLedger>(header, std::move(stateMap), std::move(txMap));

    std::lock_guard lock(mutable_mutex_);
    closedLedger_ = genesis;
    validatedLedger_ = genesis;
    history_.push_back(genesis);

    JLOG(j_.info()) << "InnerChain: genesis ledger created, hash="
                    << to_string(header.hash);
    return genesis;
}

void
InnerLedgerMaster::prune()
{
    while (history_.size() > kMaxRetainedLedgers)
        history_.pop_front();
}

}  // namespace inner
}  // namespace xrpl
