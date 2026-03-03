#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>

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
#include <string>
#include <thread>
#include <vector>

namespace xrpl {

class Application;

/** Embedded mainnet watcher that connects via WebSocket to mainnet nodes,
    monitors the vault address for incoming Payments with OperationLimit,
    collects validation signatures, and builds XPOPs in-memory.

    Replaces the external wietsewind/xpop Docker container.
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

    static constexpr std::size_t kMaxLedgerHistory = 256;
    static constexpr std::size_t kMaxCompletedHistory = 1024;
};

}  // namespace xrpl
