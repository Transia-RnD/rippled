#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/messages.h>
#include <xrpl/tx/transactors/Import/ExportPaymentBuilder.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace xrpl {

class Application;

class ExportSignatureCollector
{
public:
    struct PendingExport
    {
        ExportPaymentParams params;
        STTx unsignedPayment;
        // Map: signerAccountID -> (validatorPubKey, signature bytes)
        std::map<AccountID, std::pair<PublicKey, Buffer>> signatures;
        std::uint32_t quorum{0};
        std::optional<STTx> assembledPayment;
        std::chrono::steady_clock::time_point created;
        std::uint32_t ledgerSequence{0};
    };

    ExportSignatureCollector(Application& app, beast::Journal journal);

    /** Process an incoming export signature from the overlay. */
    void
    onExportSignature(
        std::shared_ptr<protocol::TMExportSignature> const& m);

    /** Register a new export that needs signatures.
        Called by validators after observing a new ExportRecord
        in a validated ledger. */
    void
    registerExport(
        AccountID const& exportAccount,
        std::uint32_t exportSequence,
        ExportPaymentParams params,
        std::uint32_t quorum,
        std::uint32_t ledgerSequence);

    /** Get the status of a pending export for RPC. */
    Json::Value
    getExportStatus(
        AccountID const& account,
        std::uint32_t exportSeq) const;

    /** Get the assembled multisig payment blob, if quorum reached. */
    std::optional<STTx>
    getExportPayment(
        AccountID const& account,
        std::uint32_t exportSeq) const;

    /** Remove stale entries older than maxAge. */
    void
    pruneStale(std::chrono::seconds maxAge = std::chrono::seconds(3600));

private:
    using ExportKey = std::pair<AccountID, std::uint32_t>;

    /** Try to assemble the full multisig payment once quorum is reached. */
    void
    tryAssemble(PendingExport& pending);

    Application& app_;
    beast::Journal journal_;
    mutable std::mutex mutex_;
    std::map<ExportKey, PendingExport> pending_;
};

}  // namespace xrpl
