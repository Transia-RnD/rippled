#pragma once

#include <xrpl/basics/BasicConfig.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/ReadView.h>

#include <memory>
#include <string>

namespace xrpl {

class Application;

struct DEXFeedConfig
{
    bool enabled = false;
    std::string socketPath;
    bool includeAMMState = true;
};

DEXFeedConfig
parseDEXFeedConfig(Section const& section);

class DEXFeedEmitter
{
public:
    virtual ~DEXFeedEmitter() = default;

    virtual void
    start() = 0;

    virtual void
    stop() = 0;

    virtual void
    emit(std::shared_ptr<ReadView const> const& ledger) = 0;
};

std::unique_ptr<DEXFeedEmitter>
make_DEXFeedEmitter(
    DEXFeedConfig const& config,
    Application& app,
    beast::Journal journal);

}  // namespace xrpl
