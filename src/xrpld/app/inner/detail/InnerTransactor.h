#pragma once

#include <xrpld/app/inner/detail/InnerView.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xrpl {
namespace inner {

enum class InnerTxType : std::uint16_t
{
    Credit = 1,
    Transfer = 2,
    Withdraw = 3,
};

struct InnerTx
{
    InnerTxType type;
    AccountID source;
    AccountID destination;
    std::uint64_t amount{0};
    std::uint32_t sequence{0};

    uint256
    id() const
    {
        Serializer s;
        s.add16(static_cast<std::uint16_t>(type));
        s.addBitString(source);
        s.addBitString(destination);
        s.add64(amount);
        s.add32(sequence);
        return s.getSHA512Half();
    }

    void
    serialize(Serializer& s) const
    {
        s.add16(static_cast<std::uint16_t>(type));
        s.addBitString(source);
        s.addBitString(destination);
        s.add64(amount);
        s.add32(sequence);
    }

    static std::optional<InnerTx>
    deserialize(Slice data)
    {
        if (data.size() < 2 + 20 + 20 + 8 + 4)
            return std::nullopt;

        InnerTx tx;
        tx.type = static_cast<InnerTxType>(
            (static_cast<std::uint16_t>(data[0]) << 8) |
            static_cast<std::uint16_t>(data[1]));
        data += 2;
        std::memcpy(tx.source.data(), data.data(), 20);
        data += 20;
        std::memcpy(tx.destination.data(), data.data(), 20);
        data += 20;
        tx.amount = (static_cast<std::uint64_t>(data[0]) << 56) |
            (static_cast<std::uint64_t>(data[1]) << 48) |
            (static_cast<std::uint64_t>(data[2]) << 40) |
            (static_cast<std::uint64_t>(data[3]) << 32) |
            (static_cast<std::uint64_t>(data[4]) << 24) |
            (static_cast<std::uint64_t>(data[5]) << 16) |
            (static_cast<std::uint64_t>(data[6]) << 8) |
            static_cast<std::uint64_t>(data[7]);
        data += 8;
        tx.sequence = (static_cast<std::uint32_t>(data[0]) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]);
        return tx;
    }
};

struct InnerApplyContext
{
    InnerTx const& tx;
    InnerView& view;
    beast::Journal j;
};

class InnerTransactor
{
public:
    explicit InnerTransactor(InnerApplyContext& ctx)
        : ctx_(ctx)
    {
    }

    virtual ~InnerTransactor() = default;

    virtual TER
    preflight() const
    {
        return tesSUCCESS;
    }

    virtual TER
    preclaim() const
    {
        return tesSUCCESS;
    }

    virtual TER
    doApply() = 0;

protected:
    InnerApplyContext& ctx_;

    InnerView&
    view()
    {
        return ctx_.view;
    }

    InnerTx const&
    tx() const
    {
        return ctx_.tx;
    }
};

using InnerTransactorFactory =
    std::function<std::unique_ptr<InnerTransactor>(InnerApplyContext&)>;

class InnerTransactorRegistry
{
public:
    void
    registerType(
        InnerTxType type,
        std::string name,
        InnerTransactorFactory factory)
    {
        entries_[type] = {std::move(name), std::move(factory)};
    }

    std::unique_ptr<InnerTransactor>
    create(InnerTxType type, InnerApplyContext& ctx) const
    {
        auto it = entries_.find(type);
        if (it == entries_.end())
            return nullptr;
        return it->second.factory(ctx);
    }

    std::string const&
    name(InnerTxType type) const
    {
        static std::string const unknown = "Unknown";
        auto it = entries_.find(type);
        if (it == entries_.end())
            return unknown;
        return it->second.name;
    }

    bool
    known(InnerTxType type) const
    {
        return entries_.count(type) > 0;
    }

    TER
    apply(InnerTx const& tx, InnerView& view, beast::Journal j) const
    {
        InnerApplyContext ctx{tx, view, j};
        auto transactor = create(tx.type, ctx);
        if (!transactor)
            return temUNKNOWN;

        if (auto const ter = transactor->preflight(); ter != tesSUCCESS)
            return ter;

        if (auto const ter = transactor->preclaim(); ter != tesSUCCESS)
            return ter;

        auto const ter = transactor->doApply();
        if (isTesSuccess(ter))
            view.apply();

        return ter;
    }

private:
    struct Entry
    {
        std::string name;
        InnerTransactorFactory factory;
    };

    std::map<InnerTxType, Entry> entries_;
};

}  // namespace inner
}  // namespace xrpl
