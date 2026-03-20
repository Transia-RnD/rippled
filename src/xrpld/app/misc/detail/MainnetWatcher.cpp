#include <xrpld/app/misc/MainnetWatcher.h>
#include <xrpld/app/misc/MainnetPeerClient.h>

#include <xrpld/app/main/Application.h>

#include <xrpld/core/Config.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/server/Manifest.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <chrono>

namespace xrpl {

namespace beast_ns = boost::beast;
namespace websocket = beast_ns::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

MainnetWatcher::MainnetWatcher(
    Application& app,
    std::vector<std::string> const& wsUrls,
    std::vector<std::string> const& peerEndpoints,
    std::uint32_t mainnetNetworkID,
    beast::Journal journal)
    : app_(app)
    , journal_(journal)
    , wsUrls_(wsUrls)
    , peerEndpoints_(peerEndpoints)
    , mainnetNetworkID_(mainnetNetworkID)
{
    if (auto const va = app_.config().IMPORT_VAULT_ADDRESS)
        vaultAddress_ = *va;

    networkID_ = app_.config().NETWORK_ID;
}

MainnetWatcher::MainnetWatcher(
    Application& app,
    std::vector<std::string> const& wsUrls,
    beast::Journal journal)
    : app_(app), journal_(journal), wsUrls_(wsUrls)
{
    if (auto const va = app_.config().IMPORT_VAULT_ADDRESS)
        vaultAddress_ = *va;

    networkID_ = app_.config().NETWORK_ID;
}

MainnetWatcher::~MainnetWatcher()
{
    stop();
}

void
MainnetWatcher::start()
{
    if (vaultAddress_ == AccountID{})
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: No vault address configured, not starting";
        return;
    }

    // Prefer native peer protocol if configured
    if (!peerEndpoints_.empty())
    {
        MainnetPeerClient::Callbacks cb;
        cb.onValidation = [this](protocol::TMValidation const& m) {
            onPeerValidation(m);
        };
        cb.onStatusChange = [this](protocol::TMStatusChange const& m) {
            onPeerStatusChange(m);
        };
        cb.onTransaction = [this](protocol::TMTransaction const& m) {
            onPeerTransaction(m);
        };
        cb.onLedgerData = [this](protocol::TMLedgerData const& m) {
            onPeerLedgerData(m);
        };
        cb.onValidatorList = [this](protocol::TMValidatorList const& m) {
            onPeerValidatorList(m);
        };
        cb.onValidatorListCollection =
            [this](protocol::TMValidatorListCollection const& m) {
                onPeerValidatorListCollection(m);
            };

        peerClient_ = std::make_unique<MainnetPeerClient>(
            app_,
            peerEndpoints_,
            mainnetNetworkID_,
            std::move(cb),
            journal_);
        peerClient_->start();

        running_ = true;

        JLOG(journal_.info())
            << "MainnetWatcher: Started with " << peerEndpoints_.size()
            << " mainnet peer(s) (native protocol), vault="
            << toBase58(vaultAddress_);
        return;
    }

    // Fallback: WebSocket mode
    if (wsUrls_.empty())
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: No mainnet connectivity configured";
        return;
    }

    running_ = true;
    thread_ = std::thread([this] { run(); });

    JLOG(journal_.info())
        << "MainnetWatcher: Started with " << wsUrls_.size()
        << " mainnet node(s) (WebSocket), vault="
        << toBase58(vaultAddress_);
}

void
MainnetWatcher::stop()
{
    running_ = false;

    if (peerClient_)
        peerClient_->stop();

    if (thread_.joinable())
        thread_.join();
}

