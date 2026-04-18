#pragma once

#include "Types.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace dex {

using LedgerCallback = std::function<void(
    uint32_t ledgerSeq,
    uint32_t ledgerTime,
    std::vector<Tick> const& ticks,
    std::vector<AMMSnapshot> const& ammChanges)>;

class FeedConsumer
{
public:
    explicit FeedConsumer(std::string socketPath);
    ~FeedConsumer();

    void setCallback(LedgerCallback cb);
    void start();
    void stop();
    bool isConnected() const;

private:
    std::string socketPath_;
    LedgerCallback callback_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread thread_;

    void run();
    void processLine(std::string const& line);
};

}  // namespace dex
