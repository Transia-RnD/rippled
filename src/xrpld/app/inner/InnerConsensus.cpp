#include <xrpld/app/inner/InnerConsensus.h>
#include <xrpld/app/inner/InnerChainEngine.h>
#include <xrpld/app/inner/InnerLedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/core/TimeKeeper.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/overlay/Peer.h>

#include <xrpl/basics/Log.h>
#include <xrpl/core/HashRouter.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/messages.h>
#include <xrpl/shamap/SHAMapItem.h>
#include <xrpl/shamap/SHAMapTreeNode.h>

namespace xrpl {
namespace inner {

namespace {
std::string
ctag(std::uint32_t chainId)
{
    return "InnerConsensus[" + std::to_string(chainId) + "] ";
}
}  // namespace

// --- Adaptor Implementation ---

InnerConsensus::Adaptor::Adaptor(
    Application& app,
    InnerLedgerMaster& ledgerMaster,
    InnerTxQueue& txQueue,
    InnerTransactorRegistry const& registry,
    ValidatorKeys const& validatorKeys,
    InnerChainConfig const& config,
    beast::Journal journal)
    : app_(app)
    , ledgerMaster_(ledgerMaster)
    , txQueue_(txQueue)
    , registry_(registry)
    , j_(journal)
    , validatorKeys_(validatorKeys)
    , valCookie_(1)
    , chainId_(config.networkId)
{
    parms_.ledgerIDLE_INTERVAL = config.ledgerCloseMs;
    parms_.ledgerMIN_CLOSE = config.granularity;
    parms_.ledgerGRANULARITY =
        std::chrono::milliseconds{config.granularity.count() / 2};
    parms_.ledgerMIN_CONSENSUS =
        std::chrono::milliseconds{config.granularity.count()};
    parms_.ledgerMAX_CONSENSUS = config.ledgerCloseMs * 5;
    parms_.avMIN_CONSENSUS_TIME = config.ledgerCloseMs;

    configValidatorKeys_ = config.validatorKeys;

    JLOG(j_.debug()) << ctag(chainId_) << "adaptor constructed"
                     << ", has_keys=" << validatorKeys_.keys.has_value()
                     << ", config_validators=" << configValidatorKeys_.size()
                     << ", idle_interval="
                     << parms_.ledgerIDLE_INTERVAL.count() << "ms"
                     << ", min_close="
                     << parms_.ledgerMIN_CLOSE.count() << "ms"
                     << ", granularity="
                     << parms_.ledgerGRANULARITY.count() << "ms";
}

bool
InnerConsensus::Adaptor::preStartRound(
    InnerCxLedger const& prevLedger,
    hash_set<NodeID> const& nowTrusted)
{
    validating_ = validatorKeys_.keys.has_value();

    JLOG(j_.debug()) << ctag(chainId_) << "preStartRound"
                     << ", prev_seq=" << prevLedger.seq()
                     << ", prev_id=" << to_string(prevLedger.id())
                     << ", trusted_count=" << nowTrusted.size()
                     << ", validating=" << (validating_ ? "yes" : "no");

    return validating_;
}

bool
InnerConsensus::Adaptor::haveValidated() const
{
    return ledgerMaster_.getValidatedLedger() != nullptr;
}

std::uint32_t
InnerConsensus::Adaptor::getValidLedgerIndex() const
{
    return ledgerMaster_.getValidLedgerIndex();
}

std::pair<std::size_t, hash_set<PublicKey>>
InnerConsensus::Adaptor::getQuorumKeys() const
{
    // Resolve master keys to signing keys via outer chain's manifest cache
    // every call, since peer manifests arrive asynchronously after startup.
    hash_set<PublicKey> signingKeys;
    auto& manifests = app_.getValidatorManifests();
    for (auto const& keyStr : configValidatorKeys_)
    {
        auto const pk =
            parseBase58<PublicKey>(TokenType::NodePublic, keyStr);
        if (pk)
        {
            if (auto const sk = manifests.getSigningKey(*pk))
                signingKeys.insert(*sk);
            else
                signingKeys.insert(*pk);
        }
    }
    auto const quorum = std::max<std::size_t>(
        (signingKeys.size() * 2 + 2) / 3, 1);
    return {quorum, signingKeys};
}

std::size_t
InnerConsensus::Adaptor::laggards(
    InnerCxLedger::Seq const seq,
    hash_set<NodeKey_t>& trustedKeys) const
{
    return 0;
}

bool
InnerConsensus::Adaptor::validator() const
{
    return validatorKeys_.keys.has_value();
}

void
InnerConsensus::Adaptor::updateOperatingMode(
    std::size_t const positions) const
{
}

std::optional<InnerCxLedger>
InnerConsensus::Adaptor::acquireLedger(uint256 const& hash)
{
    JLOG(j_.trace()) << ctag(chainId_) << "acquireLedger"
                     << ", hash=" << to_string(hash);
    return ledgerMaster_.getLedger(hash);
}

void
InnerConsensus::Adaptor::share(InnerCxPeerPos const& peerPos)
{
    auto const& proposal = peerPos.proposal();

    JLOG(j_.trace()) << ctag(chainId_) << "share(peerPos)"
                     << ", pos=" << to_string(proposal.position())
                     << ", prev=" << to_string(proposal.prevLedger())
                     << ", seq=" << proposal.proposeSeq();

    protocol::TMProposeSet prop;
    prop.set_currenttxhash(
        proposal.position().begin(), proposal.position().size());
    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.prevLedger().size());
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());
    prop.set_nodepubkey(
        peerPos.publicKey().data(), peerPos.publicKey().size());
    prop.set_signature(
        peerPos.signature().data(), peerPos.signature().size());
    prop.set_chainid(chainId_);