void
MainnetWatcher::run()
{
    while (running_)
    {
        try
        {
            connect();
        }
        catch (std::exception const& e)
        {
            JLOG(journal_.warn())
                << "MainnetWatcher: Connection error: " << e.what();
        }

        // Wait before reconnecting
        if (running_)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

void
MainnetWatcher::connect()
{
    for (auto const& url : wsUrls_)
    {
        if (!running_)
            return;

        try
        {
            JLOG(journal_.info())
                << "MainnetWatcher: Connecting to " << url;

            // Parse URL: wss://host[:port][/path]
            std::string host, port, path;
            bool useTLS = false;

            auto u = url;
            if (u.substr(0, 6) == "wss://")
            {
                useTLS = true;
                u = u.substr(6);
                port = "443";
            }
            else if (u.substr(0, 5) == "ws://")
            {
                u = u.substr(5);
                port = "80";
            }
            else
            {
                JLOG(journal_.warn())
                    << "MainnetWatcher: Invalid URL scheme: " << url;
                continue;
            }

            auto pathPos = u.find('/');
            if (pathPos != std::string::npos)
            {
                path = u.substr(pathPos);
                u = u.substr(0, pathPos);
            }
            else
            {
                path = "/";
            }

            auto colonPos = u.find(':');
            if (colonPos != std::string::npos)
            {
                host = u.substr(0, colonPos);
                port = u.substr(colonPos + 1);
            }
            else
            {
                host = u;
            }

            net::io_context ioc;
            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve(host, port);

            if (useTLS)
            {
                ssl::context ctx(ssl::context::tlsv12_client);
                ctx.set_verify_mode(ssl::verify_none);

                websocket::stream<ssl::stream<tcp::socket>> ws(ioc, ctx);

                auto& tcpStream =
                    beast_ns::get_lowest_layer(ws);
                tcpStream.connect(*results.begin());

                // SNI
                if (!SSL_set_tlsext_host_name(
                        ws.next_layer().native_handle(), host.c_str()))
                {
                    JLOG(journal_.warn())
                        << "MainnetWatcher: SNI failed for " << host;
                    continue;
                }

                ws.next_layer().handshake(ssl::stream_base::client);
                ws.handshake(host, path);

                // Subscribe to streams
                Json::Value subscribe;
                subscribe["command"] = "subscribe";
                subscribe["streams"] =
                    Json::Value(Json::arrayValue);
                subscribe["streams"].append("ledger");
                subscribe["streams"].append("validations");

                // Also subscribe to vault account transactions
                subscribe["accounts"] = Json::Value(Json::arrayValue);
                subscribe["accounts"].append(
                    toBase58(vaultAddress_));

                Json::FastWriter writer;
                ws.write(net::buffer(writer.write(subscribe)));

                JLOG(journal_.info())
                    << "MainnetWatcher: Connected and subscribed to "
                    << url;

                // Read loop — periodically drain submit queue
                beast_ns::flat_buffer buffer;
                while (running_)
                {
                    drainSubmitQueue(ws);
                    resubmitPending(ws);

                    buffer.clear();
                    ws.read(buffer);
                    auto const msg = beast_ns::buffers_to_string(
                        buffer.data());
                    onMessage(msg);
                }

                ws.close(websocket::close_code::normal);
            }
            else
            {
                websocket::stream<tcp::socket> ws(ioc);

                beast_ns::get_lowest_layer(ws).connect(
                    *results.begin());
                ws.handshake(host, path);

                Json::Value subscribe;
                subscribe["command"] = "subscribe";
                subscribe["streams"] =
                    Json::Value(Json::arrayValue);
                subscribe["streams"].append("ledger");
                subscribe["streams"].append("validations");
                subscribe["accounts"] = Json::Value(Json::arrayValue);
                subscribe["accounts"].append(
                    toBase58(vaultAddress_));

                Json::FastWriter writer;
                ws.write(net::buffer(writer.write(subscribe)));

                JLOG(journal_.info())
                    << "MainnetWatcher: Connected and subscribed to "
                    << url;

                beast_ns::flat_buffer buffer;
                while (running_)
                {
                    drainSubmitQueue(ws);
                    resubmitPending(ws);

                    buffer.clear();
                    ws.read(buffer);
                    auto const msg = beast_ns::buffers_to_string(
                        buffer.data());
                    onMessage(msg);
                }

                ws.close(websocket::close_code::normal);
            }

            return;  // Successfully connected and loop ended normally
        }
        catch (std::exception const& e)
        {
            JLOG(journal_.warn())
                << "MainnetWatcher: Failed to connect to " << url
                << ": " << e.what();
            continue;  // Try next URL
        }
    }
}

void
MainnetWatcher::onMessage(std::string const& msg)
{
    Json::Value data;
    Json::Reader reader;

    if (!reader.parse(msg, data) || !data.isObject())
    {
        JLOG(journal_.trace()) << "MainnetWatcher: Unparseable message";
        return;
    }

    auto const type = data.isMember("type") ? data["type"].asString() : "";

    if (type == "ledgerClosed")
    {
        onLedger(data);
    }
    else if (type == "validationReceived")
    {
        onValidation(data);
    }
    else if (type == "transaction")
    {
        onTransaction(data);
    }
}

void
MainnetWatcher::onLedger(Json::Value const& data)
{
    if (!data.isMember("ledger_index"))
        return;

    auto const idx = data["ledger_index"].asUInt();

    std::lock_guard lock(mutex_);

    latestLedgerIndex_ = std::max(latestLedgerIndex_, idx);

    LedgerData ld;
    ld.ledgerIndex = idx;

    // Build ledger header for XPOP from the closed ledger message
    ld.ledgerHeader["index"] = idx;
    if (data.isMember("ledger_hash"))
        ld.ledgerHeader["hash"] = data["ledger_hash"].asString();
    if (data.isMember("txn_count"))
        ld.ledgerHeader["txn_count"] = data["txn_count"];

    // These fields come from the ledger_header object if available
    if (data.isMember("ledger_header"))
    {
        auto const& hdr = data["ledger_header"];
        if (hdr.isMember("account_hash"))
            ld.ledgerHeader["acroot"] = hdr["account_hash"].asString();
        if (hdr.isMember("transaction_hash"))
            ld.ledgerHeader["txroot"] =
                hdr["transaction_hash"].asString();
        if (hdr.isMember("parent_hash"))
            ld.ledgerHeader["phash"] = hdr["parent_hash"].asString();
        if (hdr.isMember("close_time"))
            ld.ledgerHeader["close"] = hdr["close_time"];
        if (hdr.isMember("close_time_resolution"))
            ld.ledgerHeader["cres"] = hdr["close_time_resolution"];
        if (hdr.isMember("close_flags"))
            ld.ledgerHeader["flags"] = hdr["close_flags"];
        if (hdr.isMember("parent_close_time"))
            ld.ledgerHeader["pclose"] = hdr["parent_close_time"];
        if (hdr.isMember("total_coins"))
            ld.ledgerHeader["coins"] = hdr["total_coins"].asString();
        ld.hasHeader = true;
    }

    ledgers_[idx] = std::move(ld);

    // Prune old ledgers
    while (ledgers_.size() > kMaxLedgerHistory)
        ledgers_.erase(ledgers_.begin());

    JLOG(journal_.trace()) << "MainnetWatcher: Ledger " << idx;

    tryBuildXPOPs();
}

void
MainnetWatcher::onValidation(Json::Value const& data)
{
    if (!data.isMember("ledger_index") || !data.isMember("validation_public_key"))
        return;

    auto const idx = data["ledger_index"].asUInt();

    ValidationEntry ve;
    ve.validatorKey = data["validation_public_key"].asString();

    if (data.isMember("data"))
        ve.validationHex = data["data"].asString();

    std::lock_guard lock(mutex_);

    validations_[idx].push_back(std::move(ve));

    // Prune old validations
    while (validations_.size() > kMaxLedgerHistory)
        validations_.erase(validations_.begin());

    tryBuildXPOPs();
}

void
MainnetWatcher::onTransaction(Json::Value const& data)
{
    if (!data.isMember("transaction") || !data.isMember("meta"))
        return;

    auto const& tx = data["transaction"];

    // Check for vault outbound confirmations (TicketCreate, Payment, etc.)
    if (isVaultOutbound(tx))
    {
        onVaultOutbound(data);
        return;
    }

    if (!isVaultPayment(tx))
        return;

    JLOG(journal_.info())
        << "MainnetWatcher: Detected vault payment in ledger "
        << (data.isMember("ledger_index")
                ? data["ledger_index"].asUInt()
                : 0);

    // Extract raw blob and meta hex if available, otherwise use JSON
    PendingTx ptx;
    if (data.isMember("tx_blob"))
        ptx.txBlob = data["tx_blob"];
    else
        ptx.txBlob = tx;

    ptx.txMeta = data["meta"];

    if (data.isMember("ledger_index"))
        ptx.ledgerIndex = data["ledger_index"].asUInt();

    // Hash
    if (tx.isMember("hash"))
    {
        if (!ptx.txHash.parseHex(tx["hash"].asString()))
        {
            JLOG(journal_.warn())
                << "MainnetWatcher: Failed to parse tx hash";
            return;
        }
    }

    std::lock_guard lock(mutex_);

    // Dedup check
    for (auto const& h : completedTxHashes_)
    {
        if (h == ptx.txHash)
            return;
    }

    pendingTxs_[ptx.txHash] = std::move(ptx);

    JLOG(journal_.info())
        << "MainnetWatcher: Queued pending tx " << ptx.txHash;

    tryBuildXPOPs();
}

bool
MainnetWatcher::isVaultPayment(Json::Value const& tx) const
{
    // Must be a Payment
    if (!tx.isMember("TransactionType") ||
        tx["TransactionType"].asString() != "Payment")
        return false;

    // Must be destined for our vault
    if (!tx.isMember("Destination"))
        return false;

    auto const dest =
        parseBase58<AccountID>(tx["Destination"].asString());
    if (!dest || *dest != vaultAddress_)
        return false;

    // Must have OperationLimit matching our network ID
    if (!tx.isMember("OperationLimit"))
        return false;

    if (tx["OperationLimit"].asUInt() != networkID_)
        return false;

    return true;
}

void
MainnetWatcher::tryBuildXPOPs()
{
    // For each pending tx, check if we have its ledger data + enough
    // validations to build a complete XPOP
    std::vector<uint256> completed;

    for (auto& [txHash, ptx] : pendingTxs_)
    {
        auto lit = ledgers_.find(ptx.ledgerIndex);
        if (lit == ledgers_.end() || !lit->second.hasHeader)
            continue;

        auto vit = validations_.find(ptx.ledgerIndex);
        if (vit == validations_.end() || vit->second.empty())
            continue;

        // Build the XPOP JSON
        Json::Value xpop;

        // Ledger section
        xpop["ledger"] = lit->second.ledgerHeader;

        // Transaction section
        Json::Value txSection;
        txSection["blob"] = ptx.txBlob;
        txSection["meta"] = ptx.txMeta;
        if (ptx.txProof.isObject() || ptx.txProof.isArray())
            txSection["proof"] = ptx.txProof;
        else
            txSection["proof"] = Json::Value(Json::objectValue);
        xpop["transaction"] = txSection;

        // Validation section
        Json::Value valSection;
        Json::Value valData(Json::objectValue);
        for (auto const& ve : vit->second)
        {
            valData[ve.validatorKey] = ve.validationHex;
        }
        valSection["data"] = valData;

        // UNL section — use first stored UNL from a trusted publisher
        if (!storedUNLs_.empty())
        {
            auto const& unl = storedUNLs_.front();
            Json::Value unlSection;
            unlSection["public_key"] = unl.publicKey;
            unlSection["manifest"] = unl.manifest;
            unlSection["blob"] = unl.blob;
            unlSection["signature"] = unl.signature;
            unlSection["version"] = unl.version;
            valSection["unl"] = unlSection;
        }
        else
        {
            valSection["unl"] = Json::Value(Json::objectValue);
        }

        xpop["validation"] = valSection;

        ReadyImport ri;
        ri.mainnetTxHash = txHash;
        ri.xpop = std::move(xpop);
        ri.importSequence = 0;

        // Derive sidechain account from inner tx signing key
        auto const& innerTx = ptx.txBlob;
        if (innerTx.isMember("Account"))
        {
            auto const acct =
                parseBase58<AccountID>(innerTx["Account"].asString());
            if (acct)
                ri.sidechainAccount = *acct;
        }

        if (innerTx.isMember("Sequence"))
            ri.importSequence = innerTx["Sequence"].asUInt();

        readyImports_.push_back(std::move(ri));
        completed.push_back(txHash);

        JLOG(journal_.info())
            << "MainnetWatcher: Built XPOP for " << txHash;
    }

    // Move completed from pending to completed history
    for (auto const& h : completed)
    {
        pendingTxs_.erase(h);
        completedTxHashes_.push_back(h);

        while (completedTxHashes_.size() > kMaxCompletedHistory)
            completedTxHashes_.pop_front();
    }
}

std::vector<MainnetWatcher::ReadyImport>
MainnetWatcher::consumeReadyImports()
{
    std::lock_guard lock(mutex_);
    std::vector<ReadyImport> result;
    result.swap(readyImports_);
    return result;
}

std::optional<MainnetWatcher::ReadyImport>
MainnetWatcher::getImport(uint256 const& mainnetTxHash) const
{
    std::lock_guard lock(mutex_);
    for (auto const& ri : readyImports_)
    {
        if (ri.mainnetTxHash == mainnetTxHash)
            return ri;
    }
    return std::nullopt;
}

Json::Value
MainnetWatcher::getStatus() const
{
    std::lock_guard lock(mutex_);

    Json::Value result(Json::objectValue);
    result["running"] = running_.load();
    result["mainnet_urls"] =
        static_cast<Json::UInt>(wsUrls_.size());
    result["vault_address"] = toBase58(vaultAddress_);
    result["ledgers_cached"] =
        static_cast<Json::UInt>(ledgers_.size());
    result["pending_txs"] =
        static_cast<Json::UInt>(pendingTxs_.size());
    result["ready_imports"] =
        static_cast<Json::UInt>(readyImports_.size());
    result["completed_count"] =
        static_cast<Json::UInt>(completedTxHashes_.size());
    result["submit_queue"] =
        static_cast<Json::UInt>(submitQueue_.size());
    result["pending_submissions"] =
        static_cast<Json::UInt>(pendingSubmissions_.size());
    result["confirmed_vault_txs"] =
        static_cast<Json::UInt>(confirmedVaultTxs_.size());

    if (!ledgers_.empty())
    {
        result["latest_ledger"] = ledgers_.rbegin()->first;
        result["oldest_ledger"] = ledgers_.begin()->first;
    }

    return result;
}

void
MainnetWatcher::submitTransaction(std::string const& txBlob)
{
    // Use peer client if available
    if (peerClient_)
    {
        peerClient_->submitTransaction(txBlob);
        JLOG(journal_.info())
            << "MainnetWatcher: Submitted tx via peer protocol";
        return;
    }

    // Fallback: queue for WebSocket submission
    std::lock_guard lock(mutex_);
    submitQueue_.push_back(txBlob);

    JLOG(journal_.info())
        << "MainnetWatcher: Queued tx for WebSocket submission, queue size="
        << submitQueue_.size();
}

std::vector<MainnetWatcher::ConfirmedVaultTx>
MainnetWatcher::consumeConfirmedVaultTxs()
{
    std::lock_guard lock(mutex_);
    std::vector<ConfirmedVaultTx> result;
    result.swap(confirmedVaultTxs_);
    return result;
}

bool
MainnetWatcher::isVaultOutbound(Json::Value const& tx) const
{
    if (!tx.isMember("Account"))
        return false;

    auto const acct =
        parseBase58<AccountID>(tx["Account"].asString());
    return acct && *acct == vaultAddress_;
}

void
MainnetWatcher::onVaultOutbound(Json::Value const& data)
{
    auto const& tx = data["transaction"];
    auto const& meta = data["meta"];

    // Only care about successful transactions
    if (meta.isMember("TransactionResult") &&
        meta["TransactionResult"].asString() != "tesSUCCESS")
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: Vault outbound failed: "
            << meta["TransactionResult"].asString();
        return;
    }

    ConfirmedVaultTx confirmed;

    if (data.isMember("ledger_index"))
        confirmed.ledgerIndex = data["ledger_index"].asUInt();

    if (tx.isMember("TransactionType"))
        confirmed.txType = tx["TransactionType"].asString();

    if (tx.isMember("hash"))
        (void)confirmed.txHash.parseHex(tx["hash"].asString());

    // For TicketCreate: extract ticket count and new Sequence
    if (confirmed.txType == "TicketCreate")
    {
        if (tx.isMember("TicketCount"))
            confirmed.ticketCount = tx["TicketCount"].asUInt();

        // Extract new Sequence from metadata AffectedNodes
        if (meta.isMember("AffectedNodes"))
        {
            for (auto const& node : meta["AffectedNodes"])
            {
                if (!node.isMember("ModifiedNode"))
                    continue;
                auto const& mn = node["ModifiedNode"];
                if (mn.isMember("LedgerEntryType") &&
                    mn["LedgerEntryType"].asString() == "AccountRoot" &&
                    mn.isMember("FinalFields") &&
                    mn["FinalFields"].isMember("Sequence"))
                {
                    confirmed.newSequence =
                        mn["FinalFields"]["Sequence"].asUInt();
                    break;
                }
            }
        }

        JLOG(journal_.info())
            << "MainnetWatcher: Vault TicketCreate confirmed, "
            << confirmed.ticketCount << " tickets, newSeq="
            << confirmed.newSequence;
    }
    else if (confirmed.txType == "Payment")
    {
        if (tx.isMember("TicketSequence"))
            confirmed.ticketSequence = tx["TicketSequence"].asUInt();

        JLOG(journal_.info())
            << "MainnetWatcher: Vault Payment confirmed, ticket="
            << confirmed.ticketSequence
            << " in ledger " << confirmed.ledgerIndex;
    }
    else
    {
        JLOG(journal_.info())
            << "MainnetWatcher: Vault " << confirmed.txType
            << " confirmed in ledger " << confirmed.ledgerIndex;
    }

    std::lock_guard lock(mutex_);

    confirmedVaultTxs_.push_back(std::move(confirmed));

    // Remove from pending submissions
    pendingSubmissions_.erase(confirmed.txHash);
}

template <class WsStream>
void
MainnetWatcher::drainSubmitQueue(WsStream& ws)
{
    std::deque<std::string> toSend;
    {
        std::lock_guard lock(mutex_);
        toSend.swap(submitQueue_);
    }

    Json::FastWriter writer;
    for (auto const& blob : toSend)
    {
        Json::Value cmd;
        cmd["command"] = "submit";
        cmd["tx_blob"] = blob;

        try
        {
            ws.write(net::buffer(writer.write(cmd)));

            JLOG(journal_.info())
                << "MainnetWatcher: Submitted tx to mainnet";

            // Track as pending for retry
            // We don't know the hash yet — it will be matched on confirmation
            // Store by blob hash for dedup
            std::lock_guard lock(mutex_);
            // Use SHA512Half of the blob as a tracking key
            auto const blobHash = sha512Half(Slice(
                reinterpret_cast<std::uint8_t const*>(blob.data()),
                blob.size()));
            pendingSubmissions_[blobHash] = {
                blob, latestLedgerIndex_, 0};
        }
        catch (std::exception const& e)
        {
            JLOG(journal_.warn())
                << "MainnetWatcher: Submit failed: " << e.what();
            // Re-queue for retry
            std::lock_guard lock(mutex_);
            submitQueue_.push_back(blob);
        }
    }
}

template <class WsStream>
void
MainnetWatcher::resubmitPending(WsStream& ws)
{
    std::vector<std::pair<uint256, SubmissionState>> toRetry;
    std::vector<uint256> toRemove;

    {
        std::lock_guard lock(mutex_);
        for (auto& [hash, state] : pendingSubmissions_)
        {
            if (latestLedgerIndex_ <
                state.lastSubmitLedger + kRetryAfterLedgers)
                continue;

            if (state.retryCount >= kMaxRetries)
            {
                JLOG(journal_.warn())
                    << "MainnetWatcher: Giving up on submission "
                    << hash << " after " << kMaxRetries << " retries";
                toRemove.push_back(hash);
                continue;
            }

            toRetry.push_back({hash, state});
        }
    }

    Json::FastWriter writer;
    for (auto& [hash, state] : toRetry)
    {
        Json::Value cmd;
        cmd["command"] = "submit";
        cmd["tx_blob"] = state.txBlob;

        try
        {
            ws.write(net::buffer(writer.write(cmd)));

            std::lock_guard lock(mutex_);
            auto it = pendingSubmissions_.find(hash);
            if (it != pendingSubmissions_.end())
            {
                it->second.retryCount++;
                it->second.lastSubmitLedger = latestLedgerIndex_;
            }

            JLOG(journal_.info())
                << "MainnetWatcher: Resubmitted tx, retry #"
                << state.retryCount + 1;
        }
        catch (std::exception const& e)
        {
            JLOG(journal_.warn())
                << "MainnetWatcher: Resubmit failed: " << e.what();
        }
    }

    if (!toRemove.empty())
    {
        std::lock_guard lock(mutex_);
        for (auto const& h : toRemove)
            pendingSubmissions_.erase(h);
    }
}

// Explicit template instantiations for both WebSocket stream types
template void MainnetWatcher::drainSubmitQueue(
    websocket::stream<ssl::stream<tcp::socket>>& ws);
template void MainnetWatcher::drainSubmitQueue(
    websocket::stream<tcp::socket>& ws);
template void MainnetWatcher::resubmitPending(
    websocket::stream<ssl::stream<tcp::socket>>& ws);
template void MainnetWatcher::resubmitPending(
    websocket::stream<tcp::socket>& ws);

// ---------------------------------------------------------------------------
// Peer protocol callbacks
// ---------------------------------------------------------------------------

void
MainnetWatcher::onPeerValidation(protocol::TMValidation const& m)
{
    auto const& valBytes = m.validation();
    if (valBytes.size() < 50)
        return;

    try
    {
        // Deserialize STValidation to extract ledger sequence and key
        SerialIter sit(
            reinterpret_cast<std::uint8_t const*>(valBytes.data()),
            valBytes.size());
        auto val = std::make_shared<STValidation>(
            sit,
            [](PublicKey const& pk) { return calcNodeID(pk); },
            false);

        auto const ledgerSeq = val->getFieldU32(sfLedgerSequence);
        auto const pubKey = val->getSignerPublic();

        // Dedup via hash of validation blob
        auto const valHash = sha512Half(
            Slice(valBytes.data(), valBytes.size()));

        std::lock_guard lock(mutex_);

        // Prune dedup set on new ledger
        if (ledgerSeq > lastValPruneSeq_ + 2)
        {
            recentValHashes_.clear();
            lastValPruneSeq_ = ledgerSeq;
        }

        if (!recentValHashes_.insert(valHash).second)
            return;  // duplicate

        ValidationEntry ve;
        ve.validatorKey = toBase58(TokenType::NodePublic, pubKey);
        ve.validationHex = strHex(
            Slice(valBytes.data(), valBytes.size()));

        validations_[ledgerSeq].push_back(std::move(ve));

        while (validations_.size() > kMaxLedgerHistory)
            validations_.erase(validations_.begin());

        JLOG(journal_.trace())
            << "MainnetWatcher: Peer validation for ledger "
            << ledgerSeq;

        tryBuildXPOPs();
    }
    catch (std::exception const& e)
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: Failed to parse peer validation: "
            << e.what();
    }
}

