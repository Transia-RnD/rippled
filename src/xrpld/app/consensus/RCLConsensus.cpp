#include <xrpld/app/consensus/RCLConsensus.h>
#include <xrpld/app/consensus/RCLValidations.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/InboundLedgers.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LocalTxs.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
#include <xrpld/app/misc/NegativeUNLVote.h>
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/consensus/LedgerTiming.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/overlay/predicates.h>

#include <xrpl/basics/random.h>
#include <xrpl/beast/core/LexicalCast.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/core/HashRouter.h>
#include <xrpl/ledger/AmendmentTable.h>
#include <xrpl/protocol/BuildInfo.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/tx/transactors/Import/ExportPaymentBuilder.h>
#include <xrpl/server/LoadFeeTrack.h>
#include <xrpl/server/NetworkOPs.h>

#include <algorithm>
#include <iomanip>
#include <mutex>

namespace xrpl {

RCLConsensus::RCLConsensus(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    Consensus<Adaptor>::clock_type const& clock,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : adaptor_(
          app,
          std::move(feeVote),
          ledgerMaster,
          localTxs,
          inboundTransactions,
          validatorKeys,
          journal)
    , consensus_(clock, adaptor_, journal)
    , j_(journal)
{
}

RCLConsensus::Adaptor::Adaptor(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : app_(app)
    , feeVote_(std::move(feeVote))
    , ledgerMaster_(ledgerMaster)
    , localTxs_(localTxs)
    , inboundTransactions_{inboundTransactions}
    , j_(journal)
    , validatorKeys_(validatorKeys)
    , valCookie_(1 + rand_int(crypto_prng(), std::numeric_limits<std::uint64_t>::max() - 1))
    , nUnlVote_(validatorKeys_.nodeID, j_)
{
    XRPL_ASSERT(valCookie_, "xrpl::RCLConsensus::Adaptor::Adaptor : nonzero cookie");

    JLOG(j_.info()) << "Consensus engine started (cookie: " + std::to_string(valCookie_) + ")";

    if (validatorKeys_.nodeID != beast::zero && validatorKeys_.keys)
    {
        std::stringstream ss;

        JLOG(j_.info()) << "Validator identity: "
                        << toBase58(TokenType::NodePublic, validatorKeys_.keys->masterPublicKey);

        if (validatorKeys_.keys->masterPublicKey != validatorKeys_.keys->publicKey)
        {
            JLOG(j_.debug()) << "Validator ephemeral signing key: "
                             << toBase58(TokenType::NodePublic, validatorKeys_.keys->publicKey)
                             << " (seq: " << std::to_string(validatorKeys_.sequence) << ")";
        }
    }
}

std::optional<RCLCxLedger>
RCLConsensus::Adaptor::acquireLedger(LedgerHash const& hash)
{
    // we need to switch the ledger we're working from
    auto built = ledgerMaster_.getLedgerByHash(hash);
    if (!built)
    {
        if (acquiringLedger_ != hash)
        {
            // need to start acquiring the correct consensus LCL
            JLOG(j_.warn()) << "Need consensus ledger " << hash;

            // Tell the ledger acquire system that we need the consensus ledger
            acquiringLedger_ = hash;

            app_.getJobQueue().addJob(jtADVANCE, "GetConsL1", [id = hash, &app = app_, this]() {
                JLOG(j_.debug()) << "JOB advanceLedger getConsensusLedger1 started";
                app.getInboundLedgers().acquireAsync(id, 0, InboundLedger::Reason::CONSENSUS);
            });
        }
        return std::nullopt;
    }

    XRPL_ASSERT(
        !built->open() && built->isImmutable(),
        "xrpl::RCLConsensus::Adaptor::acquireLedger : valid ledger state");
    XRPL_ASSERT(
        built->header().hash == hash,
        "xrpl::RCLConsensus::Adaptor::acquireLedger : ledger hash match");

    // Notify inbound transactions of the new ledger sequence number
    inboundTransactions_.newRound(built->header().seq);

    return RCLCxLedger(built);
}

void
RCLConsensus::Adaptor::share(RCLCxPeerPos const& peerPos)
{
    protocol::TMProposeSet prop;

    auto const& proposal = peerPos.proposal();

    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());

    prop.set_currenttxhash(proposal.position().begin(), proposal.position().size());
    prop.set_previousledger(proposal.prevLedger().begin(), proposal.position().size());

    auto const pk = peerPos.publicKey().slice();
    prop.set_nodepubkey(pk.data(), pk.size());

    auto const sig = peerPos.signature();
    prop.set_signature(sig.data(), sig.size());

    app_.overlay().relay(prop, peerPos.suppressionID(), peerPos.publicKey());
}

void
RCLConsensus::Adaptor::share(RCLCxTx const& tx)
{
    // If we didn't relay this transaction recently, relay it to all peers
    if (app_.getHashRouter().shouldRelay(tx.id()))
    {
        JLOG(j_.debug()) << "Relaying disputed tx " << tx.id();
        auto const slice = tx.tx_->slice();
        protocol::TMTransaction msg;
        msg.set_rawtransaction(slice.data(), slice.size());
        msg.set_status(protocol::tsNEW);
        msg.set_receivetimestamp(app_.timeKeeper().now().time_since_epoch().count());
        static std::set<Peer::id_t> skip{};
        app_.overlay().relay(tx.id(), msg, skip);
    }
    else
    {
        JLOG(j_.debug()) << "Not relaying disputed tx " << tx.id();
    }
}
void
RCLConsensus::Adaptor::propose(RCLCxPeerPos::Proposal const& proposal)
{
    JLOG(j_.trace()) << (proposal.isBowOut() ? "We bow out: " : "We propose: ")
                     << xrpl::to_string(proposal.prevLedger()) << " -> "
                     << xrpl::to_string(proposal.position());

    protocol::TMProposeSet prop;

    prop.set_currenttxhash(proposal.position().begin(), proposal.position().size());
    prop.set_previousledger(proposal.prevLedger().begin(), proposal.prevLedger().size());
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());

    if (!validatorKeys_.keys)
    {
        JLOG(j_.warn()) << "RCLConsensus::Adaptor::propose: ValidatorKeys "
                           "not set: \n";
        return;
    }

    auto const& keys = *validatorKeys_.keys;

    prop.set_nodepubkey(keys.publicKey.data(), keys.publicKey.size());

    auto sig = signDigest(keys.publicKey, keys.secretKey, proposal.signingHash());

    prop.set_signature(sig.data(), sig.size());

    auto const suppression = proposalUniqueId(
        proposal.position(),
        proposal.prevLedger(),
        proposal.proposeSeq(),
        proposal.closeTime(),
        keys.publicKey,
        sig);

    app_.getHashRouter().addSuppression(suppression);

    app_.overlay().broadcast(prop);
}

void
RCLConsensus::Adaptor::share(RCLTxSet const& txns)
{
    inboundTransactions_.giveSet(txns.id(), txns.map_, false);
}

std::optional<RCLTxSet>
RCLConsensus::Adaptor::acquireTxSet(RCLTxSet::ID const& setId)
{
    if (auto txns = inboundTransactions_.getSet(setId, true))
    {
        return RCLTxSet{std::move(txns)};
    }
    return std::nullopt;
}

bool
RCLConsensus::Adaptor::hasOpenTransactions() const
{
    return !app_.openLedger().empty();
}

std::size_t
RCLConsensus::Adaptor::proposersValidated(LedgerHash const& h) const
{
    return app_.getValidations().numTrustedForLedger(h);
}

std::size_t
RCLConsensus::Adaptor::proposersFinished(RCLCxLedger const& ledger, LedgerHash const& h) const
{
    RCLValidations& vals = app_.getValidations();
    return vals.getNodesAfter(RCLValidatedLedger(ledger.ledger_, vals.adaptor().journal()), h);
}

uint256
RCLConsensus::Adaptor::getPrevLedger(
    uint256 ledgerID,
    RCLCxLedger const& ledger,
    ConsensusMode mode)
{
    RCLValidations& vals = app_.getValidations();
    uint256 netLgr = vals.getPreferred(
        RCLValidatedLedger{ledger.ledger_, vals.adaptor().journal()},
        ledgerMaster_.getValidLedgerIndex());

    if (netLgr != ledgerID)
    {
        if (mode != ConsensusMode::wrongLedger)
            app_.getOPs().consensusViewChange();

        JLOG(j_.debug()) << Json::Compact(app_.getValidations().getJsonTrie());
    }

    return netLgr;
}

auto
RCLConsensus::Adaptor::onClose(
    RCLCxLedger const& ledger,
    NetClock::time_point const& closeTime,
    ConsensusMode mode) -> Result
{
    bool const wrongLCL = mode == ConsensusMode::wrongLedger;
    bool const proposing = mode == ConsensusMode::proposing;

    notify(protocol::neCLOSING_LEDGER, ledger, !wrongLCL);

    auto const& prevLedger = ledger.ledger_;

    ledgerMaster_.applyHeldTransactions();
    // Tell the ledger master not to acquire the ledger we're probably building
    ledgerMaster_.setBuildingLedger(prevLedger->header().seq + 1);

    auto initialLedger = app_.openLedger().current();

    auto initialSet = std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    initialSet->setUnbacked();

    // Build SHAMap containing all transactions in our open ledger
    for (auto const& tx : initialLedger->txs)
    {
        JLOG(j_.trace()) << "Adding open ledger TX " << tx.first->getTransactionID();
        Serializer s(2048);
        tx.first->add(s);
        initialSet->addItem(
            SHAMapNodeType::tnTRANSACTION_NM,
            make_shamapitem(tx.first->getTransactionID(), s.slice()));
    }

    // Add pseudo-transactions to the set
    if (app_.config().standalone() || (proposing && !wrongLCL))
    {
        if (prevLedger->isFlagLedger())
        {
            // previous ledger was flag ledger, add fee and amendment
            // pseudo-transactions
            auto validations =
                app_.validators().negativeUNLFilter(app_.getValidations().getTrustedForLedger(
                    prevLedger->header().parentHash, prevLedger->seq() - 1));
            if (validations.size() >= app_.validators().quorum())
            {
                feeVote_->doVoting(prevLedger, validations, initialSet);
                app_.getAmendmentTable().doVoting(prevLedger, validations, initialSet, j_);
            }
        }
        else if (prevLedger->isVotingLedger())
        {
            // previous ledger was a voting ledger,
            // so the current consensus session is for a flag ledger,
            // add negative UNL pseudo-transactions
            nUnlVote_.doVoting(
                prevLedger,
                app_.validators().getTrustedMasterKeys(),
                app_.getValidations(),
                initialSet);
        }
    }

    // Now we need an immutable snapshot
    initialSet = initialSet->snapShot(false);

    if (!wrongLCL)
    {
        LedgerIndex const seq = prevLedger->header().seq + 1;
        RCLCensorshipDetector<TxID, LedgerIndex>::TxIDSeqVec proposed;

        initialSet->visitLeaves(
            [&proposed, seq](boost::intrusive_ptr<SHAMapItem const> const& item) {
                proposed.emplace_back(item->key(), seq);
            });

        censorshipDetector_.propose(std::move(proposed));
    }

    // Needed because of the move below.
    auto const setHash = initialSet->getHash().as_uint256();

    return Result{
        std::move(initialSet),
        RCLCxPeerPos::Proposal{
            initialLedger->header().parentHash,
            RCLCxPeerPos::Proposal::seqJoin,
            setHash,
            closeTime,
            app_.timeKeeper().closeTime(),
            validatorKeys_.nodeID}};
}

void
RCLConsensus::Adaptor::onForceAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value&& consensusJson)
{
    doAccept(result, prevLedger, closeResolution, rawCloseTimes, mode, std::move(consensusJson));
}

