#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/messages.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xrpl {

class Application;

/** Lightweight peer protocol client for connecting to mainnet nodes.
 *
 *  Manages N async connections to mainnet peers using the native xrpld
 *  peer protocol (TCP + TLS + HTTP upgrade). Each connection uses the
 *  mainnet network ID for the handshake, separate from the sidechain.
 *
 *  Handles:
 *  - TMValidation: validation signatures for XPOP building
 *  - TMStatusChange: ledger close notifications
 *  - TMTransaction: mempool transaction detection
 *  - TMLedgerData: responses to ledger data requests
 *  - TMPing: keepalive (responds with pong)
 *
 *  Does NOT participate in the sidechain's Overlay.
 */
class MainnetPeerClient
{
public:
    /** Callbacks invoked on the io_context threads — must be thread-safe. */
    struct Callbacks
    {
        std::function<void(protocol::TMValidation const&)> onValidation;
        std::function<void(protocol::TMStatusChange const&)> onStatusChange;
        std::function<void(protocol::TMTransaction const&)> onTransaction;
        std::function<void(protocol::TMLedgerData const&)> onLedgerData;
        std::function<void(protocol::TMValidatorList const&)>
            onValidatorList;
        std::function<void(protocol::TMValidatorListCollection const&)>
            onValidatorListCollection;
    };

    MainnetPeerClient(
        Application& app,
        std::vector<std::string> const& peerEndpoints,
        std::uint32_t mainnetNetworkID,
        Callbacks callbacks,
        beast::Journal journal);

    ~MainnetPeerClient();

    void start();
    void stop();

    /** Submit a signed transaction blob to mainnet via connected peers.
        Sends to up to `fanout` peers for redundancy (default 2). */
    void submitTransaction(std::string const& txBlob, std::size_t fanout = 2);

    /** Request ledger base data (header + root nodes) by sequence. */
    void requestLedgerBase(std::uint32_t ledgerSeq);

    /** Request ledger base data by hash. */
    void requestLedgerBase(uint256 const& ledgerHash);

    /** Request specific transaction tree nodes from a ledger. */
    void requestTxNodes(
        uint256 const& ledgerHash,
        std::uint32_t ledgerSeq,
        std::vector<std::string> const& nodeIDs,
        std::uint32_t queryDepth = 3);

    /** Get diagnostic status. */
    Json::Value getStatus() const;

private:
    class PeerConn;

    /** Pick a connected peer for sending a request. */
    std::shared_ptr<PeerConn> pickPeer() const;

    Application& app_;
    beast::Journal journal_;
    std::vector<std::string> peerEndpoints_;
    std::uint32_t mainnetNetworkID_;
    Callbacks callbacks_;

    // Ephemeral node identity for mainnet handshakes
    PublicKey nodePublicKey_;
    SecretKey nodeSecretKey_;
    std::uint64_t instanceCookie_;

    // Async I/O
    boost::asio::io_context io_context_;
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> work_;
    std::vector<std::thread> threads_;
    std::shared_ptr<boost::asio::ssl::context> sslContext_;

    // Active connections
    mutable std::mutex connMutex_;
    std::vector<std::shared_ptr<PeerConn>> connections_;

    std::atomic<bool> running_{false};

    // Round-robin index for peer selection
    mutable std::atomic<std::size_t> nextPeer_{0};
};

}  // namespace xrpl