void
MainnetWatcher::onPeerStatusChange(protocol::TMStatusChange const& m)
{
    if (!m.has_ledgerseq())
        return;

    // We care about accepted/closed ledger events
    if (m.has_newevent())
    {
        auto const ev = m.newevent();
        if (ev != protocol::neACCEPTED_LEDGER &&
            ev != protocol::neCLOSING_LEDGER)
            return;
    }

    auto const seq = m.ledgerseq();

    std::lock_guard lock(mutex_);

    latestLedgerIndex_ = std::max(latestLedgerIndex_, seq);

    LedgerData ld;
    ld.ledgerIndex = seq;
    ld.ledgerHeader["index"] = seq;

    if (m.has_ledgerhash() && m.ledgerhash().size() == 32)
    {
        ld.ledgerHeader["hash"] = strHex(
            Slice(m.ledgerhash().data(), m.ledgerhash().size()));
    }
    // hasHeader stays false until liBASE response fills in details

    ledgers_[seq] = std::move(ld);

    while (ledgers_.size() > kMaxLedgerHistory)
        ledgers_.erase(ledgers_.begin());

    JLOG(journal_.trace())
        << "MainnetWatcher: Peer status change, ledger " << seq;

    // Request full ledger header via liBASE
    if (peerClient_ && m.has_ledgerhash() && m.ledgerhash().size() == 32)
    {
        uint256 hash;
        std::memcpy(hash.data(), m.ledgerhash().data(), 32);
        peerClient_->requestLedgerBase(seq);
    }
}