void
RCLConsensus::Adaptor::onAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value&& consensusJson,
    bool const validating)
{
    app_.getJobQueue().addJob(
        jtACCEPT, "AcceptLedger", [=, this, cj = std::move(consensusJson)]() mutable {
            // Note that no lock is held or acquired during this job.
            // This is because generic Consensus guarantees that once a ledger
            // is accepted, the consensus results and capture by reference state
            // will not change until startRound is called (which happens via
            // endConsensus).
            RclConsensusLogger clog("onAccept", validating, j_);
            this->doAccept(result, prevLedger, closeResolution, rawCloseTimes, mode, std::move(cj));
            this->app_.getOPs().endConsensus(clog.ss());
        });
}

void
RCLConsensus::Adaptor::doAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value&& consensusJson)
{
    prevProposers_ = result.proposers;
    prevRoundTime_ = result.roundTime.read();

    bool closeTimeCorrect;

    bool const proposing = mode == ConsensusMode::proposing;
    bool const haveCorrectLCL = mode != ConsensusMode::wrongLedger;
    bool const consensusFail = result.state == ConsensusState::MovedOn;

    auto consensusCloseTime = result.position.closeTime();

    if (consensusCloseTime == NetClock::time_point{})
    {
        // We agreed to disagree on the close time
        using namespace std::chrono_literals;
        consensusCloseTime = prevLedger.closeTime() + 1s;
        closeTimeCorrect = false;
    }
    else
    {
        // We agreed on a close time
        consensusCloseTime =
            effCloseTime(consensusCloseTime, closeResolution, prevLedger.closeTime());
        closeTimeCorrect = true;
    }

    JLOG(j_.debug()) << "Report: Prop=" << (proposing ? "yes" : "no")
                     << " val=" << (validating_ ? "yes" : "no")
                     << " corLCL=" << (haveCorrectLCL ? "yes" : "no")
                     << " fail=" << (consensusFail ? "yes" : "no");
    JLOG(j_.debug()) << "Report: Prev = " << prevLedger.id() << ":" << prevLedger.seq();

    //--------------------------------------------------------------------------
    std::set<TxID> failed;

    // We want to put transactions in an unpredictable but deterministic order:
    // we use the hash of the set.
    //
    // FIXME: Use a std::vector and a custom sorter instead of CanonicalTXSet?
    CanonicalTXSet retriableTxs{result.txns.map_->getHash().as_uint256()};

    JLOG(j_.debug()) << "Building canonical tx set: " << retriableTxs.key();

    for (auto const& item : *result.txns.map_)
    {
        try
        {
            retriableTxs.insert(std::make_shared<STTx const>(SerialIter{item.slice()}));
            JLOG(j_.debug()) << "    Tx: " << item.key();
        }
        catch (std::exception const& ex)
        {
            failed.insert(item.key());
            JLOG(j_.warn()) << "    Tx: " << item.key() << " throws: " << ex.what();
        }
    }

    auto built = buildLCL(
        prevLedger,
        retriableTxs,
        consensusCloseTime,
        closeTimeCorrect,
        closeResolution,
        result.roundTime.read(),
        failed);

    auto const newLCLHash = built.id();
    JLOG(j_.debug()) << "Built ledger #" << built.seq() << ": " << newLCLHash;

    // Tell directly connected peers that we have a new LCL
    notify(protocol::neACCEPTED_LEDGER, built, haveCorrectLCL);

    // As long as we're in sync with the network, attempt to detect attempts
    // at censorship of transaction by tracking which ones don't make it in
    // after a period of time.
    if (haveCorrectLCL && result.state == ConsensusState::Yes)
    {
        std::vector<TxID> accepted;

        result.txns.map_->visitLeaves(
            [&accepted](boost::intrusive_ptr<SHAMapItem const> const& item) {
                accepted.push_back(item->key());
            });

        // Track all the transactions which failed or were marked as retriable
        for (auto const& r : retriableTxs)
            failed.insert(r.first.getTXID());

        censorshipDetector_.check(
            std::move(accepted),
            [curr = built.seq(), j = app_.journal("CensorshipDetector"), &failed](
                uint256 const& id, LedgerIndex seq) {
                if (failed.count(id))
                    return true;

                auto const wait = curr - seq;

                if (wait && (wait % censorshipWarnInternal == 0))
                {
                    std::ostringstream ss;
                    ss << "Potential Censorship: Eligible tx " << id
                       << ", which we are tracking since ledger " << seq
                       << " has not been included as of ledger " << curr << ".";

                    JLOG(j.warn()) << ss.str();
                }

                return false;
            });
    }

    if (validating_)
        validating_ = ledgerMaster_.isCompatible(*built.ledger_, j_.warn(), "Not validating");

    if (validating_ && !consensusFail && app_.getValidations().canValidateSeq(built.seq()))
    {
        validate(built, result.txns, proposing);
        JLOG(j_.info()) << "CNF Val " << newLCLHash;
    }
    else
        JLOG(j_.info()) << "CNF buildLCL " << newLCLHash;

    // See if we can accept a ledger as fully-validated
    ledgerMaster_.consensusBuilt(built.ledger_, result.txns.id(), std::move(consensusJson));

    //-------------------------------------------------------------------------
    {
        // Apply disputed transactions that didn't get in
        //
        // The first crack of transactions to get into the new
        // open ledger goes to transactions proposed by a validator
        // we trust but not included in the consensus set.
        //
        // These are done first because they are the most likely
        // to receive agreement during consensus. They are also
        // ordered logically "sooner" than transactions not mentioned
        // in the previous consensus round.
        //
        bool anyDisputes = false;
        for (auto const& [_, dispute] : result.disputes)
        {
            (void)_;
            if (!dispute.getOurVote())
            {
                // we voted NO
                try
                {
                    JLOG(j_.debug()) << "Test applying disputed transaction that did"
                                     << " not get in " << dispute.tx().id();

                    SerialIter sit(dispute.tx().tx_->slice());
                    auto txn = std::make_shared<STTx const>(sit);

                    // Disputed pseudo-transactions that were not accepted
                    // can't be successfully applied in the next ledger
                    if (isPseudoTx(*txn))
                        continue;

                    retriableTxs.insert(txn);

                    anyDisputes = true;
                }
                catch (std::exception const& ex)
                {
                    JLOG(j_.debug()) << "Failed to apply transaction we voted "
                                        "NO on. Exception: "
                                     << ex.what();
                }
            }
        }

        // Build new open ledger
        std::unique_lock lock{app_.getMasterMutex(), std::defer_lock};
        std::unique_lock sl{ledgerMaster_.peekMutex(), std::defer_lock};
        std::lock(lock, sl);

        auto const lastVal = ledgerMaster_.getValidatedLedger();
        std::optional<Rules> rules;
        if (lastVal)
            rules = makeRulesGivenLedger(*lastVal, app_.config().features);
        else
            rules.emplace(app_.config().features);
        app_.openLedger().accept(
            app_,
            *rules,
            built.ledger_,
            localTxs_.getTxSet(),
            anyDisputes,
            retriableTxs,
            tapNONE,
            "consensus",
            [&](OpenView& view, beast::Journal j) {
                // Stuff the ledger with transactions from the queue.
                return app_.getTxQ().accept(app_, view);
            });

        // Signal a potential fee change to subscribers after the open ledger
        // is created
        app_.getOPs().reportFeeChange();
    }

    //-------------------------------------------------------------------------
    {
        ledgerMaster_.switchLCL(built.ledger_);

        // Do these need to exist?
        XRPL_ASSERT(
            ledgerMaster_.getClosedLedger()->header().hash == built.id(),
            "xrpl::RCLConsensus::Adaptor::doAccept : ledger hash match");
        XRPL_ASSERT(
            app_.openLedger().current()->header().parentHash == built.id(),
            "xrpl::RCLConsensus::Adaptor::doAccept : parent hash match");
    }

    //-------------------------------------------------------------------------
    // we entered the round with the network,
    // see how close our close time is to other node's
    //  close time reports, and update our clock.
    if ((mode == ConsensusMode::proposing || mode == ConsensusMode::observing) && !consensusFail)
    {
        auto closeTime = rawCloseTimes.self;

        JLOG(j_.info()) << "We closed at " << closeTime.time_since_epoch().count();
        using usec64_t = std::chrono::duration<std::uint64_t>;
        usec64_t closeTotal = std::chrono::duration_cast<usec64_t>(closeTime.time_since_epoch());
        int closeCount = 1;

        for (auto const& [t, v] : rawCloseTimes.peers)
        {
            JLOG(j_.info()) << std::to_string(v) << " time votes for "
                            << std::to_string(t.time_since_epoch().count());
            closeCount += v;
            closeTotal += std::chrono::duration_cast<usec64_t>(t.time_since_epoch()) * v;
        }

        closeTotal += usec64_t(closeCount / 2);  // for round to nearest
        closeTotal /= closeCount;

        // Use signed times since we are subtracting
        using duration = std::chrono::duration<std::int32_t>;
        using time_point = std::chrono::time_point<NetClock, duration>;
        auto offset = time_point{closeTotal} - std::chrono::time_point_cast<duration>(closeTime);
        JLOG(j_.info()) << "Our close offset is estimated at " << offset.count() << " ("
                        << closeCount << ")";

        app_.timeKeeper().adjustCloseTime(offset);
    }
}

