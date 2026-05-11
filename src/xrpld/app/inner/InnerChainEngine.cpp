#include <xrpld/app/inner/InnerChainEngine.h>
#include <xrpld/app/inner/InnerCxPeerPos.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/TimeKeeper.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/overlay/Peer.h>

#include <xrpl/basics/Log.h>
#include <xrpl/core/HashRouter.h>
#include <xrpl/core/JobQueue.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

#include <xrpld/app/misc/ValidatorKeys.h>

#include <boost/endian/conversion.hpp>

#include <cstring>

namespace xrpl {
namespace inner {

// ---------------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------------

std::vector<InnerChainConfig>
parseInnerChainConfigs(Config const& config)
{
    std::vector<InnerChainConfig> results;

    // New format: [inner_chains] lists chain section names
    if (config.exists("inner_chains"))
    {
        auto const& names = config.section("inner_chains").values();
        for (auto const& name : names)
        {
            if (!config.exists(name))
                continue;

            auto const& section = config.section(name);
            InnerChainConfig c;
            c.name = name;
            c.enabled =
                section.valueOr<std::string>("enabled", "1") == "1";
            c.networkId =
                section.valueOr<std::uint32_t>("network_id", 0);
            c.ledgerCloseMs = std::chrono::milliseconds{
                section.valueOr<std::uint32_t>("ledger_close_ms", 1000)};
            c.granularity = std::chrono::milliseconds{
                section.valueOr<std::uint32_t>("granularity", 500)};
            c.stateRootInterval =
                section.valueOr<std::uint32_t>("state_root_interval", 10);

            auto const dbSection = name + ".node_db";
            if (config.exists(dbSection))
            {
                c.nodeDbPath =
                    config.section(dbSection)
                        .valueOr<std::string>("path", "");
            }

            auto const valSection = name + ".validators";
            if (config.exists(valSection))
                c.validatorKeys = config.section(valSection).values();

            auto const vlkSection = name + ".validator_list_keys";
            if (config.exists(vlkSection))
                c.validatorListKeys = config.section(vlkSection).values();

            if (c.networkId != 0)
                results.push_back(std::move(c));
        }
        return results;
    }

    // Legacy format: single [inner_chain] section
    if (config.exists("inner_chain"))
    {
        auto const& section = config.section("inner_chain");
        InnerChainConfig c;
        c.name = "inner_chain";
        c.enabled =
            section.valueOr<std::string>("enabled", "0") == "1";
        c.networkId =
            section.valueOr<std::uint32_t>("network_id", 1001);
        c.ledgerCloseMs = std::chrono::milliseconds{
            section.valueOr<std::uint32_t>("ledger_close_ms", 1000)};
        c.granularity = std::chrono::milliseconds{
            section.valueOr<std::uint32_t>("granularity", 500)};
        c.stateRootInterval =
            section.valueOr<std::uint32_t>("state_root_interval", 10);

        if (config.exists("inner_node_db"))
        {
            c.nodeDbPath =
                config.section("inner_node_db")
                    .valueOr<std::string>("path", "");
        }

        if (config.exists("inner_validators"))
            c.validatorKeys = config.section("inner_validators").values();

        if (config.exists("inner_validator_list_keys"))
            c.validatorListKeys =
                config.section("inner_validator_list_keys").values();

        if (c.enabled)
            results.push_back(std::move(c));
    }

    return results;
}

InnerChainConfig
parseInnerChainConfig(Config const& config)
{
    auto configs = parseInnerChainConfigs(config);
    if (!configs.empty())
        return std::move(configs.front());
    return {};
}

// ---------------------------------------------------------------------------
// InnerChainEngine
// ---------------------------------------------------------------------------

InnerChainEngine::InnerChainEngine(
    Application& app,
    InnerChainConfig config,
    Stopwatch const& clock,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : app_(app)
    , config_(std::move(config))
    , j_(journal)
    , validators_(std::make_unique<ValidatorList>(
          validatorManifests_,
          publisherManifests_,
          app.getTimeKeeper(),
          app.config().legacy("database_path"),
          journal))
    , ledgerMaster_(app.getNodeFamily(), journal)
    , heartbeatTimer_(app.getIOContext())
{
    JLOG(j_.info()) << tag() << "initializing, network_id="
                    << config_.networkId
                    << ", name=" << config_.name;

    // Load per-chain UNL if configured
    if (!config_.validatorKeys.empty() ||
        !config_.validatorListKeys.empty())
    {
        std::optional<PublicKey> localKey;
        if (validatorKeys.keys)
            localKey = validatorKeys.keys->publicKey;

        if (!validators_->load(
                localKey,
                config_.validatorKeys,
                config_.validatorListKeys))
        {
            JLOG(j_.warn())
                << tag() << "failed to load validator list from config; "
                << "validator_keys=" << config_.validatorKeys.size()
                << ", publisher_keys=" << config_.validatorListKeys.size();
        }
        else
        {
            JLOG(j_.info())
                << tag() << "loaded UNL: "
                << config_.validatorKeys.size() << " validator keys, "
                << config_.validatorListKeys.size() << " publisher keys";
        }
    }
    else
    {
        JLOG(j_.info())
            << tag()
            << "no per-chain UNL configured; using standalone mode";
    }

    ledgerMaster_.buildGenesisLedger();
    JLOG(j_.debug()) << tag() << "genesis ledger built";

    registerBuiltinTransactors(registry_);
    JLOG(j_.debug()) << tag() << "registered builtin transactors";

    consensus_ = std::make_unique<InnerConsensus>(
        app,
        ledgerMaster_,
        txQueue_,
        registry_,
        clock,
        validatorKeys,
        config_,
        journal);

    consensus_->setWithdrawalCallback(
        [this](InnerConsensus::WithdrawalInfo wi) {
            WithdrawalRecord rec;
            rec.destination = wi.destination;
            rec.amount = wi.amount;
            rec.innerSeq = wi.innerSeq;
            rec.chainId = wi.chainId;
            queueWithdrawal(std::move(rec));
        });

    JLOG(j_.info()) << tag() << "initialized successfully"
                    << ", ledger_close_ms=" << config_.ledgerCloseMs.count()
                    << ", granularity=" << config_.granularity.count()
                    << ", state_root_interval=" << config_.stateRootInterval;
}

InnerChainEngine::~InnerChainEngine()
{
    stop();
}

bool
InnerChainEngine::start()
{
    if (running_)
    {
        JLOG(j_.warn()) << tag() << "start() called but already running";
        return true;
    }

    auto closed = ledgerMaster_.getClosedLedger();
    if (!closed)
    {
        JLOG(j_.fatal()) << tag() << "no closed ledger at start — aborting";
        return false;
    }

    running_ = true;
    startTime_ = std::chrono::steady_clock::now();

    auto const now = app_.getTimeKeeper().closeTime();
    InnerCxLedger prevLgr{closed};

    JLOG(j_.debug()) << tag() << "starting first consensus round"
                     << ", prev_ledger_seq=" << prevLgr.seq()
                     << ", prev_ledger_id=" << to_string(prevLgr.id());

    auto const trusted = getTrustedNodeIDs();
    consensus_->startRound(
        now, prevLgr.id(), prevLgr, hash_set<NodeID>{}, trusted);

    setHeartbeatTimer();

    JLOG(j_.info()) << tag() << "started";
    return true;
}

void
InnerChainEngine::stop()
{
    if (!running_)
        return;

    running_ = false;

    try
    {
        heartbeatTimer_.cancel();
    }
    catch (boost::system::system_error const& e)
    {
        JLOG(j_.error()) << tag()
                         << "heartbeatTimer cancel error: " << e.what();
    }

    JLOG(j_.info()) << tag() << "stopped";
}

bool
InnerChainEngine::beginConsensus(uint256 const& networkClosed)
{
    JLOG(j_.trace()) << tag() << "beginConsensus called"
                     << ", networkClosed=" << to_string(networkClosed);
    return true;
}

void
InnerChainEngine::timerEntry()
{
    if (!running_)
        return;

    auto const now = app_.getTimeKeeper().closeTime();
    JLOG(j_.trace()) << tag() << "timerEntry";
    consensus_->timerEntry(now);
}

void
InnerChainEngine::onPeerProposal(
    NetClock::time_point const& now,
    std::shared_ptr<protocol::TMProposeSet> const& m)
{
    auto const& set = *m;

    auto const sig = makeSlice(set.signature());
    if (std::clamp<std::size_t>(sig.size(), 64, 72) != sig.size())
    {
        JLOG(j_.warn()) << tag() << "onPeerProposal: bad signature size";
        return;
    }

    auto const pkSlice = makeSlice(set.nodepubkey());
    if (publicKeyType(pkSlice) != KeyType::Secp256k1)
    {
        JLOG(j_.warn()) << tag() << "onPeerProposal: bad key type";
        return;
    }

    if (set.currenttxhash().size() != sizeof(uint256) ||
        set.previousledger().size() != sizeof(uint256))
    {
        JLOG(j_.warn()) << tag() << "onPeerProposal: malformed hashes";
        return;
    }

    PublicKey const publicKey{pkSlice};
    uint256 const proposeHash{set.currenttxhash()};
    uint256 const prevLedger{set.previousledger()};
    NetClock::time_point const closeTime{
        NetClock::duration{set.closetime()}};

    // Compute suppression ID including chainId to avoid collision with
    // outer chain proposals
    Serializer s(512);
    s.addBitString(proposeHash);
    s.addBitString(prevLedger);
    s.add32(set.proposeseq());
    s.add32(closeTime.time_since_epoch().count());
    s.addVL(publicKey.slice());
    s.addVL(sig);
    s.add32(config_.networkId);
    uint256 const suppression = s.getSHA512Half();

    if (auto [added, relayed] =
            app_.getHashRouter().addSuppressionPeerWithStatus(
                suppression, 0);
        !added)
    {
        JLOG(j_.trace()) << tag() << "onPeerProposal: duplicate";
        return;
    }

    auto peerPos = InnerCxPeerPos(
        publicKey,
        sig,
        suppression,
        InnerCxPeerPos::Proposal{
            prevLedger,
            set.proposeseq(),
            proposeHash,
            closeTime,
            app_.getTimeKeeper().closeTime(),
            calcNodeID(publicKey)});

    if (!peerPos.checkSign())
    {
        JLOG(j_.warn()) << tag() << "onPeerProposal: bad signature";
        return;
    }

    JLOG(j_.debug()) << tag() << "onPeerProposal: accepted"
                     << ", node=" << to_string(peerPos.proposal().nodeID())
                     << ", seq=" << peerPos.proposal().proposeSeq()
                     << ", pos=" << to_string(proposeHash);

    consensus_->peerProposal(now, peerPos);
}

void
InnerChainEngine::onPeerValidation(
    std::shared_ptr<protocol::TMValidation> const& m)
{
    if (m->validation().size() < 50)
    {
        JLOG(j_.warn()) << tag() << "onPeerValidation: too small";
        return;
    }

    // Gap 2: Validation suppression via HashRouter
    auto const suppression = sha512Half(
        makeSlice(m->validation()),
        config_.networkId);

    if (auto [added, relayed] =
            app_.getHashRouter().addSuppressionPeerWithStatus(
                suppression, 0);
        !added)
    {
        JLOG(j_.trace()) << tag() << "onPeerValidation: duplicate";
        return;
    }

    try
    {
        SerialIter sit(makeSlice(m->validation()));
        auto val = std::make_shared<STValidation>(
            std::ref(sit),
            [](PublicKey const& pk) {
                return calcNodeID(pk);
            },
            true);

        if (!val->isValid())
        {
            JLOG(j_.warn()) << tag() << "onPeerValidation: invalid signature";
            return;
        }

        auto const seq = val->getFieldU32(sfLedgerSequence);
        auto const hash = val->getFieldH256(sfLedgerHash);

        JLOG(j_.debug()) << tag() << "onPeerValidation: accepted"
                         << ", seq=" << seq
                         << ", hash=" << to_string(hash)
                         << ", node=" << toBase58(
                                TokenType::NodePublic, val->getSignerPublic());

        consensus_->handleValidation(val);
    }
    catch (std::exception const& ex)
    {
        JLOG(j_.warn()) << tag()
                        << "onPeerValidation: exception: " << ex.what();
    }
}

void
InnerChainEngine::onPeerTransaction(
    std::shared_ptr<protocol::TMTransaction> const& m)
{
    try
    {
        auto const data = makeSlice(m->rawtransaction());

        JLOG(j_.info()) << tag() << "onPeerTransaction: received"
                        << ", data_size=" << data.size()
                        << ", has_chainid=" << m->has_chainid()
                        << ", chainid=" << (m->has_chainid() ? m->chainid() : 0);

        // Gap 1: Check for TXST magic prefix (tx set sharing)
        static constexpr char kTxSetMagic[] = "TXST";
        bool hasMagic = data.size() >= 40 &&
            std::memcmp(data.data(), kTxSetMagic, 4) == 0;

        JLOG(j_.info()) << tag() << "onPeerTransaction: TXST check"
                        << ", size_ok=" << (data.size() >= 40)
                        << ", magic_match=" << hasMagic
                        << ", first4="
                        << (data.size() >= 4
                                ? strHex(Slice(data.data(), 4))
                                : "too_short");

        if (hasMagic)
        {
            JLOG(j_.info()) << tag()
                            << "onPeerTransaction: routing to handleTxSetMessage";
            handleTxSetMessage(data);
            return;
        }

        // Gap 6: Transaction dedup via HashRouter
        auto const suppression = sha512Half(data, config_.networkId);
        if (auto [added, relayed] =
                app_.getHashRouter().addSuppressionPeerWithStatus(
                    suppression, 0);
            !added)
        {
            JLOG(j_.debug()) << tag() << "onPeerTransaction: duplicate"
                             << ", suppression=" << to_string(suppression);
            return;
        }

        auto tx = InnerTx::deserialize(data);
        if (!tx)
        {
            JLOG(j_.warn()) << tag()
                            << "onPeerTransaction: failed to deserialize"
                            << ", data_size=" << data.size()
                            << ", first_bytes="
                            << strHex(Slice(
                                   data.data(),
                                   std::min(data.size(), std::size_t(16))));
            return;
        }

        JLOG(j_.info()) << tag() << "onPeerTransaction: submitting"
                        << ", type=" << static_cast<int>(tx->type)
                        << ", amount=" << tx->amount
                        << ", dest=" << toBase58(tx->destination);

        txQueue_.submit(std::move(*tx));

        // Re-relay to peers for gossip propagation
        auto const sm = std::make_shared<Message>(
            *m, protocol::mtTRANSACTION);
        app_.getOverlay().foreach(
            [&sm](std::shared_ptr<Peer> const& p) { p->send(sm); });
    }
    catch (std::exception const& ex)
    {
        JLOG(j_.warn()) << tag()
                        << "onPeerTransaction: exception: " << ex.what();
    }
}

void
InnerChainEngine::handleTxSetMessage(Slice data)
{
    // Format: "TXST"(4) | setId(32) | count(4) | [key(32) | len(4) | data]...
    constexpr std::size_t kHeaderSize = 4 + 32 + 4;
    if (data.size() < kHeaderSize)
    {
        JLOG(j_.warn()) << tag() << "handleTxSetMessage: too small";
        return;
    }

    auto const* p = data.data() + 4;
    uint256 setId;
    std::memcpy(setId.data(), p, 32);
    p += 32;

    std::uint32_t count;
    std::memcpy(&count, p, 4);
    count = boost::endian::big_to_native(count);
    p += 4;

    JLOG(j_.info()) << tag() << "handleTxSetMessage: parsed header"
                    << ", setId=" << to_string(setId)
                    << ", count=" << count
                    << ", total_size=" << data.size();

    // Dedup via HashRouter
    auto const suppression = sha512Half(
        Slice("TXST", 4), setId, config_.networkId);
    if (auto [added, relayed] =
            app_.getHashRouter().addSuppressionPeerWithStatus(
                suppression, 0);
        !added)
    {
        JLOG(j_.debug()) << tag() << "handleTxSetMessage: duplicate set "
                         << to_string(setId);
        return;
    }

    auto const* end = data.data() + data.size();
    auto txMap = std::make_shared<SHAMap>(
        SHAMapType::TRANSACTION, app_.getNodeFamily());

    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (p + 36 > end)
        {
            JLOG(j_.warn()) << tag()
                            << "handleTxSetMessage: truncated at item " << i;
            return;
        }

        uint256 key;
        std::memcpy(key.data(), p, 32);
        p += 32;

        std::uint32_t len;
        std::memcpy(&len, p, 4);
        len = boost::endian::big_to_native(len);
        p += 4;

        if (p + len > end)
        {
            JLOG(j_.warn()) << tag()
                            << "handleTxSetMessage: truncated data at item "
                            << i;
            return;
        }

        Slice itemData(p, len);
        txMap->addItem(
            SHAMapNodeType::TnTransactionNm,
            makeShamapitem(key, itemData));
        p += len;
    }

    InnerTxSet txSet{txMap};
    if (txSet.id() != setId)
    {
        JLOG(j_.warn()) << tag() << "handleTxSetMessage: hash mismatch"
                        << ", expected=" << to_string(setId)
                        << ", got=" << to_string(txSet.id());
        return;
    }

    JLOG(j_.info()) << tag() << "handleTxSetMessage: received tx set"
                    << ", id=" << to_string(setId)
                    << ", items=" << count;

    consensus_->cacheTxSet(txSet);

    auto const now = app_.getTimeKeeper().closeTime();
    consensus_->gotTxSet(now, txSet);
}

std::optional<InnerChainEngine::StateRootInfo>
InnerChainEngine::getStateRootForCommit(std::uint32_t outerSeq) const
{
    if (!running_)
    {
        JLOG(j_.trace()) << tag()
                         << "getStateRootForCommit: not running";
        return std::nullopt;
    }

    if (config_.stateRootInterval == 0)
        return std::nullopt;

    if (outerSeq % config_.stateRootInterval != 0)
        return std::nullopt;

    auto validated = ledgerMaster_.getValidatedLedger();
    if (!validated)
    {
        JLOG(j_.debug()) << tag()
                         << "getStateRootForCommit: no validated ledger"
                         << ", outerSeq=" << outerSeq;
        return std::nullopt;
    }

    auto const& header = validated->header();
    JLOG(j_.info()) << tag()
                    << "producing state root for outer commit"
                    << ", outerSeq=" << outerSeq
                    << ", innerSeq=" << header.seq
                    << ", stateRoot=" << to_string(header.stateHash);

    return StateRootInfo{header.stateHash, header.seq, config_.networkId};
}

void
InnerChainEngine::queueWithdrawal(WithdrawalRecord record)
{
    std::lock_guard lock(withdrawalMutex_);
    JLOG(j_.info()) << tag() << "queued withdrawal for outer chain"
                    << ", dest=" << toBase58(record.destination)
                    << ", amount=" << record.amount
                    << ", innerSeq=" << record.innerSeq;
    pendingWithdrawals_.push_back(std::move(record));
}

std::vector<InnerChainEngine::WithdrawalRecord>
InnerChainEngine::drainWithdrawalsForCommit(std::uint32_t outerSeq)
{
    if (!running_)
        return {};

    if (config_.stateRootInterval == 0)
        return {};

    if (outerSeq % config_.stateRootInterval != 0)
        return {};

    std::lock_guard lock(withdrawalMutex_);
    if (pendingWithdrawals_.empty())
        return {};

    std::vector<WithdrawalRecord> result;
    result.swap(pendingWithdrawals_);

    JLOG(j_.info()) << tag() << "draining " << result.size()
                    << " withdrawal(s) for outer commit"
                    << ", outerSeq=" << outerSeq;
    return result;
}

bool
InnerChainEngine::submit(InnerTx tx)
{
    JLOG(j_.debug()) << tag() << "submit: type="
                     << static_cast<int>(tx.type)
                     << ", src=" << toBase58(tx.source)
                     << ", dst=" << toBase58(tx.destination)
                     << ", amount=" << tx.amount;

    // Serialize before moving into queue
    Serializer s;
    tx.serialize(s);
    auto const txData = s.getData();

    // Register in HashRouter so we don't re-process our own broadcast
    auto const suppression = sha512Half(
        Slice(txData.data(), txData.size()), config_.networkId);
    app_.getHashRouter().addSuppressionPeerWithStatus(suppression, 0);

    if (!txQueue_.submit(std::move(tx)))
    {
        JLOG(j_.warn()) << tag() << "submit rejected: queue full ("
                        << InnerTxQueue::kMaxPending << ")";
        return false;
    }

    // Relay to peers
    protocol::TMTransaction msg;
    msg.set_rawtransaction(txData.data(), txData.size());
    msg.set_status(protocol::tsNEW);
    msg.set_chainid(config_.networkId);

    auto const sm = std::make_shared<Message>(
        msg, protocol::mtTRANSACTION);
    app_.getOverlay().foreach(
        [&sm](std::shared_ptr<Peer> const& p) { p->send(sm); });

    JLOG(j_.info()) << tag() << "submit: relayed to peers"
                    << ", bytes=" << txData.size();

    return true;
}

std::optional<InnerAccountFields>
InnerChainEngine::getAccount(AccountID const& id) const
{
    auto validated = ledgerMaster_.getValidatedLedger();
    if (!validated)
    {
        JLOG(j_.trace()) << tag()
                         << "getAccount: no validated ledger"
                         << ", account=" << toBase58(id);
        return std::nullopt;
    }

    auto const k = keyspace::account(id);
    auto const item = validated->stateMap()->peekItem(k.key);
    if (!item)
    {
        JLOG(j_.trace()) << tag()
                         << "getAccount: not found"
                         << ", account=" << toBase58(id);
        return std::nullopt;
    }

    auto data = item->slice();
    if (data.size() < 2)
        return std::nullopt;
    data += 2;

    InnerSLE sle(k.type, k.key, data);
    auto result = InnerAccountFields::readFrom(sle);

    if (result)
    {
        JLOG(j_.trace()) << tag()
                         << "getAccount: found"
                         << ", account=" << toBase58(id)
                         << ", balance=" << result->balance
                         << ", seq=" << result->sequence;
    }

    return result;
}

bool
InnerChainEngine::creditAccount(AccountID const& dest, std::uint64_t amount)
{
    InnerTx tx;
    tx.type = InnerTxType::Credit;
    tx.destination = dest;
    tx.amount = amount;

    // Serialize before moving into queue
    Serializer s;
    tx.serialize(s);
    auto const txData = s.getData();

    // Register in HashRouter so we don't re-process our own broadcast
    auto const suppression = sha512Half(
        Slice(txData.data(), txData.size()), config_.networkId);
    app_.getHashRouter().addSuppressionPeerWithStatus(suppression, 0);

    if (!txQueue_.submit(std::move(tx)))
    {
        JLOG(j_.warn()) << tag() << "creditAccount rejected: queue full ("
                        << InnerTxQueue::kMaxPending << ")"
                        << ", dest=" << toBase58(dest)
                        << ", amount=" << amount;
        return false;
    }

    JLOG(j_.info()) << tag() << "creditAccount: queued credit"
                    << ", dest=" << toBase58(dest)
                    << ", amount=" << amount;

    // Relay to peers so all validators have this tx before proposing
    try
    {
        protocol::TMTransaction msg;
        msg.set_rawtransaction(txData.data(), txData.size());
        msg.set_status(protocol::tsNEW);
        msg.set_chainid(config_.networkId);

        JLOG(j_.info()) << tag() << "creditAccount: built protobuf"
                        << ", raw_size=" << txData.size()
                        << ", has_chainid=" << msg.has_chainid()
                        << ", chainid=" << msg.chainid();

        auto const sm = std::make_shared<Message>(
            msg, protocol::mtTRANSACTION);

        std::size_t peerCount = 0;
        app_.getOverlay().foreach(
            [&sm, &peerCount](std::shared_ptr<Peer> const& p) {
                p->send(sm);
                ++peerCount;
            });

        JLOG(j_.info()) << tag() << "creditAccount: relayed to peers"
                        << ", dest=" << toBase58(dest)
                        << ", amount=" << amount
                        << ", bytes=" << txData.size()
                        << ", peer_count=" << peerCount;
    }
    catch (std::exception const& ex)
    {
        JLOG(j_.error()) << tag()
                         << "creditAccount: relay FAILED: " << ex.what()
                         << ", dest=" << toBase58(dest)
                         << ", amount=" << amount;
    }

    return true;
}

hash_set<NodeID>
InnerChainEngine::getTrustedNodeIDs() const
{
    hash_set<NodeID> trusted;
    auto& manifests = app_.getValidatorManifests();
    for (auto const& keyStr : config_.validatorKeys)
    {
        auto const pk =
            parseBase58<PublicKey>(TokenType::NodePublic, keyStr);
        if (pk)
        {
            if (auto const signingKey = manifests.getSigningKey(*pk))
                trusted.insert(calcNodeID(*signingKey));
            else
                trusted.insert(calcNodeID(*pk));
        }
    }
    return trusted;
}

json::Value
InnerChainEngine::getJson(bool full) const
{
    json::Value ret;
    ret["name"] = config_.name;
    ret["network_id"] = config_.networkId;
    ret["running"] = running_.load();
    ret["ledger_close_ms"] =
        static_cast<int>(config_.ledgerCloseMs.count());
    ret["granularity_ms"] =
        static_cast<int>(config_.granularity.count());
    ret["state_root_interval"] = config_.stateRootInterval;

    if (auto closed = ledgerMaster_.getClosedLedger())
    {
        ret["closed_ledger_seq"] = closed->header().seq;
        ret["closed_ledger_hash"] =
            to_string(closed->header().hash);
        ret["closed_state_hash"] =
            to_string(closed->header().stateHash);
    }

    if (auto validated = ledgerMaster_.getValidatedLedger())
    {
        ret["validated_ledger_seq"] = validated->header().seq;
        ret["validated_ledger_hash"] =
            to_string(validated->header().hash);
    }

    ret["last_checkpoint_outer_seq"] =
        ledgerMaster_.getLastCheckpointSeq();
    ret["tx_queue_size"] =
        static_cast<int>(txQueue_.size());
    ret["tx_queue_max"] =
        static_cast<int>(InnerTxQueue::kMaxPending);

    // Consensus health
    ret["consensus_phase"] =
        to_string(consensus_->phase());
    ret["consensus_mode"] =
        to_string(consensus_->mode());
    ret["validating"] = consensus_->validating();
    ret["prev_round_time_ms"] =
        static_cast<int>(consensus_->prevRoundTime().count());

    // Persistent counters
    ret["total_tx_applied"] =
        std::to_string(consensus_->totalTxApplied());
    ret["total_tx_failed"] =
        std::to_string(consensus_->totalTxFailed());
    ret["total_rounds"] = consensus_->totalRounds();

    // Uptime
    if (running_)
    {
        auto const elapsed = std::chrono::steady_clock::now() - startTime_;
        ret["uptime_ms"] = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count());
    }