void
MainnetWatcher::onPeerTransaction(protocol::TMTransaction const& m)
{
    auto const& raw = m.rawtransaction();
    if (raw.empty())
        return;

    try
    {
        SerialIter sit(
            reinterpret_cast<std::uint8_t const*>(raw.data()),
            raw.size());
        auto stx = std::make_shared<STTx const>(sit);

        // Check if this is a Payment to our vault with correct OperationLimit
        if (stx->getTxnType() != ttPAYMENT)
            return;

        auto const dest = stx->getAccountID(sfDestination);
        if (dest != vaultAddress_)
            return;

        if (!stx->isFieldPresent(sfOperationLimit))
            return;

        if (stx->getFieldU32(sfOperationLimit) != networkID_)
            return;

        auto const txHash = stx->getTransactionID();

        std::lock_guard lock(mutex_);

        // Dedup
        for (auto const& h : completedTxHashes_)
        {
            if (h == txHash)
                return;
        }

        mempoolDetected_.insert(txHash);

        JLOG(journal_.info())
            << "MainnetWatcher: Detected vault payment in mempool: "
            << txHash;
    }
    catch (std::exception const& e)
    {
        JLOG(journal_.trace())
            << "MainnetWatcher: Failed to parse peer tx: " << e.what();
    }
}