void
RCLConsensus::Adaptor::notify(
    protocol::NodeEvent ne,
    RCLCxLedger const& ledger,
    bool haveCorrectLCL)
{
    protocol::TMStatusChange s;

    if (!haveCorrectLCL)
        s.set_newevent(protocol::neLOST_SYNC);
    else
        s.set_newevent(ne);

    s.set_ledgerseq(ledger.seq());
    s.set_networktime(app_.timeKeeper().now().time_since_epoch().count());
    s.set_ledgerhashprevious(
        ledger.parentID().begin(), std::decay_t<decltype(ledger.parentID())>::bytes);
    s.set_ledgerhash(ledger.id().begin(), std::decay_t<decltype(ledger.id())>::bytes);

    std::uint32_t uMin, uMax;
    if (!ledgerMaster_.getFullValidatedRange(uMin, uMax))
    {
        uMin = 0;
        uMax = 0;
    }
    else
    {
        // Don't advertise ledgers we're not willing to serve
        uMin = std::max(uMin, ledgerMaster_.getEarliestFetch());
    }
    s.set_firstseq(uMin);
    s.set_lastseq(uMax);
    app_.overlay().foreach(send_always(std::make_shared<Message>(s, protocol::mtSTATUS_CHANGE)));
    JLOG(j_.trace()) << "send status change to peer";
}