    app_.getOverlay().broadcast(prop);
}

void
InnerConsensus::Adaptor::share(InnerCxTx const& tx)
{
    auto const& item = tx.tx;
    if (!item)
    {
        JLOG(j_.warn()) << ctag(chainId_) << "share(tx): null SHAMapItem";
        return;
    }

    auto const data = item->slice();

    // Register in HashRouter so we don't re-process our own broadcast
    auto const suppression = sha512Half(data, chainId_);
    app_.getHashRouter().addSuppressionPeerWithStatus(suppression, 0);

    protocol::TMTransaction msg;
    msg.set_rawtransaction(data.data(), data.size());
    msg.set_status(protocol::tsNEW);
    msg.set_chainid(chainId_);

    auto const sm = std::make_shared<Message>(
        msg, protocol::mtTRANSACTION);
    app_.getOverlay().foreach(
        [&sm](std::shared_ptr<Peer> const& p) { p->send(sm); });

    JLOG(j_.debug()) << ctag(chainId_) << "share(tx): relayed"
                     << ", id=" << to_string(tx.id())
                     << ", bytes=" << data.size();
}

std::optional<InnerTxSet>
InnerConsensus::Adaptor::acquireTxSet(InnerTxSet::ID const& setId)
{
    JLOG(j_.trace()) << ctag(chainId_) << "acquireTxSet"
                     << ", id=" << to_string(setId);
    return getCachedTxSet(setId);
}

void
InnerConsensus::Adaptor::cacheTxSet(InnerTxSet const& txSet)
{
    std::lock_guard lock(txSetMutex_);
    auto const id = txSet.id();
    txSetCache_.emplace(id, txSet);

    if (txSetCache_.size() > 20)
    {
        auto it = txSetCache_.begin();
        if (it->first != id)
            txSetCache_.erase(it);
    }
}

std::optional<InnerTxSet>
InnerConsensus::Adaptor::getCachedTxSet(uint256 const& id) const
{
    std::lock_guard lock(txSetMutex_);
    auto it = txSetCache_.find(id);
    if (it != txSetCache_.end())
        return it->second;
    return std::nullopt;
}

bool
InnerConsensus::Adaptor::hasOpenTransactions() const
{
    return false;
}

std::size_t
InnerConsensus::Adaptor::proposersValidated(uint256 const& h) const
{
    std::lock_guard lock(valMutex_);
    auto it = validationsPerLedger_.find(h);
    if (it == validationsPerLedger_.end())
        return 0;
    return it->second.validators.size();
}

std::size_t
InnerConsensus::Adaptor::proposersFinished(
    InnerCxLedger const& ledger,
    uint256 const& h) const
{
    std::lock_guard lock(valMutex_);
    std::size_t count = 0;
    for (auto const& [nodeID, info] : latestByNode_)
    {
        if (info.second > ledger.seq())
            ++count;
    }
    return count;
}