void
MainnetWatcher::onPeerLedgerData(protocol::TMLedgerData const& m)
{
    if (m.type() == protocol::liBASE)
        processLedgerBase(m);
    else if (m.type() == protocol::liTX_NODE)
        processTxNodes(m);
}

void
MainnetWatcher::processLedgerBase(protocol::TMLedgerData const& m)
{
    if (m.nodes_size() < 1)
        return;

    auto const seq = m.ledgerseq();

    // Node 0 is the serialized LedgerHeader
    auto const& headerNode = m.nodes(0);
    auto const& headerData = headerNode.nodedata();

    if (headerData.empty())
        return;

    std::lock_guard lock(mutex_);

    auto it = ledgers_.find(seq);
    if (it == ledgers_.end())
    {
        LedgerData ld;
        ld.ledgerIndex = seq;
        ledgers_[seq] = std::move(ld);
        it = ledgers_.find(seq);
    }

    auto& ld = it->second;

    // Parse serialized LedgerHeader
    try
    {
        Serializer s(headerData.data(), headerData.size());
        SerialIter sit(s.slice());

        // LedgerHeader wire format:
        // uint32 sequence, uint64 drops, uint256 accountHash,
        // uint256 txHash, uint256 parentHash, uint32 closeTime,
        // uint32 parentCloseTime, uint8 closeTimeResolution,
        // uint8 closeFlags
        // (Plus the ledger hash which is in the TMLedgerData envelope)

        auto const ledgerSeq = sit.get32();
        auto const drops = sit.get64();
        auto const accountHash = sit.get256();
        auto const txHash = sit.get256();
        auto const parentHash = sit.get256();
        auto const closeTime = sit.get32();
        auto const parentCloseTime = sit.get32();
        auto const closeTimeRes = sit.get8();
        auto const closeFlags = sit.get8();

        ld.ledgerHeader["index"] = ledgerSeq;
        ld.ledgerHeader["coins"] = std::to_string(drops);
        ld.ledgerHeader["acroot"] = strHex(accountHash);
        ld.ledgerHeader["txroot"] = strHex(txHash);
        ld.ledgerHeader["phash"] = strHex(parentHash);
        ld.ledgerHeader["close"] = closeTime;
        ld.ledgerHeader["pclose"] = parentCloseTime;
        ld.ledgerHeader["cres"] = closeTimeRes;
        ld.ledgerHeader["flags"] = closeFlags;

        if (m.has_ledgerhash() && m.ledgerhash().size() == 32)
        {
            ld.ledgerHeader["hash"] = strHex(
                Slice(m.ledgerhash().data(), m.ledgerhash().size()));
        }

        ld.hasHeader = true;

        JLOG(journal_.info())
            << "MainnetWatcher: Got ledger header for " << seq;

        // Check if any mempool-detected txs are in this ledger
        // Start SHAMap traversal using the tx root from liBASE
        if (!mempoolDetected_.empty())
        {
            uint256 ledgerHash;
            if (m.has_ledgerhash() && m.ledgerhash().size() == 32)
                std::memcpy(
                    ledgerHash.data(), m.ledgerhash().data(), 32);

            for (auto const& txH : mempoolDetected_)
            {
                SHAMapTraversal traversal;
                traversal.txHash = txH;
                traversal.ledgerHash = ledgerHash;
                traversal.ledgerSeq = seq;
                activeTraversals_[txH] = std::move(traversal);
            }

            // Try to parse the tx root from node[2] of liBASE
            // to start targeted traversal at depth 1
            bool startedTargeted = false;
            if (m.nodes_size() >= 3)
            {
                auto const& txRootNode = m.nodes(2);
                auto const& txRootData = txRootNode.nodedata();
                if (txRootData.size() >= 2)
                {
                    auto const typeByte = static_cast<unsigned char>(
                        txRootData[txRootData.size() - 1]);
                    uint256 branches[16];
                    auto const innerData = Slice(
                        txRootData.data(), txRootData.size() - 1);

                    if (parseInnerNode(
                            innerData, typeByte, branches))
                    {
                        // For each pending tx, follow the
                        // matching branch at depth 0
                        std::vector<std::string> nodeIDs;
                        for (auto& [txH2, trav] :
                             activeTraversals_)
                        {
                            if (trav.ledgerSeq != seq)
                                continue;
                            auto const nibble =
                                getNibble(txH2, 0);
                            if (branches[nibble] ==
                                beast::zero)
                                continue;

                            // Store root as proof
                            trav.proofPath.push_back(
                                std::string(
                                    txRootData.begin(),
                                    txRootData.end()));
                            trav.depth = 1;

                            // Build child nodeID at depth 1
                            uint256 childId;
                            childId.begin()[0] =
                                static_cast<unsigned char>(
                                    nibble << 4);
                            nodeIDs.push_back(
                                makeNodeIDString(childId, 1));
                        }

                        if (peerClient_ && !nodeIDs.empty())
                        {
                            peerClient_->requestTxNodes(
                                ledgerHash, seq, nodeIDs, 3);
                            startedTargeted = true;
                        }
                    }
                }
            }

            // Fallback: request the root if we couldn't parse it
            if (!startedTargeted && peerClient_ &&
                !mempoolDetected_.empty())
            {
                std::string rootNodeID(33, '\0');
                peerClient_->requestTxNodes(
                    ledgerHash, seq, {rootNodeID}, 3);
            }
        }

        tryBuildXPOPs();
    }
    catch (std::exception const& e)
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: Failed to parse ledger header: "
            << e.what();
    }
}