RCLCxLedger
RCLConsensus::Adaptor::buildLCL(
    RCLCxLedger const& previousLedger,
    CanonicalTXSet& retriableTxs,
    NetClock::time_point closeTime,
    bool closeTimeCorrect,
    NetClock::duration closeResolution,
    std::chrono::milliseconds roundTime,
    std::set<TxID>& failedTxs)
{
    std::shared_ptr<Ledger> built = [&]() {
        if (auto const replayData = ledgerMaster_.releaseReplay())
        {
            XRPL_ASSERT(
                replayData->parent()->header().hash == previousLedger.id(),
                "xrpl::RCLConsensus::Adaptor::buildLCL : parent hash match");
            return buildLedger(*replayData, tapNONE, app_, j_);
        }
        return buildLedger(
            previousLedger.ledger_,
            closeTime,
            closeTimeCorrect,
            closeResolution,
            app_,
            retriableTxs,
            failedTxs,
            j_);
    }();

    // Update fee computations based on accepted txs
    using namespace std::chrono_literals;
    app_.getTxQ().processClosedLedger(app_, *built, roundTime > 5s);

    // And stash the ledger in the ledger master
    if (ledgerMaster_.storeLedger(built))
        JLOG(j_.debug()) << "Consensus built ledger we already had";
    else if (app_.getInboundLedgers().find(built->header().hash))
        JLOG(j_.debug()) << "Consensus built ledger we were acquiring";
    else
        JLOG(j_.debug()) << "Consensus built new ledger";
    return RCLCxLedger{std::move(built)};
}

