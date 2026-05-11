#pragma once

#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/json/json_value.h>
#include <xrpl/shamap/SHAMap.h>

#include <cstdint>
#include <memory>

namespace xrpl {
namespace inner {

struct InnerLedgerHeader
{
    uint256 hash;
    uint256 parentHash;
    uint256 stateHash;
    uint256 txHash;
    std::uint32_t seq{0};
    NetClock::time_point closeTime{};
    NetClock::time_point parentCloseTime{};
    NetClock::duration closeTimeResolution{};
    bool closeTimeAgree{true};

    // Anchoring to outer chain
    std::uint32_t outerCheckpointSeq{0};
    uint256 outerCheckpointHash;
};

class InnerLedger
{
public:
    InnerLedger() = default;

    explicit InnerLedger(
        InnerLedgerHeader const& header,
        std::shared_ptr<SHAMap> stateMap,
        std::shared_ptr<SHAMap> txMap)
        : header_(header), stateMap_(std::move(stateMap)), txMap_(std::move(txMap))
    {
    }

    InnerLedgerHeader const&
    header() const
    {
        return header_;
    }

    InnerLedgerHeader&
    mutableHeader()
    {
        return header_;
    }

    std::shared_ptr<SHAMap> const&
    stateMap() const
    {
        return stateMap_;
    }

    std::shared_ptr<SHAMap> const&
    txMap() const
    {
        return txMap_;
    }

private:
    InnerLedgerHeader header_;
    std::shared_ptr<SHAMap> stateMap_;
    std::shared_ptr<SHAMap> txMap_;
};

class InnerCxLedger
{
public:
    using ID = uint256;
    using Seq = std::uint32_t;

    InnerCxLedger() = default;

    explicit InnerCxLedger(std::shared_ptr<InnerLedger const> const& l)
        : ledger(l)
    {
    }

    [[nodiscard]] Seq
    seq() const
    {
        return ledger->header().seq;
    }

    [[nodiscard]] ID const&
    id() const
    {
        return ledger->header().hash;
    }

    [[nodiscard]] ID const&
    parentID() const
    {
        return ledger->header().parentHash;
    }

    [[nodiscard]] NetClock::duration
    closeTimeResolution() const
    {
        return ledger->header().closeTimeResolution;
    }

    [[nodiscard]] bool
    closeAgree() const
    {
        return ledger->header().closeTimeAgree;
    }

    [[nodiscard]] NetClock::time_point
    closeTime() const
    {
        return ledger->header().closeTime;
    }

    [[nodiscard]] NetClock::time_point
    parentCloseTime() const
    {
        return ledger->header().parentCloseTime;
    }

    [[nodiscard]] json::Value
    getJson() const
    {
        json::Value j;
        j["seq"] = ledger->header().seq;
        j["hash"] = to_string(ledger->header().hash);
        return j;
    }

    std::shared_ptr<InnerLedger const> ledger;
};

}  // namespace inner
}  // namespace xrpl