void
InnerConsensus::Adaptor::propose(
    InnerCxPeerPos::Proposal const& proposal)
{
    JLOG(j_.trace()) << ctag(chainId_)
                     << (proposal.isBowOut() ? "bow out: " : "propose: ")
                     << to_string(proposal.prevLedger()) << " -> "
                     << to_string(proposal.position());

    if (!validatorKeys_.keys)
    {
        JLOG(j_.warn()) << ctag(chainId_)
                        << "propose: no validator keys available";
        return;
    }

    auto const& keys = *validatorKeys_.keys;

    protocol::TMProposeSet prop;
    prop.set_currenttxhash(
        proposal.position().begin(), proposal.position().size());
    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.prevLedger().size());
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());
    prop.set_nodepubkey(keys.publicKey.data(), keys.publicKey.size());
    prop.set_chainid(chainId_);

    auto sig = signDigest(
        keys.publicKey, keys.secretKey, proposal.signingHash());
    prop.set_signature(sig.data(), sig.size());

    JLOG(j_.debug()) << ctag(chainId_) << "broadcasting proposal"
                     << ", seq=" << proposal.proposeSeq();

    app_.getOverlay().broadcast(prop);
}

void
InnerConsensus::Adaptor::share(InnerTxSet const& txns)
{
    auto const setId = txns.id();

    JLOG(j_.debug()) << ctag(chainId_) << "share(txSet)"
                     << ", id=" << to_string(setId);

    cacheTxSet(txns);

    Serializer s;
    s.addRaw("TXST", 4);
    s.addBitString(setId);

    std::uint32_t count = 0;
    std::vector<std::pair<uint256, Blob>> items;
    txns.map->visitLeaves(
        [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
            items.emplace_back(item->key(), Blob(
                item->slice().begin(), item->slice().end()));
            ++count;
        });

    s.add32(count);
    for (auto const& [key, data] : items)
    {
        s.addBitString(key);
        s.add32(static_cast<std::uint32_t>(data.size()));
        s.addRaw(data.data(), data.size());
    }

    protocol::TMTransaction tx;
    tx.set_rawtransaction(s.getData().data(), s.getData().size());
    tx.set_status(protocol::tsNEW);
    tx.set_chainid(chainId_);

    auto const sm = std::make_shared<Message>(
        tx, protocol::mtTRANSACTION);
    app_.getOverlay().foreach(
        [&sm](std::shared_ptr<Peer> const& p) { p->send(sm); });

    JLOG(j_.debug()) << ctag(chainId_) << "broadcast tx set"
                     << ", id=" << to_string(setId)
                     << ", items=" << count
                     << ", bytes=" << s.getData().size();
}

uint256
InnerConsensus::Adaptor::getPrevLedger(
    uint256 ledgerID,
    InnerCxLedger const& ledger,
    ConsensusMode mode)
{
    std::lock_guard lock(valMutex_);

    std::map<uint256, std::size_t> hashCounts;
    std::size_t total = 0;

    for (auto const& [nodeID, info] : latestByNode_)
    {
        if (info.second == ledger.seq())
        {
            ++total;
            ++hashCounts[info.first];
        }
    }

    if (total == 0)
        return ledgerID;

    uint256 bestHash = ledgerID;
    std::size_t bestCount = 0;
    for (auto const& [hash, count] : hashCounts)
    {
        if (count > bestCount)
        {
            bestHash = hash;
            bestCount = count;
        }
    }

    if (bestHash != ledgerID && bestCount > 0 &&
        (bestCount * 100 / total) >= 60)
    {
        JLOG(j_.warn()) << ctag(chainId_) << "getPrevLedger: switching"
                        << ", from=" << to_string(ledgerID)
                        << ", to=" << to_string(bestHash)
                        << ", votes=" << bestCount << "/" << total;
        return bestHash;
    }

    return ledgerID;
}

void
InnerConsensus::Adaptor::onModeChange(
    ConsensusMode before,
    ConsensusMode after)
{
    mode_ = after;
    JLOG(j_.info()) << ctag(chainId_) << "mode change: "
                    << to_string(before) << " -> " << to_string(after);
}