void
RCLConsensus::Adaptor::validate(RCLCxLedger const& ledger, RCLTxSet const& txns, bool proposing)
{
    using namespace std::chrono_literals;

    auto validationTime = app_.timeKeeper().closeTime();
    if (validationTime <= lastValidationTime_)
        validationTime = lastValidationTime_ + 1s;
    lastValidationTime_ = validationTime;

    if (!validatorKeys_.keys)
    {
        JLOG(j_.warn()) << "RCLConsensus::Adaptor::validate: ValidatorKeys "
                           "not set\n";
        return;
    }

    auto const& keys = *validatorKeys_.keys;

    auto v = std::make_shared<STValidation>(
        lastValidationTime_,
        keys.publicKey,
        keys.secretKey,
        validatorKeys_.nodeID,
        [&](STValidation& v) {
            v.setFieldH256(sfLedgerHash, ledger.id());
            v.setFieldH256(sfConsensusHash, txns.id());

            v.setFieldU32(sfLedgerSequence, ledger.seq());

            if (proposing)
                v.setFlag(vfFullValidation);

            // Attest to the hash of what we consider to be the last fully
            // validated ledger. This may be the hash of the ledger we are
            // validating here, and that's fine.
            if (auto const vl = ledgerMaster_.getValidatedLedger())
                v.setFieldH256(sfValidatedHash, vl->header().hash);

            v.setFieldU64(sfCookie, valCookie_);

            // Report our server version every flag ledger:
            if (ledger.ledger_->isVotingLedger())
                v.setFieldU64(sfServerVersion, BuildInfo::getEncodedVersion());

            // Report our load
            {
                auto const& ft = app_.getFeeTrack();
                auto const fee = std::max(ft.getLocalFee(), ft.getClusterFee());
                if (fee > ft.getLoadBase())
                    v.setFieldU32(sfLoadFee, fee);
            }

            // If the next ledger is a flag ledger, suggest fee changes and
            // new features:
            if (ledger.ledger_->isVotingLedger())
            {
                // Fees:
                feeVote_->doValidation(ledger.ledger_->fees(), ledger.ledger_->rules(), v);

                // Amendments
                // FIXME: pass `v` and have the function insert the array
                // directly?
                auto const amendments =
                    app_.getAmendmentTable().doValidation(getEnabledAmendments(*ledger.ledger_));

                if (!amendments.empty())
                    v.setFieldV256(sfAmendments, STVector256(sfAmendments, amendments));
            }
        });

    auto const serialized = v->getSerialized();

    // suppress it if we receive it
    app_.getHashRouter().addSuppression(sha512Half(makeSlice(serialized)));

    handleNewValidation(app_, v, "local");

    // Broadcast to all our peers:
    protocol::TMValidation val;
    val.set_validation(serialized.data(), serialized.size());
    app_.overlay().broadcast(val);

    // Publish to all our subscribers:
    app_.getOPs().pubValidation(v);

    // Sign export records for validator-signed exports
    if (ledger.ledger_->rules().enabled(featureImportExport))
    {
        signExportRecords(ledger, txns, keys);

        // At flag ledgers, check for UNL changes that require
        // updating the mainnet vault's SignerList, and check
        // if tickets need replenishment
        if (ledger.ledger_->isVotingLedger())
        {
            checkSignerListRotation(ledger, keys);
            checkTicketReplenishment(ledger, keys);
        }
    }
}

