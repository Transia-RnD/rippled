#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/tx/transactors/Import/ExportPaymentBuilder.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace xrpl {

class Application;
class MainnetWatcher;

/** Collects export signatures from validators using two-phase verification.

    Phase 1 (early arrival): Signatures arrive via TMValidation before we
    have the transaction data cached. These are stored unverified, keyed
    by txnHash.

    Phase 2 (retroactive verification): When transaction data is stashed
    (after we process the export in a validated ledger), all pending
    unverified signatures for that txnHash are retroactively verified.
    Bad signatures are pruned.

    Keyed by txnHash (the hash of the deterministic unsigned mainnet
    Payment), with a reverse lookup from (AccountID, exportSeq) for RPC.
*/
class ExportSignatureCollector
{
public:
    /** Per-txnHash signature data. */
    struct UnverifiedSig
    {
        PublicKey validatorKey;
        Buffer signature;  // raw multisig signature bytes
    };

    struct TxnData
    {
        AccountID exportAccount;
        std::uint32_t exportSequence{0};
        ExportPaymentParams params;
        STTx unsignedPayment;
        std::uint32_t quorum{0};
        std::uint32_t ledgerSequence{0};
    };

    ExportSignatureCollector(Application& app, beast::Journal journal);

    /** Set the MainnetWatcher for auto-submission of assembled transactions. */
    void
    setMainnetWatcher(MainnetWatcher* watcher)
    {
        mainnetWatcher_ = watcher;
    }

    /** Process an incoming export signature extracted from a TMValidation.

        Two-phase: If txnData is already cached for this txnHash, the
        signature is verified immediately. Otherwise it is stored
        unverified and will be verified when stashTxnData is called.

        @param txnHash  Hash of the unsigned mainnet Payment
        @param signer   Serialized sfSigner STObject (contains sfAccount,
                        sfSigningPubKey, sfTxnSignature)
        @param validatorKey  The validator's public key (for identity check)
    */
    void
    onExportSignatureFromValidation(
        uint256 const& txnHash,
        Slice const& signer,
        PublicKey const& validatorKey);

    /** Cache transaction data for an export, enabling signature verification.

        Called by validators after computing the deterministic Payment for
        an ExportRecord. Retroactively verifies all pending unverified
        signatures for this txnHash, pruning failures.

        @param txnHash       Hash of the unsigned mainnet Payment
        @param account       The exporter's AccountID
        @param exportSeq     The export sequence number
        @param params        Parameters for building the Payment
        @param quorum        Required number of signatures
        @param ledgerSeq     Ledger containing the Export tx
    */
    void
    stashTxnData(
        uint256 const& txnHash,
        AccountID const& account,
        std::uint32_t exportSeq,
        ExportPaymentParams params,
        std::uint32_t quorum,
        std::uint32_t ledgerSeq);

    /** Check if quorum has been reached for a given txnHash. */
    bool
    hasQuorum(uint256 const& txnHash) const;

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

    /** Remove stale entries older than maxAge ledgers. */
    void
    pruneStale(
        std::uint32_t currentLedger,
        std::uint32_t maxAge = 256);

private:
    /** Try to assemble the full multisig payment once quorum is reached. */
    void
    tryAssemble(uint256 const& txnHash);

    /** Verify a single signature against cached txnData. */
    bool
    verifySig(
        TxnData const& data,
        AccountID const& signerAccountID,
        PublicKey const& validatorKey,
        Slice const& sig) const;

    beast::Journal journal_;
    MainnetWatcher* mainnetWatcher_{nullptr};
    mutable std::mutex mutex_;

    // Primary storage: txnHash -> verified signatures
    std::map<uint256, std::map<AccountID, STObject>> signatures_;

    // Transaction data cache: txnHash -> params + unsigned payment
    std::map<uint256, TxnData> txnData_;

    // Unverified signatures waiting for txnData: txnHash -> list
    std::map<uint256, std::vector<UnverifiedSig>> unverified_;

    // Reverse lookup: (AccountID, exportSeq) -> txnHash
    std::map<std::pair<AccountID, std::uint32_t>, uint256> exportKeyLookup_;

    // Assembled payments: txnHash -> completed multisig STTx
    std::map<uint256, STTx> assembled_;
};

}  // namespace xrpl