InnerConsensus::Adaptor::Result
InnerConsensus::Adaptor::onClose(
    InnerCxLedger const& ledger,
    NetClock::time_point const& closeTime,
    ConsensusMode mode)
{
    auto txSet = std::make_shared<SHAMap>(
        SHAMapType::TRANSACTION, app_.getNodeFamily());

    auto pending = txQueue_.drain();

    JLOG(j_.debug()) << ctag(chainId_) << "onClose"
                     << ", ledger_seq=" << ledger.seq()
                     << ", pending_txs=" << pending.size()
                     << ", mode=" << to_string(mode);

    std::size_t added = 0;
    for (auto const& tx : pending)
    {
        Serializer s;
        tx.serialize(s);
        auto const txID = tx.id();
        txSet->addItem(
            SHAMapNodeType::TnTransactionNm,
            makeShamapitem(txID, s.slice()));

        JLOG(j_.trace()) << ctag(chainId_) << "onClose: added tx"
                         << ", type=" << static_cast<int>(tx.type)
                         << ", id=" << to_string(txID)
                         << ", amount=" << tx.amount;
        ++added;
    }

    auto const setID = txSet->getHash().asUint256();

    JLOG(j_.debug()) << ctag(chainId_) << "onClose: built tx set"
                     << ", tx_count=" << added
                     << ", set_id=" << to_string(setID);

    return Result(
        InnerTxSet{std::move(txSet)},
        ConsensusProposal<NodeID, uint256, uint256>(
            ledger.id(),
            0,
            setID,
            closeTime,
            app_.getTimeKeeper().closeTime(),
            NodeID{}));
}

void
InnerConsensus::Adaptor::onAccept(
    Result const& result,
    InnerCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    json::Value&& consensusJson,
    bool const validating)
{
    auto const& prevHeader = prevLedger.ledger->header();

    JLOG(j_.debug()) << ctag(chainId_) << "onAccept"
                     << ", prev_seq=" << prevHeader.seq
                     << ", prev_hash=" << to_string(prevHeader.hash)
                     << ", validating=" << (validating ? "yes" : "no");

    InnerLedgerHeader newHeader;
    newHeader.seq = prevHeader.seq + 1;
    newHeader.parentHash = prevHeader.hash;
    newHeader.closeTimeResolution = closeResolution;
    newHeader.closeTimeAgree = true;
    newHeader.outerCheckpointSeq = prevHeader.outerCheckpointSeq;
    newHeader.outerCheckpointHash = prevHeader.outerCheckpointHash;

    auto stateMap = prevLedger.ledger->stateMap()->snapShot(true);
    auto txMap = std::make_shared<SHAMap>(
        SHAMapType::TRANSACTION, app_.getNodeFamily());

    auto const& agreedTxs = result.txns;
    std::size_t applied = 0;
    std::size_t failed = 0;

    std::size_t agreedCount = 0;
    agreedTxs.map->visitLeaves(
        [&agreedCount](boost::intrusive_ptr<SHAMapItem const> const&) {
            ++agreedCount;
        });

    JLOG(j_.info()) << ctag(chainId_) << "onAccept: agreed tx set"
                    << ", set_id=" << to_string(agreedTxs.id())
                    << ", agreed_tx_count=" << agreedCount
                    << ", mode=" << to_string(mode)
                    << ", validating=" << (validating ? "yes" : "no");

    agreedTxs.map->visitLeaves(
        [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
            auto tx = InnerTx::deserialize(item->slice());
            if (!tx)
            {
                JLOG(j_.warn()) << ctag(chainId_)
                                << "onAccept: failed to deserialize tx"
                                << ", item_key="
                                << to_string(item->key())
                                << ", data_size=" << item->slice().size();
                ++failed;
                return;
            }

            JLOG(j_.trace()) << ctag(chainId_)
                             << "onAccept: applying tx"
                             << ", type="
                             << registry_.name(tx->type)
                             << ", amount=" << tx->amount
                             << ", dst=" << toBase58(tx->destination);

            InnerView view(*stateMap);
            auto const ter = registry_.apply(*tx, view, j_);

            if (isTesSuccess(ter))
            {
                txMap->addItem(
                    SHAMapNodeType::TnTransactionNm,
                    makeShamapitem(item->key(), item->slice()));
                ++applied;

                JLOG(j_.debug()) << ctag(chainId_)
                                 << "onAccept: tx applied"
                                 << ", type="
                                 << registry_.name(tx->type)
                                 << ", result=" << transHuman(ter);

                if (tx->type == InnerTxType::Withdraw &&
                    withdrawalCallback_)
                {
                    WithdrawalInfo wi;
                    wi.destination = tx->destination;
                    wi.amount = tx->amount;
                    wi.innerSeq = newHeader.seq;
                    wi.chainId = chainId_;
                    withdrawalCallback_(std::move(wi));
                }
            }
            else
            {
                ++failed;
                JLOG(j_.warn()) << ctag(chainId_)
                                << "onAccept: tx FAILED"
                                << ", type="
                                << registry_.name(tx->type)
                                << ", result=" << transHuman(ter)
                                << ", amount=" << tx->amount;
            }
        });

    newHeader.stateHash = stateMap->getHash().asUint256();
    newHeader.txHash = txMap->getHash().asUint256();
    newHeader.hash = sha512Half(
        newHeader.stateHash,
        newHeader.txHash,
        newHeader.seq,
        newHeader.parentHash);

    auto newLedger = std::make_shared<InnerLedger>(
        newHeader, stateMap, std::move(txMap));

    ledgerMaster_.storeLedger(newLedger);
    ledgerMaster_.setClosed(newLedger);

    if (validating)
    {
        ledgerMaster_.setValidated(newLedger);
        doValidation(newHeader, result.txns.id());
    }

    prevProposers_ = result.proposers;
    prevRoundTime_ = result.roundTime.read();

    totalTxApplied_ += applied;
    totalTxFailed_ += failed;
    ++totalRounds_;

    JLOG(j_.info()) << ctag(chainId_) << "accepted ledger"
                    << ", seq=" << newHeader.seq
                    << ", hash=" << to_string(newHeader.hash)
                    << ", state_hash=" << to_string(newHeader.stateHash)
                    << ", txs_applied=" << applied
                    << ", txs_failed=" << failed
                    << ", total_rounds=" << totalRounds_.load();

    // Record our own validation in the tracker
    {
        std::lock_guard lock(valMutex_);
        if (validatorKeys_.keys)
        {
            auto& entry = validationsPerLedger_[newHeader.hash];
            entry.seq = newHeader.seq;
            entry.validators.insert(validatorKeys_.nodeID);
            latestByNode_[validatorKeys_.nodeID] =
                {newHeader.hash, newHeader.seq};
        }
    }
}