// SHAMap wire type constants (last byte of node data)
static constexpr unsigned char kWireTypeInner = 2;
static constexpr unsigned char kWireTypeCompressedInner = 3;
static constexpr unsigned char kWireTypeTransactionWithMeta = 4;

// Get the nibble at a given depth from a 256-bit hash.
// Even depths use the high nibble, odd depths use the low nibble.
static unsigned int
getNibble(uint256 const& hash, int depth)
{
    auto const byte = hash[depth / 2];
    return (depth & 1) ? (byte & 0x0F) : (byte >> 4);
}

// Build a 33-byte SHAMapNodeID string (32-byte hash + 1-byte depth).
static std::string
makeNodeIDString(uint256 const& id, int depth)
{
    std::string result(33, '\0');
    std::memcpy(result.data(), id.data(), 32);
    result[32] = static_cast<char>(depth);
    return result;
}

// Parse branch hashes from an inner node's wire data (excluding type byte).
// Returns true if parsed successfully, fills branches[16].
static bool
parseInnerNode(Slice innerData, unsigned char typeByte, uint256 branches[16])
{
    std::memset(branches, 0, 16 * sizeof(uint256));

    if (typeByte == kWireTypeInner)
    {
        // Full inner: 16 x 32-byte hashes = 512 bytes
        if (innerData.size() != 512)
            return false;
        for (int b = 0; b < 16; ++b)
            std::memcpy(branches[b].data(), innerData.data() + b * 32, 32);
        return true;
    }
    else if (typeByte == kWireTypeCompressedInner)
    {
        // Compressed: pairs of [32-byte hash][1-byte position]
        if (innerData.size() % 33 != 0)
            return false;
        for (std::size_t off = 0; off < innerData.size(); off += 33)
        {
            auto const pos =
                static_cast<unsigned char>(innerData[off + 32]);
            if (pos >= 16)
                return false;
            std::memcpy(
                branches[pos].data(), innerData.data() + off, 32);
        }
        return true;
    }
    return false;
}

