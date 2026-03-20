#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/messages.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace xrpl {

class Application;
class MainnetPeerClient;

/** Embedded mainnet watcher that monitors mainnet for vault payments,
    collects validation signatures, and builds XPOPs in-memory.

    Supports two transport modes:
    1. Native peer protocol via MainnetPeerClient (preferred)
    2. WebSocket subscriptions (fallback)
*/
class MainnetWatcher
{
public:
    /** A ready-to-submit import with all data needed. */
    struct ReadyImport
    {
        AccountID sidechainAccount;     // Derived from inner tx signing key
        std::uint32_t importSequence;   // Inner tx sequence
        Json::Value xpop;               // Complete XPOP JSON blob
        uint256 mainnetTxHash;          // Hash of the mainnet tx
    };

    /** Per-ledger data stored in the ring buffer. */
    struct LedgerData
    {
        std::uint32_t ledgerIndex{0};
        Json::Value ledgerHeader;       // Ledger header fields for XPOP
        bool hasHeader{false};
    };

    /** Per-validation data. */
    struct ValidationEntry
    {
        std::string validatorKey;       // Base58 node public key
        std::string validationHex;      // Hex-encoded validation blob
    };

    /** Pending transaction waiting for XPOP construction. */
    struct PendingTx
    {
        Json::Value txBlob;             // Transaction blob (hex)
        Json::Value txMeta;             // Transaction metadata (hex)
        Json::Value txProof;            // Merkle proof
        std::uint32_t ledgerIndex{0};
        uint256 txHash;
    };

    /** Construct with optional peer protocol and/or WebSocket transport.
        If peerEndpoints is non-empty, native peer protocol is used (preferred).
        If wsUrls is non-empty, WebSocket is used as fallback. */
    MainnetWatcher(
        Application& app,
        std::vector<std::string> const& wsUrls,
        std::vector<std::string> const& peerEndpoints,
        std::uint32_t mainnetNetworkID,
        beast::Journal journal);

    /** Legacy constructor — WebSocket only. */
    MainnetWatcher(
        Application& app,
        std::vector<std::string> const& wsUrls,
        beast::Journal journal);

    ~MainnetWatcher();

    /** Start the watcher thread. */
    void
    start();

    /** Stop the watcher and join the thread. */
    void
    stop();

    /** Consume all ready imports (thread-safe).
        Returns and clears the list of imports that have complete XPOPs. */
    std::vector<ReadyImport>
    consumeReadyImports();

    /** Get a specific import by mainnet tx hash (thread-safe). */
    std::optional<ReadyImport>
    getImport(uint256 const& mainnetTxHash) const;

    /** Get status information for diagnostics. */
    Json::Value
    getStatus() const;

    /** Inject a ready import for testing purposes. Thread-safe. */
    void
    injectReadyImport(ReadyImport ri)
    {
        std::lock_guard lock(mutex_);
        readyImports_.push_back(std::move(ri));
    }

    /** Submit a signed transaction blob to mainnet via WebSocket.
        Thread-safe — queued for async delivery on the watcher's connection. */
    void
    submitTransaction(std::string const& txBlob);

    /** A confirmed vault outbound transaction on mainnet. */
    struct ConfirmedVaultTx
    {
        uint256 txHash;              // Mainnet tx hash
        std::uint32_t ledgerIndex;   // Ledger it was confirmed in
        std::string txType;          // "TicketCreate", "Payment", etc.
        std::uint32_t ticketCount;   // For TicketCreate: number of tickets
        std::uint32_t ticketSequence{0};  // For Payment: the TicketSequence used
        std::uint32_t newSequence;   // Post-tx account Sequence
    };

    /** Consume all confirmed vault transactions (thread-safe).
        Returns and clears the list. */
    std::vector<ConfirmedVaultTx>
    consumeConfirmedVaultTxs();

private:
    /** Main event loop running in the watcher thread. */
    void
    run();

    /** Attempt to connect to one of the configured mainnet WebSocket URLs. */
    void
    connect();

    /** Process an incoming WebSocket message from mainnet. */
    void
    onMessage(std::string const& msg);

    /** Handle a ledger subscription message. */
    void
    onLedger(Json::Value const& data);

    /** Handle a validation subscription message. */
    void
    onValidation(Json::Value const& data);

    /** Handle a transaction subscription message. */
    void
    onTransaction(Json::Value const& data);

    /** Try to build XPOPs for pending transactions. Called after each
        new ledger or validation arrives. */
    void
    tryBuildXPOPs();

    /** Check if a transaction is a Payment to our vault with correct
        OperationLimit. */
    bool
    isVaultPayment(Json::Value const& tx) const;

    /** Check if a transaction is an outbound tx from our vault account. */
    bool
    isVaultOutbound(Json::Value const& tx) const;