void
InnerConsensus::Adaptor::onForceAccept(
    Result const& result,
    InnerCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    json::Value&& consensusJson)
{
    bool const validating = validating_ || app_.config().standalone();
    JLOG(j_.debug()) << ctag(chainId_) << "onForceAccept"
                     << ", standalone=" << app_.config().standalone()
                     << ", validating_flag=" << validating_.load()
                     << ", effective_validating="
                     << (validating ? "yes" : "no");

    onAccept(
        result,
        prevLedger,
        closeResolution,
        rawCloseTimes,
        mode,
        std::move(consensusJson),
        validating);
}

void
InnerConsensus::Adaptor::notify(
    protocol::NodeEvent ne,
    InnerCxLedger const& ledger,
    bool haveCorrectLCL)
{
    JLOG(j_.trace()) << ctag(chainId_) << "notify"
                     << ", event=" << static_cast<int>(ne)
                     << ", ledger_seq=" << ledger.seq()
                     << ", correct_lcl="
                     << (haveCorrectLCL ? "yes" : "no");
}

void
InnerConsensus::Adaptor::doValidation(
    InnerLedgerHeader const& header,
    uint256 const& consensusHash)
{
    if (!validatorKeys_.keys)
    {
        JLOG(j_.trace()) << ctag(chainId_)
                         << "doValidation: no keys, skipping";
        return;
    }

    auto const& keys = *validatorKeys_.keys;
    auto const now = app_.getTimeKeeper().closeTime();

    auto v = std::make_shared<STValidation>(
        now,
        keys.publicKey,
        keys.secretKey,
        validatorKeys_.nodeID,
        [&](STValidation& v) {
            v.setFieldH256(sfLedgerHash, header.hash);
            v.setFieldH256(sfConsensusHash, consensusHash);
            v.setFieldU32(sfLedgerSequence, header.seq);
            v.setFlag(kVF_FULL_VALIDATION);
            v.setFieldU64(sfCookie, valCookie_);
        });

    auto const serialized = v->getSerialized();

    protocol::TMValidation val;
    val.set_validation(serialized.data(), serialized.size());
    val.set_chainid(chainId_);

    app_.getOverlay().broadcast(val);

    JLOG(j_.debug()) << ctag(chainId_) << "sent validation"
                     << ", seq=" << header.seq
                     << ", hash=" << to_string(header.hash);
}