void
MainnetWatcher::processTxNodes(protocol::TMLedgerData const& m)
{
    auto const seq = m.ledgerseq();

    uint256 ledgerHash;
    if (m.has_ledgerhash() && m.ledgerhash().size() == 32)
        std::memcpy(ledgerHash.data(), m.ledgerhash().data(), 32);

    JLOG(journal_.trace())
        << "MainnetWatcher: Received " << m.nodes_size()
        << " tx node(s) for ledger " << seq;

    std::lock_guard lock(mutex_);

    // Collect nodes that need deeper requests
    struct NextRequest
    {
        uint256 nodeId;
        int depth;
    };
    std::vector<NextRequest> needMore;

    for (int i = 0; i < m.nodes_size(); ++i)
    {
        auto const& node = m.nodes(i);
        auto const& data = node.nodedata();

        if (data.size() < 2)
            continue;

        auto const typeByte =
            static_cast<unsigned char>(data[data.size() - 1]);

        // Parse the 33-byte nodeid if present
        int nodeDepth = -1;
        if (node.has_nodeid() && node.nodeid().size() == 33)
        {
            nodeDepth = static_cast<unsigned char>(
                node.nodeid()[32]);
        }

        if (typeByte == kWireTypeTransactionWithMeta)
        {
            // Leaf: [VL tx][VL meta][32-byte key][0x04]
            if (data.size() < 34)
                continue;

            // Extract the 32-byte key (tx hash) — last 33 bytes
            // are [32-byte key][1-byte type]
            uint256 leafKey;
            std::memcpy(
                leafKey.data(), data.data() + data.size() - 33, 32);

            // Check if this matches a mempool-detected tx
            auto mit = mempoolDetected_.find(leafKey);
            if (mit == mempoolDetected_.end())
                continue;

            // Extract tx + metadata from the VL-encoded item data
            auto const itemLen = data.size() - 33;  // strip key + type

            try
            {
                SerialIter sit(
                    reinterpret_cast<std::uint8_t const*>(data.data()),
                    itemLen);

                auto const txBytes = sit.getVLBuffer();
                auto const metaBytes = sit.getVLBuffer();

                PendingTx ptx;
                ptx.txHash = leafKey;
                ptx.ledgerIndex = seq;
                ptx.txBlob = strHex(Slice(txBytes.data(), txBytes.size()));
                ptx.txMeta =
                    strHex(Slice(metaBytes.data(), metaBytes.size()));

                // Build proof from traversal path
                auto tit = activeTraversals_.find(leafKey);
                if (tit != activeTraversals_.end())
                {
                    Json::Value proof(Json::arrayValue);
                    for (auto const& p : tit->second.proofPath)
                        proof.append(
                            strHex(Slice(p.data(), p.size())));
                    ptx.txProof = proof;
                    activeTraversals_.erase(tit);
                }

                pendingTxs_[leafKey] = std::move(ptx);
                mempoolDetected_.erase(mit);

                JLOG(journal_.info())
                    << "MainnetWatcher: Found tx leaf in SHAMap: "
                    << leafKey;

                tryBuildXPOPs();
            }
            catch (std::exception const& e)
            {
                JLOG(journal_.warn())
                    << "MainnetWatcher: Failed to parse tx leaf: "
                    << e.what();
            }
        }
        else if (
            typeByte == kWireTypeInner ||
            typeByte == kWireTypeCompressedInner)
        {
            if (nodeDepth < 0)
                continue;

            // Parse branch hashes
            uint256 branches[16];
            auto const innerData = Slice(data.data(), data.size() - 1);

            if (!parseInnerNode(innerData, typeByte, branches))
                continue;

            // For each active traversal at this ledger, follow the
            // branch matching the tx hash nibble at this depth
            for (auto& [txHash, traversal] : activeTraversals_)
            {
                if (traversal.ledgerSeq != seq)
                    continue;

                auto const nibble = getNibble(txHash, nodeDepth);

                if (branches[nibble] == beast::zero)
                {
                    JLOG(journal_.trace())
                        << "MainnetWatcher: Tx " << txHash
                        << " not found at depth " << nodeDepth;
                    continue;
                }

                // Store inner node data as proof path element
                traversal.proofPath.push_back(
                    std::string(data.begin(), data.end()));
                traversal.depth = nodeDepth + 1;

                // Build child nodeID: encode nibbles up to depth+1
                uint256 childId;
                // Copy nibbles from txHash up to this depth
                for (int d = 0; d <= nodeDepth; ++d)
                {
                    auto const n = getNibble(txHash, d);
                    if (d & 1)
                        childId.begin()[d / 2] |= n;
                    else
                        childId.begin()[d / 2] |=
                            static_cast<unsigned char>(n << 4);
                }

                needMore.push_back({childId, nodeDepth + 1});
            }
        }
    }

    // Request deeper nodes for unresolved traversals
    if (!needMore.empty() && peerClient_)
    {
        std::vector<std::string> nodeIDs;
        for (auto const& req : needMore)
            nodeIDs.push_back(makeNodeIDString(req.nodeId, req.depth));

        peerClient_->requestTxNodes(ledgerHash, seq, nodeIDs, 3);

        JLOG(journal_.trace())
            << "MainnetWatcher: Requesting " << nodeIDs.size()
            << " deeper tx node(s) for ledger " << seq;
    }
}

