#include <xrpld/app/misc/MainnetWatcher.h>

#include <xrpld/app/main/Application.h>

#include <xrpld/core/Config.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
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
    if (wsUrls_.empty())
    {
        JLOG(journal_.warn()) << "MainnetWatcher: No WebSocket URLs configured";
        return;
    }

    if (vaultAddress_ == AccountID{})
    {
        JLOG(journal_.warn())
            << "MainnetWatcher: No vault address configured, not starting";
        return;
    }

    running_ = true;
    thread_ = std::thread([this] { run(); });

    JLOG(journal_.info())
        << "MainnetWatcher: Started with " << wsUrls_.size()
        << " mainnet node(s), vault=" << toBase58(vaultAddress_);
}

void
MainnetWatcher::stop()
{
    running_ = false;
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

                // Read loop
                beast_ns::flat_buffer buffer;
                while (running_)
                {
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

        // UNL section - populated from our import_vl_keys config
        valSection["unl"] = Json::Value(Json::objectValue);

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

    if (!ledgers_.empty())
    {
        result["latest_ledger"] = ledgers_.rbegin()->first;
        result["oldest_ledger"] = ledgers_.begin()->first;
    }

    return result;
}

}  // namespace xrpl