    {
        std::lock_guard lock(withdrawalMutex_);
        ret["pending_withdrawals"] =
            static_cast<int>(pendingWithdrawals_.size());
    }

    if (full)
    {
        ret["validator_count"] =
            static_cast<int>(config_.validatorKeys.size());
        ret["publisher_count"] =
            static_cast<int>(config_.validatorListKeys.size());
    }

    return ret;
}

void
InnerChainEngine::setHeartbeatTimer()
{
    if (!running_)
        return;

    heartbeatTimer_.expires_after(config_.granularity);
    heartbeatTimer_.async_wait([this](boost::system::error_code const& ec) {
        if (ec == boost::asio::error::operation_aborted || !running_)
            return;
        if (ec)
        {
            JLOG(j_.error()) << tag()
                             << "heartbeat timer error: " << ec.message()
                             << ". Restarting timer.";
            setHeartbeatTimer();
            return;
        }
        processHeartbeat();
    });
}

void
InnerChainEngine::processHeartbeat()
{
    if (!running_)
        return;

    try
    {
        auto const now = app_.getTimeKeeper().closeTime();

        if (app_.config().standalone())
        {
            JLOG(j_.trace()) << tag()
                             << "heartbeat: simulating (standalone)";

            consensus_->simulate(now, std::nullopt);

            auto closed = ledgerMaster_.getClosedLedger();
            InnerCxLedger prevLgr{closed};
            auto const trusted = getTrustedNodeIDs();
            consensus_->startRound(
                now,
                prevLgr.id(),
                prevLgr,
                hash_set<NodeID>{},
                trusted);
        }
        else
        {
            if (consensus_->phase() == ConsensusPhase::Accepted)
            {
                JLOG(j_.debug()) << tag()
                                 << "heartbeat: starting next round";
                consensus_->startNextRound();
            }
            else
            {
                JLOG(j_.trace()) << tag() << "heartbeat: timerEntry";
                consensus_->timerEntry(now);
            }
        }
    }
    catch (std::exception const& ex)
    {
        JLOG(j_.error()) << tag()
                         << "processHeartbeat exception: " << ex.what();
    }
    catch (...)
    {
        JLOG(j_.error()) << tag()
                         << "processHeartbeat unknown exception";
    }

    setHeartbeatTimer();
}

std::string
InnerChainEngine::tag() const
{
    return "InnerChain[" + std::to_string(config_.networkId) + "] ";
}

// ---------------------------------------------------------------------------
// InnerChainRegistry
// ---------------------------------------------------------------------------

void
InnerChainRegistry::add(std::unique_ptr<InnerChainEngine> engine)
{
    auto id = engine->networkId();
    order_.push_back(id);
    engines_[id] = std::move(engine);
}

InnerChainEngine*
InnerChainRegistry::get(std::uint32_t networkId)
{
    auto it = engines_.find(networkId);
    return it != engines_.end() ? it->second.get() : nullptr;
}

InnerChainEngine const*
InnerChainRegistry::get(std::uint32_t networkId) const
{
    auto it = engines_.find(networkId);
    return it != engines_.end() ? it->second.get() : nullptr;
}

InnerChainEngine*
InnerChainRegistry::first()
{
    if (order_.empty())
        return nullptr;
    return get(order_.front());
}

InnerChainEngine const*
InnerChainRegistry::first() const
{
    if (order_.empty())
        return nullptr;
    return get(order_.front());
}

bool
InnerChainRegistry::startAll()
{
    bool allOk = true;
    for (auto id : order_)
    {
        auto* e = get(id);
        if (e && !e->start())
            allOk = false;
    }
    return allOk;
}

void
InnerChainRegistry::stopAll()
{
    for (auto it = order_.rbegin(); it != order_.rend(); ++it)
    {
        auto* e = get(*it);
        if (e)
            e->stop();
    }
}

json::Value
InnerChainRegistry::getJson(bool full) const
{
    json::Value ret(json::ObjectValue);
    ret["chain_count"] = static_cast<int>(engines_.size());

    json::Value chains(json::ArrayValue);
    for (auto id : order_)
    {
        auto const* e = get(id);
        if (e)
            chains.append(e->getJson(full));
    }
    ret["chains"] = std::move(chains);
    return ret;
}

}  // namespace inner
}  // namespace xrpl