void
RCLConsensus::Adaptor::signExportRecords(
    RCLCxLedger const& ledger,
    RCLTxSet const& txns,
    ValidatorKeys::Keys const& keys)
{
    auto const& l = *ledger.ledger_;

    // Read VaultState for quorum and signer count
    auto const sleVault = l.read(keylet::exportVaultState());
    if (!sleVault)
        return;

    auto const quorum = sleVault->getFieldU32(sfExportQuorum);
    auto const signerCount = sleVault->getFieldU32(sfSignerCount);

    // Derive our signer AccountID from our signing (ephemeral) public key
    auto const signerAccountID = calcAccountID(keys.publicKey);

    // Scan the consensus transaction set for Export transactions
    txns.map_->visitLeaves(
        [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
            try
            {
                SerialIter sit(item->slice());
                auto const stx = std::make_shared<STTx const>(sit);

                if (stx->getTxnType() != ttEXPORT)
                    return;

                // Check if this transaction succeeded by reading
                // the transaction result from the built ledger
                auto const [tx, meta] = l.txRead(item->key());
                if (!meta)
                    return;

                // Check the transaction result
                auto const result =
                    meta->getFieldU8(sfTransactionResult);
                if (result != tesSUCCESS)
                    return;

                auto const account = (*tx)[sfAccount];
                auto const destination = (*tx)[sfDestination];
                auto const amount = (*tx)[sfAmount];

                // Find the ExportRecord created by this transaction.
                // The metadata contains CreatedNode entries.
                // Look through metadata for the created ExportRecord.
                if (!meta->isFieldPresent(sfAffectedNodes))
                    return;

                auto const& affected =
                    meta->getFieldArray(sfAffectedNodes);

                for (auto const& node : affected)
                {
                    if (node.getFieldU16(sfLedgerEntryType) !=
                        ltEXPORT_RECORD)
                        continue;

                    // Check if this is a CreatedNode
                    if (!node.isFieldPresent(sfNewFields))
                        continue;

                    auto const& newFields =
                        node.peekAtField(sfNewFields)
                            .downcast<STObject>();

                    if (!newFields.isFieldPresent(sfTicketSequence))
                        continue;

                    auto const ticketSeq =
                        newFields.getFieldU32(sfTicketSequence);
                    auto const exportSeq =
                        newFields.getFieldU32(sfExportSequence);

                    // Build deterministic mainnet Payment
                    ExportPaymentParams params;
                    params.vaultAddress =
                        *app_.getImportVaultAddress();
                    params.destination = destination;
                    params.amount = amount;
                    params.ticketSeq = ticketSeq;
                    params.signerCount = signerCount;

                    if (tx->isFieldPresent(sfDestinationTag))
                        params.destinationTag =
                            tx->getFieldU32(sfDestinationTag);

                    auto const payment = buildExportPayment(params);

                    // Compute per-signer multisig hash and sign
                    auto const msHash = exportPaymentMultiSignHash(
                        payment, signerAccountID);
                    auto const sig = signDigest(
                        keys.publicKey, keys.secretKey, msHash);

                    // Register in our local collector
                    app_.getExportSignatureCollector().registerExport(
                        account,
                        exportSeq,
                        params,
                        quorum,
                        ledger.seq());

                    // Build and broadcast the overlay message
                    protocol::TMExportSignature msg;
                    msg.set_exportaccount(
                        account.data(), account.size());
                    msg.set_exportsequence(exportSeq);
                    msg.set_validatorkey(
                        keys.publicKey.data(),
                        keys.publicKey.size());
                    msg.set_signature(sig.data(), sig.size());
                    msg.set_ledgersequence(ledger.seq());

                    // Suppress our own signature in the hash router
                    auto const suppKey = sha512Half(
                        Slice(account.data(), account.size()),
                        exportSeq,
                        keys.publicKey.slice());
                    app_.getHashRouter().addSuppression(suppKey);

                    app_.overlay().broadcast(msg);

                    // Also add our own signature to the collector
                    auto msgPtr = std::make_shared<
                        protocol::TMExportSignature>(msg);
                    app_.getExportSignatureCollector()
                        .onExportSignature(msgPtr);

                    JLOG(j_.info())
                        << "Signed export " << account << ":"
                        << exportSeq << " ticket=" << ticketSeq;
                }
            }
            catch (std::exception const& ex)
            {
                JLOG(j_.warn())
                    << "Error signing export: " << ex.what();
            }
        });
}

