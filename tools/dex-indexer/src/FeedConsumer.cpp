#include "FeedConsumer.h"

#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Minimal JSON parsing — avoids external dependency.
// The feed emits compact JSON with known structure from computeBookChanges.
#include <algorithm>
#include <charconv>

namespace dex {

namespace {

// Simple JSON field extraction for the known flat structure
std::string
jsonStr(std::string const& json, std::string const& key)
{
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos)
        return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos)
        return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string::npos)
        return {};
    return json.substr(pos, end - pos);
}

double
jsonNum(std::string const& json, std::string const& key)
{
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return 0.0;
    pos = json.find(':', pos);
    if (pos == std::string::npos)
        return 0.0;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    double val = 0.0;
    auto [ptr, ec] = std::from_chars(json.data() + pos, json.data() + json.size(), val);
    return val;
}

uint32_t
jsonU32(std::string const& json, std::string const& key)
{
    return static_cast<uint32_t>(jsonNum(json, key));
}

// Split JSON array elements (top-level only, handles nested objects)
std::vector<std::string>
jsonArrayElements(std::string const& json, std::string const& key)
{
    std::vector<std::string> result;
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos)
        return result;
    pos = json.find('[', pos);
    if (pos == std::string::npos)
        return result;
    ++pos;

    int depth = 0;
    size_t start = pos;
    for (size_t i = pos; i < json.size(); ++i)
    {
        if (json[i] == '{')
        {
            if (depth == 0)
                start = i;
            ++depth;
        }
        else if (json[i] == '}')
        {
            --depth;
            if (depth == 0)
                result.push_back(json.substr(start, i - start + 1));
        }
        else if (json[i] == ']' && depth == 0)
        {
            break;
        }
    }
    return result;
}

}  // namespace

FeedConsumer::FeedConsumer(std::string socketPath)
    : socketPath_(std::move(socketPath))
{
}

FeedConsumer::~FeedConsumer()
{
    stop();
}

void
FeedConsumer::setCallback(LedgerCallback cb)
{
    callback_ = std::move(cb);
}

void
FeedConsumer::start()
{
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void
FeedConsumer::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

bool
FeedConsumer::isConnected() const
{
    return connected_.load();
}

void
FeedConsumer::run()
{
    while (running_)
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            std::cerr << "FeedConsumer: socket() failed\n";
            sleep(2);
            continue;
        }

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            if (running_)
            {
                std::cerr << "FeedConsumer: connect failed, retrying...\n";
                sleep(2);
            }
            continue;
        }

        connected_ = true;
        std::cout << "FeedConsumer: connected to " << socketPath_ << "\n";

        std::string buffer;
        char readBuf[8192];

        while (running_)
        {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 1000);
            if (pr == 0)
                continue;
            if (pr < 0)
                break;

            ssize_t n = read(fd, readBuf, sizeof(readBuf));
            if (n <= 0)
            {
                std::cerr << "FeedConsumer: connection lost\n";
                break;
            }

            buffer.append(readBuf, static_cast<size_t>(n));

            size_t pos = 0;
            size_t newline;
            while ((newline = buffer.find('\n', pos)) != std::string::npos)
            {
                std::string line = buffer.substr(pos, newline - pos);
                pos = newline + 1;
                if (!line.empty())
                    processLine(line);
            }
            buffer.erase(0, pos);
        }

        connected_ = false;
        ::close(fd);

        if (running_)
            sleep(2);
    }
}

void
FeedConsumer::processLine(std::string const& line)
{
    uint32_t const ledgerSeq = jsonU32(line, "ledger_index");
    uint32_t const ledgerTime = jsonU32(line, "ledger_time");

    if (ledgerSeq == 0)
        return;

    std::vector<Tick> ticks;
    std::vector<AMMSnapshot> ammChanges;

    uint32_t txIdx = 0;
    for (auto const& change : jsonArrayElements(line, "changes"))
    {
        std::string currA = jsonStr(change, "currency_a");
        std::string currB = jsonStr(change, "currency_b");
        if (currA.empty() || currB.empty())
            continue;

        std::string const bookKey = currA + "|" + currB;

        Tick t;
        t.bookKey = bookKey;
        t.ledgerSeq = ledgerSeq;
        t.txIndex = txIdx++;
        t.timestamp = ledgerTime;

        std::string const closeStr = jsonStr(change, "close");
        if (!closeStr.empty())
            std::from_chars(closeStr.data(), closeStr.data() + closeStr.size(), t.rate);

        std::string const volA = jsonStr(change, "volume_a");
        if (!volA.empty())
            std::from_chars(volA.data(), volA.data() + volA.size(), t.volumeA);

        std::string const volB = jsonStr(change, "volume_b");
        if (!volB.empty())
            std::from_chars(volB.data(), volB.data() + volB.size(), t.volumeB);

        ticks.push_back(t);
    }

    for (auto const& ammEntry : jsonArrayElements(line, "amm_changes"))
    {
        AMMSnapshot snap;
        snap.account = jsonStr(ammEntry, "account");
        snap.ledgerSeq = ledgerSeq;
        snap.timestamp = ledgerTime;
        snap.lptBalance = jsonStr(ammEntry, "lpt_balance");
        ammChanges.push_back(snap);
    }

    if (callback_)
        callback_(ledgerSeq, ledgerTime, ticks, ammChanges);
}

}  // namespace dex
