#pragma once

#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/shamap/SHAMap.h>
#include <xrpl/shamap/SHAMapItem.h>
#include <xrpl/shamap/SHAMapTreeNode.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace xrpl {
namespace inner {

enum class InnerLedgerEntryType : std::uint16_t
{
    Account = 1,
};

struct InnerKeylet
{
    InnerLedgerEntryType type;
    uint256 key;
};

namespace keyspace {

inline InnerKeylet
account(AccountID const& id)
{
    return {InnerLedgerEntryType::Account, sha512Half(id)};
}

}  // namespace keyspace

class InnerSLE
{
public:
    InnerSLE(InnerKeylet const& k)
        : type_(k.type), key_(k.key)
    {
    }

    InnerSLE(InnerLedgerEntryType type, uint256 const& key, Slice data)
        : type_(type), key_(key), data_(data.data(), data.data() + data.size())
    {
    }

    InnerLedgerEntryType
    type() const
    {
        return type_;
    }

    uint256 const&
    key() const
    {
        return key_;
    }

    Slice
    slice() const
    {
        return makeSlice(data_);
    }

    std::vector<std::uint8_t>&
    data()
    {
        return data_;
    }

    std::vector<std::uint8_t> const&
    data() const
    {
        return data_;
    }

    void
    setData(Serializer const& s)
    {
        auto sl = s.slice();
        data_.assign(sl.data(), sl.data() + sl.size());
    }

private:
    InnerLedgerEntryType type_;
    uint256 key_;
    std::vector<std::uint8_t> data_;
};

class InnerView
{
public:
    explicit InnerView(SHAMap& stateMap)
        : stateMap_(stateMap)
    {
    }

    std::shared_ptr<InnerSLE>
    peek(InnerKeylet const& k)
    {
        auto it = mods_.find(k.key);
        if (it != mods_.end())
        {
            if (it->second.action == Action::Erase)
                return nullptr;
            return it->second.sle;
        }

        auto const item = stateMap_.peekItem(k.key);
        if (!item)
            return nullptr;

        auto data = item->slice();
        if (data.size() < 2)
            return nullptr;
        data += 2;

        auto sle = std::make_shared<InnerSLE>(k.type, k.key, data);
        mods_[k.key] = {Action::Cache, sle};
        return sle;
    }

    bool
    exists(InnerKeylet const& k) const
    {
        auto it = mods_.find(k.key);
        if (it != mods_.end())
            return it->second.action != Action::Erase;
        return stateMap_.hasItem(k.key);
    }

    void
    insert(std::shared_ptr<InnerSLE> const& sle)
    {
        mods_[sle->key()] = {Action::Insert, sle};
    }

    void
    update(std::shared_ptr<InnerSLE> const& sle)
    {
        auto it = mods_.find(sle->key());
        if (it != mods_.end() && it->second.action == Action::Insert)
            return;
        mods_[sle->key()] = {Action::Modify, sle};
    }

    void
    erase(InnerKeylet const& k)
    {
        mods_[k.key] = {Action::Erase, nullptr};
    }

    void
    apply()
    {
        for (auto const& [key, mod] : mods_)
        {
            switch (mod.action)
            {
                case Action::Cache:
                    break;
                case Action::Insert: {
                    Serializer s;
                    s.add16(
                        static_cast<std::uint16_t>(mod.sle->type()));
                    auto const& d = mod.sle->data();
                    s.addRaw(d.data(), d.size());
                    stateMap_.addGiveItem(
                        SHAMapNodeType::TnAccountState,
                        makeShamapitem(key, s.slice()));
                    break;
                }
                case Action::Modify: {
                    Serializer s;
                    s.add16(
                        static_cast<std::uint16_t>(mod.sle->type()));
                    auto const& d = mod.sle->data();
                    s.addRaw(d.data(), d.size());
                    stateMap_.updateGiveItem(
                        SHAMapNodeType::TnAccountState,
                        makeShamapitem(key, s.slice()));
                    break;
                }
                case Action::Erase:
                    stateMap_.delItem(key);
                    break;
            }
        }
        mods_.clear();
    }

private:
    enum class Action
    {
        Cache,
        Insert,
        Modify,
        Erase,
    };

    struct Mod
    {
        Action action;
        std::shared_ptr<InnerSLE> sle;
    };

    SHAMap& stateMap_;
    std::map<uint256, Mod> mods_;
};

}  // namespace inner
}  // namespace xrpl
