#include <xrpld/app/misc/MainnetPeerClient.h>

#include <xrpld/app/main/Application.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/Handshake.h>
#include <xrpld/overlay/detail/ProtocolMessage.h>
#include <xrpld/overlay/detail/ProtocolVersion.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/random.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/SecretKey.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <chrono>
#include <random>

namespace xrpl {

namespace beast_ns = boost::beast;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using error_code = boost::system::error_code;

// ---------------------------------------------------------------------------
// PeerConn — one connection to a mainnet peer
// ---------------------------------------------------------------------------

class MainnetPeerClient::PeerConn
    : public std::enable_shared_from_this<PeerConn>
{
public:
    enum class State
    {
        Disconnected,
        Connecting,
        TlsHandshake,
        HttpHandshake,
        Connected,
        Stopping
    };

    PeerConn(
        MainnetPeerClient& owner,
        std::string endpoint,
        net::io_context& ioc,
        std::shared_ptr<ssl::context> const& sslCtx,
        beast::Journal journal)
        : owner_(owner)
        , endpoint_(std::move(endpoint))
        , strand_(net::make_strand(ioc))
        , reconnectTimer_(ioc)
        , sslCtx_(sslCtx)
        , journal_(journal)
    {
    }

    void start()
    {
        net::post(strand_, [self = shared_from_this()] { self->connect(); });
    }

    void stop()
    {
        net::post(strand_, [self = shared_from_this()] {
            self->state_ = State::Stopping;
            self->reconnectTimer_.cancel();
            if (self->stream_)
            {
                error_code ec;
                beast_ns::get_lowest_layer(*self->stream_).socket().close(ec);
            }
        });
    }

    bool isConnected() const { return state_ == State::Connected; }
    std::string const& endpoint() const { return endpoint_; }
    State state() const { return state_; }

    /** Send a protocol message to this peer. */
    void sendMessage(
        ::google::protobuf::Message const& msg,
        protocol::MessageType type)
    {
        auto const message = std::make_shared<Message>(msg, type);
        auto const& buf =
            message->getBuffer(compression::Compressed::Off);

        net::post(strand_, [self = shared_from_this(), message, &buf] {
            if (self->state_ != State::Connected || !self->stream_)
                return;
            error_code ec;
            net::write(*self->stream_, net::buffer(buf), ec);
            if (ec)
            {
                JLOG(self->journal_.warn())
                    << "MainnetPeer[" << self->endpoint_
                    << "]: send failed: " << ec.message();
                self->scheduleReconnect();
            }
        });
    }

private:
    // -- Connection flow --

    void connect()
    {
        if (state_ == State::Stopping)
            return;

        state_ = State::Connecting;
        backoffSeconds_ = std::min(backoffSeconds_, maxBackoff_);

        // Parse "host port" or "host:port"
        std::string host, port;
        {
            auto const s = endpoint_;
            auto pos = s.find(' ');
            if (pos == std::string::npos)
                pos = s.find(':');
            if (pos != std::string::npos)
            {
                host = s.substr(0, pos);
                port = s.substr(pos + 1);
            }
            else
            {
                host = s;
                port = "51235";  // default peer port
            }
        }

        JLOG(journal_.info())
            << "MainnetPeer[" << endpoint_ << "]: connecting";

        try
        {
            // Create fresh stream
            stream_ = std::make_unique<stream_type>(
                socket_type(strand_), *sslCtx_);

            // Resolve
            tcp::resolver resolver(strand_);
            auto const results = resolver.resolve(host, port);

            // TCP connect
            auto& lowest = beast_ns::get_lowest_layer(*stream_);
            lowest.expires_after(std::chrono::seconds(10));
            lowest.connect(*results.begin());

            // TLS handshake
            state_ = State::TlsHandshake;
            stream_->next_layer().expires_after(std::chrono::seconds(10));
            stream_->set_verify_mode(ssl::verify_none);
            stream_->handshake(ssl::stream_base::client);

            // Compute shared value for MITM detection
            auto const sharedValue = makeSharedValue(*stream_, journal_);
            if (!sharedValue)
            {
                JLOG(journal_.warn())
                    << "MainnetPeer[" << endpoint_
                    << "]: shared value failed";
                return scheduleReconnect();
            }

            // Build HTTP upgrade request
            state_ = State::HttpHandshake;
            auto req = makeRequest(
                false,   // crawlPublic
                false,   // comprEnabled
                false,   // ledgerReplayEnabled
                false,   // txReduceRelayEnabled
                false);  // vpReduceRelayEnabled

            // Insert handshake headers with mainnet network ID
            buildMainnetHandshake(req, *sharedValue);

            // Write request
            beast_ns::get_lowest_layer(*stream_).expires_after(
                std::chrono::seconds(5));
            boost::beast::http::write(*stream_, req);

            // Read response
            boost::beast::flat_buffer readBuf;
            http_response_type response;
            beast_ns::get_lowest_layer(*stream_).expires_after(
                std::chrono::seconds(5));
            boost::beast::http::read(*stream_, readBuf, response);

            // Check for upgrade
            if (response.result() !=
                boost::beast::http::status::switching_protocols)
            {
                JLOG(journal_.warn())
                    << "MainnetPeer[" << endpoint_
                    << "]: upgrade failed: " << response.result();
                return scheduleReconnect();
            }

            // Verify peer handshake
            auto const peerKey = verifyHandshake(
                response,
                *sharedValue,
                owner_.mainnetNetworkID_,
                beast::IP::Address(),
                beast_ns::get_lowest_layer(*stream_)
                    .socket()
                    .remote_endpoint()
                    .address(),
                owner_.app_);

            state_ = State::Connected;
            backoffSeconds_ = 1;

            JLOG(journal_.info())
                << "MainnetPeer[" << endpoint_
                << "]: connected, peer="
                << toBase58(TokenType::NodePublic, peerKey);

            // Disable timeouts for the read loop
            beast_ns::get_lowest_layer(*stream_).expires_never();

            // Enter message read loop
            readLoop();
        }
        catch (std::exception const& e)
        {
            JLOG(journal_.warn())
                << "MainnetPeer[" << endpoint_
                << "]: connection failed: " << e.what();
            scheduleReconnect();
        }
    }

    void buildMainnetHandshake(
        boost::beast::http::fields& h,
        uint256 const& sharedValue)
    {
        h.insert(
            "Network-ID",
            std::to_string(owner_.mainnetNetworkID_));

        h.insert(
            "Network-Time",
            std::to_string(
                owner_.app_.timeKeeper()
                    .now()
                    .time_since_epoch()
                    .count()));

        h.insert(
            "Public-Key",
            toBase58(TokenType::NodePublic, owner_.nodePublicKey_));

        {
            auto const sig = signDigest(
                owner_.nodePublicKey_,
                owner_.nodeSecretKey_,
                sharedValue);
            h.insert(
                "Session-Signature",
                base64_encode(sig.data(), sig.size()));
        }

        h.insert(
            "Instance-Cookie",
            std::to_string(owner_.instanceCookie_));

        // Omit Closed-Ledger, Previous-Ledger — we don't track mainnet
    }

    // -- Message read loop --

    void readLoop()
    {
        if (state_ != State::Connected || !stream_)
            return;

        boost::beast::flat_buffer buffer;
        buffer.reserve(65536);

        while (state_ == State::Connected && owner_.running_)
        {
            error_code ec;
            auto const bytes = stream_->read_some(
                buffer.prepare(65536), ec);

            if (ec)
            {
                if (ec == net::error::eof ||
                    ec == net::error::connection_reset ||
                    ec == net::error::operation_aborted)
                {
                    JLOG(journal_.info())
                        << "MainnetPeer[" << endpoint_
                        << "]: disconnected: " << ec.message();
                }
                else
                {
                    JLOG(journal_.warn())
                        << "MainnetPeer[" << endpoint_
                        << "]: read error: " << ec.message();
                }
                break;
            }

            buffer.commit(bytes);

            // Parse and dispatch messages
            while (buffer.size() > 0)
            {
                std::size_t hint = 0;
                auto [consumed, parseEc] =
                    invokeProtocolMessage(buffer.data(), *this, hint);

                if (parseEc)
                {
                    JLOG(journal_.warn())
                        << "MainnetPeer[" << endpoint_
                        << "]: parse error: " << parseEc.message();
                    state_ = State::Disconnected;
                    break;
                }

                if (consumed == 0)
                    break;  // need more data

                buffer.consume(consumed);
            }

            if (state_ != State::Connected)
                break;
        }

        if (state_ != State::Stopping)
            scheduleReconnect();
    }

    // -- invokeProtocolMessage handler interface --
public:
    bool compressionEnabled() const { return false; }

    template <class T>
    void onMessageBegin(
        std::uint16_t, std::shared_ptr<T> const&,
        std::uint32_t, std::uint32_t, bool)
    {
    }

    template <class T>
    void onMessageEnd(std::uint16_t, std::shared_ptr<T> const&)
    {
    }

    void onMessageUnknown(std::uint16_t) {}

    // TMPing — respond with pong
    void onMessage(std::shared_ptr<protocol::TMPing> const& m)
    {
        if (m->type() == protocol::TMPing::ptPING)
        {
            protocol::TMPing pong;
            pong.set_type(protocol::TMPing::ptPONG);
            if (m->has_seq())
                pong.set_seq(m->seq());
            if (m->has_pingtime())
                pong.set_pingtime(m->pingtime());

            sendMessage(pong, protocol::mtPING);
        }
    }

    // TMValidation — forward to callback
    void onMessage(std::shared_ptr<protocol::TMValidation> const& m)
    {
        if (owner_.callbacks_.onValidation)
            owner_.callbacks_.onValidation(*m);
    }

    // TMStatusChange — forward to callback
    void onMessage(std::shared_ptr<protocol::TMStatusChange> const& m)
    {
        if (owner_.callbacks_.onStatusChange)
            owner_.callbacks_.onStatusChange(*m);
    }

    // TMTransaction — forward to callback
    void onMessage(std::shared_ptr<protocol::TMTransaction> const& m)
    {
        if (owner_.callbacks_.onTransaction)
            owner_.callbacks_.onTransaction(*m);
    }

    // TMLedgerData — forward to callback
    void onMessage(std::shared_ptr<protocol::TMLedgerData> const& m)
    {
        if (owner_.callbacks_.onLedgerData)
            owner_.callbacks_.onLedgerData(*m);
    }

    // TMValidatorList — forward to callback
    void onMessage(std::shared_ptr<protocol::TMValidatorList> const& m)
    {
        if (owner_.callbacks_.onValidatorList)
            owner_.callbacks_.onValidatorList(*m);
    }

    // TMValidatorListCollection — forward to callback
    void onMessage(
        std::shared_ptr<protocol::TMValidatorListCollection> const& m)
    {
        if (owner_.callbacks_.onValidatorListCollection)
            owner_.callbacks_.onValidatorListCollection(*m);
    }

    // Catch-all — ignore everything else
    template <class T>
    void onMessage(std::shared_ptr<T> const&)
    {
    }

private:
    void scheduleReconnect()
    {
        if (state_ == State::Stopping)
            return;

        state_ = State::Disconnected;
        stream_.reset();

        JLOG(journal_.info())
            << "MainnetPeer[" << endpoint_
            << "]: reconnecting in " << backoffSeconds_ << "s";

        reconnectTimer_.expires_after(
            std::chrono::seconds(backoffSeconds_));
        reconnectTimer_.async_wait(
            net::bind_executor(
                strand_,
                [self = shared_from_this()](error_code ec) {
                    if (ec || self->state_ == State::Stopping)
                        return;
                    self->connect();
                }));

        // Exponential backoff: 1, 2, 4, 8, 16, 30, 30, ...
        backoffSeconds_ = std::min(backoffSeconds_ * 2, maxBackoff_);
    }

    MainnetPeerClient& owner_;
    std::string endpoint_;
    net::strand<net::io_context::executor_type> strand_;
    net::steady_timer reconnectTimer_;
    std::shared_ptr<ssl::context> sslCtx_;
    beast::Journal journal_;

    std::unique_ptr<stream_type> stream_;
    State state_{State::Disconnected};

    int backoffSeconds_{1};
    static constexpr int maxBackoff_ = 30;
};

// ---------------------------------------------------------------------------
// MainnetPeerClient
// ---------------------------------------------------------------------------

MainnetPeerClient::MainnetPeerClient(
    Application& app,
    std::vector<std::string> const& peerEndpoints,
    std::uint32_t mainnetNetworkID,
    Callbacks callbacks,
    beast::Journal journal)
    : app_(app)
    , journal_(journal)
    , peerEndpoints_(peerEndpoints)
    , mainnetNetworkID_(mainnetNetworkID)
    , callbacks_(std::move(callbacks))
{
    // Generate ephemeral node identity for mainnet handshakes
    auto const [pub, sec] = randomKeyPair(KeyType::secp256k1);
    nodePublicKey_ = pub;
    nodeSecretKey_ = sec;

    // Random instance cookie
    instanceCookie_ = rand_int<std::uint64_t>();

    // SSL context
    sslContext_ = std::make_shared<ssl::context>(ssl::context::tlsv12_client);
    sslContext_->set_verify_mode(ssl::verify_none);
}

MainnetPeerClient::~MainnetPeerClient()
{
    stop();
}

void
MainnetPeerClient::start()
{
    if (peerEndpoints_.empty())
    {
        JLOG(journal_.warn())
            << "MainnetPeerClient: No peer endpoints configured";
        return;
    }

    running_ = true;

    // Work guard to keep io_context alive
    work_.emplace(net::make_work_guard(io_context_));

    // Start 2 threads for the io_context
    auto const numThreads = std::min<std::size_t>(2, peerEndpoints_.size());
    for (std::size_t i = 0; i < numThreads; ++i)
    {
        threads_.emplace_back([this] {
            try
            {
                io_context_.run();
            }
            catch (std::exception const& e)
            {
                JLOG(journal_.error())
                    << "MainnetPeerClient: io_context error: " << e.what();
            }
        });
    }

    // Create and start a PeerConn for each endpoint
    {
        std::lock_guard lock(connMutex_);
        for (auto const& ep : peerEndpoints_)
        {
            auto conn = std::make_shared<PeerConn>(
                *this, ep, io_context_, sslContext_, journal_);
            connections_.push_back(conn);
            conn->start();
        }
    }

    JLOG(journal_.info())
        << "MainnetPeerClient: Started with " << peerEndpoints_.size()
        << " mainnet peer(s), networkID=" << mainnetNetworkID_;
}

void
MainnetPeerClient::stop()
{
    if (!running_.exchange(false))
        return;

    // Stop all connections
    {
        std::lock_guard lock(connMutex_);
        for (auto& conn : connections_)
            conn->stop();
    }

    // Release work guard and wait for threads
    work_.reset();
    for (auto& t : threads_)
    {
        if (t.joinable())
            t.join();
    }
    threads_.clear();

    {
        std::lock_guard lock(connMutex_);
        connections_.clear();
    }

    JLOG(journal_.info()) << "MainnetPeerClient: Stopped";
}

std::shared_ptr<MainnetPeerClient::PeerConn>
MainnetPeerClient::pickPeer() const
{
    std::lock_guard lock(connMutex_);
    if (connections_.empty())
        return nullptr;

    // Round-robin among connected peers
    auto const n = connections_.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        auto const idx = (nextPeer_.fetch_add(1)) % n;
        if (connections_[idx]->isConnected())
            return connections_[idx];
    }
    return nullptr;
}