    /** Handle a confirmed vault outbound transaction. */
    void
    onVaultOutbound(Json::Value const& data);

    // -- Peer protocol callbacks (from MainnetPeerClient) --

    /** Handle a TMValidation from a mainnet peer. */
    void
    onPeerValidation(protocol::TMValidation const& m);

    /** Handle a TMStatusChange from a mainnet peer. */
    void
    onPeerStatusChange(protocol::TMStatusChange const& m);

    /** Handle a TMTransaction from a mainnet peer (mempool). */
    void
    onPeerTransaction(protocol::TMTransaction const& m);

    /** Handle a TMLedgerData response from a mainnet peer. */
    void
    onPeerLedgerData(protocol::TMLedgerData const& m);

    /** Process a liBASE response — extract ledger header. */
    void
    processLedgerBase(protocol::TMLedgerData const& m);

    /** Process a liTX_NODE response — continue SHAMap traversal. */
    void
    processTxNodes(protocol::TMLedgerData const& m);

    /** Check if an STTx is a vault payment (peer protocol version). */
    bool
    isVaultPaymentRaw(Slice txData) const;

    /** Handle a TMValidatorList from a mainnet peer. */
    void
    onPeerValidatorList(protocol::TMValidatorList const& m);

    /** Handle a TMValidatorListCollection from a mainnet peer. */
    void
    onPeerValidatorListCollection(
        protocol::TMValidatorListCollection const& m);

    // -- WebSocket transport (fallback) --

    /** Drain the submit queue over an open WebSocket (templated for TLS/plain). */
    template <class WsStream>
    void
    drainSubmitQueue(WsStream& ws);

    /** Resubmit pending submissions that haven't confirmed. */
    template <class WsStream>
    void
    resubmitPending(WsStream& ws);

    Application& app_;
    beast::Journal journal_;
    std::vector<std::string> wsUrls_;
    AccountID vaultAddress_;
    std::uint32_t networkID_{0};

    // Thread management
    std::atomic<bool> running_{false};
    std::thread thread_;

    // Ring buffer of recent ledger data (keyed by ledger index)
    mutable std::mutex mutex_;
    std::map<std::uint32_t, LedgerData> ledgers_;

    // Validations keyed by ledger index -> list of validator entries
    std::map<std::uint32_t, std::vector<ValidationEntry>> validations_;

    // Pending transactions waiting for XPOP construction
    std::map<uint256, PendingTx> pendingTxs_;

    // Ready imports with complete XPOPs
    std::vector<ReadyImport> readyImports_;

    // Completed tx hashes (for dedup)
    std::deque<uint256> completedTxHashes_;

    // Submit queue: signed tx blobs waiting to be sent to mainnet
    std::deque<std::string> submitQueue_;

    // Confirmed vault outbound transactions
    std::vector<ConfirmedVaultTx> confirmedVaultTxs_;

    // Pending submissions awaiting confirmation
    struct SubmissionState
    {
        std::string txBlob;
        std::uint32_t lastSubmitLedger{0};
        std::uint32_t retryCount{0};
    };
    std::map<uint256, SubmissionState> pendingSubmissions_;

    // Latest known mainnet ledger index (for retry timing)
    std::uint32_t latestLedgerIndex_{0};

    // -- Peer protocol transport --
    std::unique_ptr<MainnetPeerClient> peerClient_;
    std::vector<std::string> peerEndpoints_;
    std::uint32_t mainnetNetworkID_{0};

    // Tx hashes detected in mempool (via TMTransaction), pending confirmation
    std::set<uint256> mempoolDetected_;

    // SHAMap traversal state for finding confirmed tx data
    struct SHAMapTraversal
    {
        uint256 txHash;
        uint256 ledgerHash;
        std::uint32_t ledgerSeq{0};
        std::vector<std::string> proofPath;  // inner node data on path
        int depth{0};
    };
    std::map<uint256, SHAMapTraversal> activeTraversals_;

    // Dedup set for peer validations (hash of validation blob)
    std::set<uint256> recentValHashes_;
    std::uint32_t lastValPruneSeq_{0};

    // Stored UNL data received from mainnet peers (for XPOP validation.unl)
    struct StoredUNL
    {
        std::string publicKey;   // Hex-encoded VL master public key
        std::string manifest;    // Base64-encoded manifest
        std::string blob;        // Base64-encoded blob
        std::string signature;   // Hex-encoded signature
        std::uint32_t version{0};
    };
    std::vector<StoredUNL> storedUNLs_;

    static constexpr std::size_t kMaxLedgerHistory = 256;
    static constexpr std::size_t kMaxCompletedHistory = 1024;
    static constexpr std::uint32_t kRetryAfterLedgers = 10;
    static constexpr std::uint32_t kMaxRetries = 5;
};

}  // namespace xrpl