void
RCLConsensus::Adaptor::checkSignerListRotation(
    RCLCxLedger const& ledger,
    ValidatorKeys::Keys const& keys)
{
    auto const& l = *ledger.ledger_;

    auto const sleVault = l.read(keylet::exportVaultState());
    if (!sleVault)
        return;

    // Get current UNL validators' signing public keys
    auto const trustedKeys = app_.validators().getTrustedMasterKeys();
    if (trustedKeys.empty())
        return;

    // Compute the hash of the current UNL-derived signer list
    // Each validator's signing key -> AccountID
    std::vector<AccountID> signerAccounts;
    signerAccounts.reserve(trustedKeys.size());
    for (auto const& masterKey : trustedKeys)
    {
        // Get the current signing key for this validator
        auto const signingKey =
            app_.validatorManifests().getSigningKey(masterKey);
        if (signingKey)
            signerAccounts.push_back(calcAccountID(*signingKey));
    }

    // Sort by AccountID for deterministic ordering
    std::sort(signerAccounts.begin(), signerAccounts.end());

    // Compute hash of the signer list
    Serializer s;
    for (auto const& acct : signerAccounts)
        s.addBitString(acct);
    auto const currentHash = sha512Half(s.slice());

    // Compare with stored hash
    auto const storedHash =
        sleVault->isFieldPresent(sfSignerListHash)
        ? sleVault->getFieldH256(sfSignerListHash)
        : uint256{};

    if (currentHash == storedHash)
        return;  // No change needed

    JLOG(j_.info())
        << "UNL signer list changed, need SignerListSet update. "
        << "Validators: " << signerAccounts.size()
        << " old hash: " << storedHash
        << " new hash: " << currentHash;

    // Build the SignerListSet transaction
    auto const signerCount = sleVault->getFieldU32(sfSignerCount);
    auto const nextTicket = sleVault->getFieldU32(sfNextTicketSeq);
    auto const maxTicket = sleVault->getFieldU32(sfMaxTicketSeq);

    if (nextTicket > maxTicket)
    {
        JLOG(j_.warn())
            << "No tickets available for SignerListSet";
        return;
    }

    auto const newQuorum = static_cast<std::uint32_t>(
        std::ceil(signerAccounts.size() * 0.8));

    SignerListSetParams params;
    params.vaultAddress = *app_.getImportVaultAddress();
    params.ticketSeq = nextTicket;  // Use next available ticket
    params.signerCount = signerCount;
    params.quorum = newQuorum;
    params.signerAccounts = signerAccounts;

    auto const slsTx = buildSignerListSet(params);

    // Compute multisign hash and sign
    auto const signerAccountID = calcAccountID(keys.publicKey);
    auto const msHash =
        exportPaymentMultiSignHash(slsTx, signerAccountID);
    auto const sig =
        signDigest(keys.publicKey, keys.secretKey, msHash);

    // Broadcast via overlay using the same message type
    // The collector treats this as a special "management" export
    // with a reserved export sequence (UINT32_MAX - 1 for SignerListSet)
    protocol::TMExportSignature msg;
    msg.set_exportaccount(
        params.vaultAddress.data(), params.vaultAddress.size());
    msg.set_exportsequence(0xFFFFFFFE);  // Reserved for SignerListSet
    msg.set_validatorkey(
        keys.publicKey.data(), keys.publicKey.size());
    msg.set_signature(sig.data(), sig.size());
    msg.set_ledgersequence(ledger.seq());

    auto const suppKey = sha512Half(
        Slice(params.vaultAddress.data(), params.vaultAddress.size()),
        std::uint32_t(0xFFFFFFFE),
        keys.publicKey.slice());
    app_.getHashRouter().addSuppression(suppKey);

    app_.overlay().broadcast(msg);

    JLOG(j_.info())
        << "Broadcast SignerListSet signature for UNL rotation";
}

void
RCLConsensus::Adaptor::checkTicketReplenishment(
    RCLCxLedger const& ledger,
    ValidatorKeys::Keys const& keys)
{
    auto const& l = *ledger.ledger_;

    auto const sleVault = l.read(keylet::exportVaultState());
    if (!sleVault)
        return;

    auto const nextTicket = sleVault->getFieldU32(sfNextTicketSeq);
    auto const maxTicket = sleVault->getFieldU32(sfMaxTicketSeq);
    auto const signerCount = sleVault->getFieldU32(sfSignerCount);

    // Calculate remaining tickets
    auto const remaining =
        (maxTicket >= nextTicket) ? (maxTicket - nextTicket + 1) : 0u;

    // Replenish when less than 25% of max pool (250) remain
    // Reserve 2 tickets for management transactions (SignerListSet, TicketCreate)
    constexpr std::uint32_t replenishThreshold = 62;  // ~25% of 250
    constexpr std::uint32_t newTicketCount = 200;      // Replenish with a batch

    if (remaining > replenishThreshold)
        return;

    JLOG(j_.info())
        << "Ticket pool low (" << remaining
        << " remaining), initiating replenishment";

    if (nextTicket > maxTicket)
    {
        JLOG(j_.warn())
            << "No tickets available for TicketCreate";
        return;
    }

    // Use the last available ticket for the TicketCreate itself
    // (reserve the last ticket for this purpose)
    auto const ticketForCreate = maxTicket;

    TicketCreateParams params;
    params.vaultAddress = *app_.getImportVaultAddress();
    params.ticketSeq = ticketForCreate;
    params.signerCount = signerCount;
    params.ticketCount = newTicketCount;

    auto const tcTx = buildTicketCreate(params);

    // Compute multisign hash and sign
    auto const signerAccountID = calcAccountID(keys.publicKey);
    auto const msHash =
        exportPaymentMultiSignHash(tcTx, signerAccountID);
    auto const sig =
        signDigest(keys.publicKey, keys.secretKey, msHash);

    // Broadcast via overlay using reserved export sequence
    protocol::TMExportSignature msg;
    msg.set_exportaccount(
        params.vaultAddress.data(), params.vaultAddress.size());
    msg.set_exportsequence(0xFFFFFFFF);  // Reserved for TicketCreate
    msg.set_validatorkey(
        keys.publicKey.data(), keys.publicKey.size());
    msg.set_signature(sig.data(), sig.size());
    msg.set_ledgersequence(ledger.seq());

    auto const suppKey = sha512Half(
        Slice(params.vaultAddress.data(), params.vaultAddress.size()),
        std::uint32_t(0xFFFFFFFF),
        keys.publicKey.slice());
    app_.getHashRouter().addSuppression(suppKey);

    app_.overlay().broadcast(msg);

    JLOG(j_.info())
        << "Broadcast TicketCreate signature for pool replenishment";
}

void
RCLConsensus::Adaptor::onModeChange(ConsensusMode before, ConsensusMode after)
{
    JLOG(j_.info()) << "Consensus mode change before=" << to_string(before)
                    << ", after=" << to_string(after);

    // If we were proposing but aren't any longer, we need to reset the
    // censorship tracking to avoid bogus warnings.
    if ((before == ConsensusMode::proposing || before == ConsensusMode::observing) &&
        before != after)
        censorshipDetector_.reset();

    mode_ = after;
}

