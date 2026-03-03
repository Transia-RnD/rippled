#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>

#include <set>

namespace xrpl {

class Application;
class ReadView;
class SHAMap;

/** Generates ttIMPORT_CREDIT pseudo-transactions every ledger close
    by consuming ready imports from the MainnetWatcher.

    Each validator independently builds the same deterministic
    ttIMPORT_CREDIT pseudo-transaction from the same mainnet data,
    producing identical bytes and thus the same SHAMap key.
    Consensus naturally provides 80%+ quorum.
*/
class ImportCreditVote
{
public:
    ImportCreditVote(Application& app, beast::Journal journal);

    /** Called every ledger close to inject import credit pseudo-txs
        into the initial consensus transaction set. */
    void
    doVoting(
        std::shared_ptr<ReadView const> const& prevLedger,
        std::shared_ptr<SHAMap> const& initialSet);

private:
    Application& app_;
    beast::Journal journal_;

    // Track which mainnet tx hashes we've already proposed
    // to avoid re-proposing
    std::set<uint256> proposedHashes_;
};

}  // namespace xrpl