// --- InnerConsensus Implementation ---

InnerConsensus::InnerConsensus(
    Application& app,
    InnerLedgerMaster& ledgerMaster,
    InnerTxQueue& txQueue,
    InnerTransactorRegistry const& registry,
    Consensus<Adaptor>::clock_type const& clock,
    ValidatorKeys const& validatorKeys,
    InnerChainConfig const& config,
    beast::Journal journal)
    : adaptor_(
          app,
          ledgerMaster,
          txQueue,
          registry,
          validatorKeys,
          config,
          journal)
    , consensus_(clock, adaptor_, journal)
{
    JLOG(journal.info()) << ctag(config.networkId)
                         << "consensus engine created";
}

json::Value
InnerConsensus::getJson(bool full) const
{
    std::scoped_lock const _{mutex_};
    return consensus_.getJson(full);
}

void
InnerConsensus::startRound(
    NetClock::time_point const& now,
    InnerCxLedger::ID const& prevLgrId,
    InnerCxLedger const& prevLgr,
    hash_set<NodeID> const& nowUntrusted,
    hash_set<NodeID> const& nowTrusted)
{
    std::scoped_lock const _{mutex_};
    adaptor_.preStartRound(prevLgr, nowTrusted);
    consensus_.startRound(
        now, prevLgrId, prevLgr, nowUntrusted, adaptor_.validating());
}

void
InnerConsensus::timerEntry(NetClock::time_point const& now)
{
    std::scoped_lock const _{mutex_};
    consensus_.timerEntry(now);
}

void
InnerConsensus::gotTxSet(
    NetClock::time_point const& now,
    InnerTxSet const& txSet)
{
    std::scoped_lock const _{mutex_};
    consensus_.gotTxSet(now, txSet);
}

void
InnerConsensus::simulate(
    NetClock::time_point const& now,
    std::optional<std::chrono::milliseconds> consensusDelay)
{
    std::scoped_lock const _{mutex_};
    consensus_.simulate(now, consensusDelay);
}

bool
InnerConsensus::peerProposal(
    NetClock::time_point const& now,
    InnerCxPeerPos const& newProposal)
{
    std::scoped_lock const _{mutex_};
    return consensus_.peerProposal(now, newProposal);
}

void
InnerConsensus::handleValidation(std::shared_ptr<STValidation> const& val)
{
    auto const nodeID = calcNodeID(val->getSignerPublic());
    auto const hash = val->getFieldH256(sfLedgerHash);
    auto const seq = val->getFieldU32(sfLedgerSequence);

    std::lock_guard lock(adaptor_.valMutex_);

    auto& entry = adaptor_.validationsPerLedger_[hash];
    entry.seq = seq;
    entry.validators.insert(nodeID);

    adaptor_.latestByNode_[nodeID] = {hash, seq};

    if (seq > adaptor_.latestValSeq_)
        adaptor_.latestValSeq_ = seq;

    // Prune entries older than latestValSeq_ - 5
    if (adaptor_.latestValSeq_ > 5)
    {
        auto const minSeq = adaptor_.latestValSeq_ - 5;
        for (auto it = adaptor_.validationsPerLedger_.begin();
             it != adaptor_.validationsPerLedger_.end();)
        {
            if (it->second.seq < minSeq)
                it = adaptor_.validationsPerLedger_.erase(it);
            else
                ++it;
        }
    }
}

void
InnerConsensus::startNextRound()
{
    auto const now =
        adaptor_.app_.getTimeKeeper().closeTime();
    auto closed = adaptor_.ledgerMaster_.getClosedLedger();
    if (!closed)
        return;

    InnerCxLedger prevLgr{closed};

    auto const [quorum, keys] = adaptor_.getQuorumKeys();
    hash_set<NodeID> trusted;
    for (auto const& pk : keys)
        trusted.insert(calcNodeID(pk));

    startRound(now, prevLgr.id(), prevLgr, hash_set<NodeID>{}, trusted);
}

}  // namespace inner
}  // namespace xrpl