void
MainnetPeerClient::submitTransaction(
    std::string const& txBlob,
    std::size_t fanout)
{
    protocol::TMTransaction msg;
    msg.set_rawtransaction(txBlob);
    msg.set_status(protocol::tsNEW);

    std::lock_guard lock(connMutex_);

    std::size_t sent = 0;
    for (auto const& conn : connections_)
    {
        if (sent >= fanout)
            break;
        if (conn->isConnected())
        {
            conn->sendMessage(msg, protocol::mtTRANSACTION);
            ++sent;
        }
    }

    if (sent == 0)
    {
        JLOG(journal_.warn())
            << "MainnetPeerClient: No connected peer for tx submit";
    }
    else
    {
        JLOG(journal_.info())
            << "MainnetPeerClient: Submitted tx to " << sent << " peer(s)";
    }
}

void
MainnetPeerClient::requestLedgerBase(std::uint32_t ledgerSeq)
{
    auto peer = pickPeer();
    if (!peer)
    {
        JLOG(journal_.warn())
            << "MainnetPeerClient: No connected peer for liBASE request";
        return;
    }

    protocol::TMGetLedger msg;
    msg.set_itype(protocol::liBASE);
    msg.set_ledgerseq(ledgerSeq);
    msg.set_ltype(protocol::ltACCEPTED);
    peer->sendMessage(msg, protocol::mtGET_LEDGER);

    JLOG(journal_.trace())
        << "MainnetPeerClient: Requested liBASE for ledger " << ledgerSeq;
}

