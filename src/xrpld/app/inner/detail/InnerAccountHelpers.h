#pragma once

#include <xrpld/app/inner/detail/InnerView.h>

#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Serializer.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace xrpl {
namespace inner {

struct InnerAccountFields
{
    AccountID id;
    std::uint64_t balance{0};
    std::uint32_t sequence{0};

    void
    writeTo(InnerSLE& sle) const
    {
        Serializer s;
        s.addBitString(id);
        s.add64(balance);
        s.add32(sequence);
        sle.setData(s);
    }

    static std::optional<InnerAccountFields>
    readFrom(InnerSLE const& sle)
    {
        auto data = sle.slice();
        if (data.size() < 20 + 8 + 4)
            return std::nullopt;

        InnerAccountFields f;
        std::memcpy(f.id.data(), data.data(), 20);
        data += 20;
        f.balance = (static_cast<std::uint64_t>(data[0]) << 56) |
            (static_cast<std::uint64_t>(data[1]) << 48) |
            (static_cast<std::uint64_t>(data[2]) << 40) |
            (static_cast<std::uint64_t>(data[3]) << 32) |
            (static_cast<std::uint64_t>(data[4]) << 24) |
            (static_cast<std::uint64_t>(data[5]) << 16) |
            (static_cast<std::uint64_t>(data[6]) << 8) |
            static_cast<std::uint64_t>(data[7]);
        data += 8;
        f.sequence = (static_cast<std::uint32_t>(data[0]) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]);
        return f;
    }
};

inline std::optional<InnerAccountFields>
readAccount(InnerView& view, AccountID const& id)
{
    auto sle = view.peek(keyspace::account(id));
    if (!sle)
        return std::nullopt;
    return InnerAccountFields::readFrom(*sle);
}

inline void
writeAccount(InnerView& view, InnerAccountFields const& acct, bool isNew)
{
    auto const k = keyspace::account(acct.id);
    if (isNew)
    {
        auto sle = std::make_shared<InnerSLE>(k);
        acct.writeTo(*sle);
        view.insert(sle);
    }
    else
    {
        auto sle = view.peek(k);
        if (sle)
        {
            acct.writeTo(*sle);
            view.update(sle);
        }
    }
}

}  // namespace inner
}  // namespace xrpl
