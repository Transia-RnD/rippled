#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>

#include <set>

namespace xrpl {

class Application;
class ReadView;
class SHAMap;

/** Generates ttEXPORT_CONFIRM pseudo-transactions every ledger close
    by consuming confirmed vault transactions from the MainnetWatcher.

    When a vault TicketCreate is confirmed on mainnet, this class builds
    a deterministic ttEXPORT_CONFIRM pseudo-tx that updates the sidechain's
    ExportVaultState (sfMaxTicketSeq, sfMainnetSequence).

    Each validator independently builds the same deterministic pseudo-tx
    from the same mainnet data, producing identical bytes and thus the
    same SHAMap key. Consensus naturally provides 80%+ quorum.
*/
class ExportConfirmVote
{
public:
    ExportConfirmVote(Application& app, beast::Journal journal);

    /** Called every ledger close to inject export confirm pseudo-txs
        into the initial consensus transaction set. */
    void
    doVoting(
        std::shared_ptr<ReadView const> const& prevLedger,
        std::shared_ptr<SHAMap> const& initialSet);

private:
    Application& app_;
    beast::Journal journal_;

    // Track which mainnet tx hashes we've already proposed
    std::set<uint256> proposedHashes_;
};

}  // namespace xrpl