void
MainnetPeerClient::requestLedgerBase(uint256 const& ledgerHash)
{
    auto peer = pickPeer();
    if (!peer)
        return;

    protocol::TMGetLedger msg;
    msg.set_itype(protocol::liBASE);
    msg.set_ledgerhash(ledgerHash.data(), ledgerHash.size());
    peer->sendMessage(msg, protocol::mtGET_LEDGER);
}

void
MainnetPeerClient::requestTxNodes(
    uint256 const& ledgerHash,
    std::uint32_t ledgerSeq,
    std::vector<std::string> const& nodeIDs,
    std::uint32_t queryDepth)
{
    auto peer = pickPeer();
    if (!peer)
    {
        JLOG(journal_.warn())
            << "MainnetPeerClient: No connected peer for liTX_NODE request";
        return;
    }

    protocol::TMGetLedger msg;
    msg.set_itype(protocol::liTX_NODE);
    msg.set_ledgerhash(ledgerHash.data(), ledgerHash.size());
    msg.set_ledgerseq(ledgerSeq);
    msg.set_querydepth(queryDepth);

    for (auto const& nid : nodeIDs)
        *(msg.add_nodeids()) = nid;

    peer->sendMessage(msg, protocol::mtGET_LEDGER);

    JLOG(journal_.trace())
        << "MainnetPeerClient: Requested liTX_NODE for ledger "
        << ledgerSeq << ", " << nodeIDs.size() << " node(s)";
}

Json::Value
MainnetPeerClient::getStatus() const
{
    Json::Value result(Json::objectValue);
    result["running"] = running_.load();
    result["mainnet_network_id"] = mainnetNetworkID_;

    std::lock_guard lock(connMutex_);
    result["total_peers"] = static_cast<Json::UInt>(connections_.size());

    Json::UInt connected = 0;
    Json::Value peers(Json::arrayValue);
    for (auto const& conn : connections_)
    {
        Json::Value p(Json::objectValue);
        p["endpoint"] = conn->endpoint();
        p["connected"] = conn->isConnected();
        peers.append(p);
        if (conn->isConnected())
            ++connected;
    }
    result["connected_peers"] = connected;
    result["peers"] = peers;

    return result;
}

}  // namespace xrpl
