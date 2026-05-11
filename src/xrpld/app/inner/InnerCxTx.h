#pragma once

#include <xrpl/basics/base_uint.h>
#include <xrpl/shamap/SHAMap.h>

#include <map>
#include <memory>

namespace xrpl {
namespace inner {

class InnerCxTx
{
public:
    using ID = uint256;

    InnerCxTx(boost::intrusive_ptr<SHAMapItem const> txn)
        : tx(std::move(txn))
    {
    }

    [[nodiscard]] ID const&
    id() const
    {
        return tx->key();
    }

    boost::intrusive_ptr<SHAMapItem const> tx;
};

class InnerTxSet
{
public:
    using ID = uint256;
    using Tx = InnerCxTx;

    class MutableTxSet
    {
        friend class InnerTxSet;
        std::shared_ptr<SHAMap> map_;

    public:
        explicit MutableTxSet(InnerTxSet const& src)
            : map_(src.map->snapShot(true))
        {
        }

        bool
        insert(Tx const& t)
        {
            return map_->addItem(SHAMapNodeType::TnTransactionNm, t.tx);
        }

        bool
        erase(Tx::ID const& entry)
        {
            return map_->delItem(entry);
        }
    };

    explicit InnerTxSet(std::shared_ptr<SHAMap> m) : map(std::move(m))
    {
    }

    InnerTxSet(MutableTxSet const& m) : map(m.map_->snapShot(false))
    {
    }

    [[nodiscard]] bool
    exists(Tx::ID const& entry) const
    {
        return map->hasItem(entry);
    }

    [[nodiscard]] boost::intrusive_ptr<SHAMapItem const> const&
    find(Tx::ID const& entry) const
    {
        return map->peekItem(entry);
    }

    [[nodiscard]] ID
    id() const
    {
        return map->getHash().asUint256();
    }

    [[nodiscard]] std::map<Tx::ID, bool>
    compare(InnerTxSet const& j) const
    {
        SHAMap::Delta delta;
        map->compare(*(j.map), delta, 65536);

        std::map<Tx::ID, bool> ret;
        for (auto const& [k, v] : delta)
            ret[k] = v.first != nullptr;
        return ret;
    }

    std::shared_ptr<SHAMap> map;
};

}  // namespace inner
}  // namespace xrpl