Json::Value
RCLConsensus::getJson(bool full) const
{
    Json::Value ret;
    {
        std::lock_guard _{mutex_};
        ret = consensus_.getJson(full);
    }
    ret["validating"] = adaptor_.validating();
    return ret;
}

void
RCLConsensus::timerEntry(
    NetClock::time_point const& now,
    std::unique_ptr<std::stringstream> const& clog)
{
    try
    {
        std::lock_guard _{mutex_};
        consensus_.timerEntry(now, clog);
    }
    catch (SHAMapMissingNode const& mn)
    {
        // This should never happen
        std::stringstream ss;
        ss << "During consensus timerEntry: " << mn.what();
        JLOG(j_.error()) << ss.str();
        CLOG(clog) << ss.str();
        Rethrow();
    }
}

void
RCLConsensus::gotTxSet(NetClock::time_point const& now, RCLTxSet const& txSet)
{
    try
    {
        std::lock_guard _{mutex_};
        consensus_.gotTxSet(now, txSet);
    }
    catch (SHAMapMissingNode const& mn)
    {
        // This should never happen
        JLOG(j_.error()) << "During consensus gotTxSet: " << mn.what();
        Rethrow();
    }
}

//! @see Consensus::simulate

void
RCLConsensus::simulate(
    NetClock::time_point const& now,
    std::optional<std::chrono::milliseconds> consensusDelay)
{
    std::lock_guard _{mutex_};
    consensus_.simulate(now, consensusDelay);
}

bool
RCLConsensus::peerProposal(NetClock::time_point const& now, RCLCxPeerPos const& newProposal)
{
    std::lock_guard _{mutex_};
    return consensus_.peerProposal(now, newProposal);
}

bool
RCLConsensus::Adaptor::preStartRound(RCLCxLedger const& prevLgr, hash_set<NodeID> const& nowTrusted)
{
    // We have a key, we do not want out of sync validations after a restart
    // and are not amendment blocked.
    validating_ = validatorKeys_.keys && prevLgr.seq() >= app_.getMaxDisallowedLedger() &&
        !app_.getOPs().isBlocked();

    // If we are not running in standalone mode and there's a configured UNL,
    // check to make sure that it's not expired.
    if (validating_ && !app_.config().standalone() && app_.validators().count())
    {
        auto const when = app_.validators().expires();

        if (!when || *when < app_.timeKeeper().now())
        {
            JLOG(j_.error()) << "Voluntarily bowing out of consensus process "
                                "because of an expired validator list.";
            validating_ = false;
        }
    }

    bool const synced = app_.getOPs().getOperatingMode() == OperatingMode::FULL;

    if (validating_)
    {
        JLOG(j_.info()) << "Entering consensus process, validating, synced="
                        << (synced ? "yes" : "no");
    }
    else
    {
        // Otherwise we just want to monitor the validation process.
        JLOG(j_.info()) << "Entering consensus process, watching, synced="
                        << (synced ? "yes" : "no");
    }

    // Notify inbound ledgers that we are starting a new round
    inboundTransactions_.newRound(prevLgr.seq());

    // Notify NegativeUNLVote that new validators are added
    if (!nowTrusted.empty())
        nUnlVote_.newValidators(prevLgr.seq() + 1, nowTrusted);

    // propose only if we're in sync with the network (and validating)
    return validating_ && synced;
}

bool
RCLConsensus::Adaptor::haveValidated() const
{
    return ledgerMaster_.haveValidated();
}

LedgerIndex
RCLConsensus::Adaptor::getValidLedgerIndex() const
{
    return ledgerMaster_.getValidLedgerIndex();
}

std::pair<std::size_t, hash_set<RCLConsensus::Adaptor::NodeKey_t>>
RCLConsensus::Adaptor::getQuorumKeys() const
{
    return app_.validators().getQuorumKeys();
}

std::size_t
RCLConsensus::Adaptor::laggards(
    Ledger_t::Seq const seq,
    hash_set<RCLConsensus::Adaptor::NodeKey_t>& trustedKeys) const
{
    return app_.getValidations().laggards(seq, trustedKeys);
}

bool
RCLConsensus::Adaptor::validator() const
{
    return validatorKeys_.keys.has_value();
}

void
RCLConsensus::Adaptor::updateOperatingMode(std::size_t const positions) const
{
    if (!positions && app_.getOPs().isFull())
        app_.getOPs().setMode(OperatingMode::CONNECTED);
}

void
RCLConsensus::startRound(
    NetClock::time_point const& now,
    RCLCxLedger::ID const& prevLgrId,
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowUntrusted,
    hash_set<NodeID> const& nowTrusted,
    std::unique_ptr<std::stringstream> const& clog)
{
    std::lock_guard _{mutex_};
    consensus_.startRound(
        now, prevLgrId, prevLgr, nowUntrusted, adaptor_.preStartRound(prevLgr, nowTrusted), clog);
}

RclConsensusLogger::RclConsensusLogger(char const* label, bool const validating, beast::Journal j)
    : j_(j)
{
    if (!validating && !j.info())
        return;
    start_ = std::chrono::steady_clock::now();
    ss_ = std::make_unique<std::stringstream>();
    header_ = "ConsensusLogger ";
    header_ += label;
    header_ += ": ";
}

RclConsensusLogger::~RclConsensusLogger()
{
    if (!ss_)
        return;
    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_);
    std::stringstream outSs;
    outSs << header_ << "duration " << (duration.count() / 1000) << '.' << std::setw(3)
          << std::setfill('0') << (duration.count() % 1000) << "s. " << ss_->str();
    j_.sink().writeAlways(beast::severities::kInfo, outSs.str());
}

}  // namespace xrpl