bool
MainnetWatcher::isVaultPaymentRaw(Slice txData) const
{
    try
    {
        SerialIter sit(txData);
        auto stx = std::make_shared<STTx const>(sit);

        if (stx->getTxnType() != ttPAYMENT)
            return false;

        if (stx->getAccountID(sfDestination) != vaultAddress_)
            return false;

        if (!stx->isFieldPresent(sfOperationLimit))
            return false;

        return stx->getFieldU32(sfOperationLimit) == networkID_;
    }
    catch (...)
    {
        return false;
    }
}

void
MainnetWatcher::onPeerValidatorList(protocol::TMValidatorList const& m)
{
    try
    {
        auto const& manifestBytes = m.manifest();
        auto const& blobBytes = m.blob();
        auto const& sigBytes = m.signature();
        auto const version = m.version();

        if (manifestBytes.empty() || blobBytes.empty() || sigBytes.empty())
            return;

        // Deserialize manifest to extract master public key
        auto const manifest = deserializeManifest(
            std::string(manifestBytes.begin(), manifestBytes.end()));
        if (!manifest)
        {
            JLOG(journal_.trace())
                << "MainnetWatcher: Failed to deserialize UNL manifest";
            return;
        }

        auto const masterKeyHex =
            strHex(manifest->masterKey.slice());

        // Filter: only store UNL from publishers in our import_vl_keys
        auto const& vlKeys = app_.config().IMPORT_VL_KEYS;
        bool trusted = vlKeys.empty();  // accept all if none configured
        for (auto const& [hexKey, pk] : vlKeys)
        {
            if (hexKey == masterKeyHex || pk == manifest->masterKey)
            {
                trusted = true;
                break;
            }
        }

        if (!trusted)
        {
            JLOG(journal_.trace())
                << "MainnetWatcher: UNL from untrusted publisher, ignoring";
            return;
        }

        StoredUNL unl;
        unl.publicKey = masterKeyHex;
        unl.manifest = base64_encode(
            reinterpret_cast<unsigned char const*>(manifestBytes.data()),
            manifestBytes.size());
        unl.blob = base64_encode(
            reinterpret_cast<unsigned char const*>(blobBytes.data()),
            blobBytes.size());
        unl.signature = strHex(
            Slice(sigBytes.data(), sigBytes.size()));
        unl.version = version;

        std::lock_guard lock(mutex_);

        // Replace existing UNL from same publisher or add new
        bool replaced = false;
        for (auto& existing : storedUNLs_)
        {
            if (existing.publicKey == unl.publicKey)
            {
                existing = std::move(unl);
                replaced = true;
                break;
            }
        }
        if (!replaced)
            storedUNLs_.push_back(std::move(unl));

        JLOG(journal_.info())
            << "MainnetWatcher: Stored UNL from publisher "
            << masterKeyHex.substr(0, 16) << "...";
    }
    catch (std::exception const& e)
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: Failed to process TMValidatorList: "
            << e.what();
    }
}

void
MainnetWatcher::onPeerValidatorListCollection(
    protocol::TMValidatorListCollection const& m)
{
    // TMValidatorListCollection v2: has manifest at top level,
    // blobs is a repeated field with per-blob manifest/blob/signature.
    // We process the first blob entry as if it were a TMValidatorList.
    try
    {
        auto const& manifestBytes = m.manifest();
        if (manifestBytes.empty() || m.blobs_size() == 0)
            return;

        auto const version = m.version();

        for (int i = 0; i < m.blobs_size(); ++i)
        {
            auto const& entry = m.blobs(i);
            if (!entry.has_blob() || !entry.has_signature())
                continue;

            // Use per-blob manifest if present, else top-level manifest
            auto const& mfBytes = entry.has_manifest()
                ? entry.manifest()
                : manifestBytes;

            protocol::TMValidatorList vl;
            vl.set_manifest(mfBytes);
            vl.set_blob(entry.blob());
            vl.set_signature(entry.signature());
            vl.set_version(version);

            onPeerValidatorList(vl);
        }
    }
    catch (std::exception const& e)
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: Failed to process "
               "TMValidatorListCollection: "
            << e.what();
    }
}

}  // namespace xrpl
