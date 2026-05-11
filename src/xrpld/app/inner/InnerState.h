#pragma once

#include <xrpld/app/inner/detail/InnerAccountHelpers.h>
#include <xrpld/app/inner/detail/InnerTransactor.h>
#include <xrpld/app/inner/detail/InnerView.h>
#include <xrpld/app/inner/transactors/InnerCredit.h>
#include <xrpld/app/inner/transactors/InnerTransfer.h>
#include <xrpld/app/inner/transactors/InnerWithdraw.h>

#include <mutex>
#include <vector>

namespace xrpl {
namespace inner {

class InnerTxQueue
{
public:
    static constexpr std::size_t kMaxPending = 10000;

    bool
    submit(InnerTx tx)
    {
        std::lock_guard lock(mutex_);
        if (pending_.size() >= kMaxPending)
            return false;
        pending_.push_back(std::move(tx));
        return true;
    }

    std::vector<InnerTx>
    drain()
    {
        std::lock_guard lock(mutex_);
        std::vector<InnerTx> result;
        result.swap(pending_);
        return result;
    }

    std::size_t
    size() const
    {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<InnerTx> pending_;
};

inline void
registerBuiltinTransactors(InnerTransactorRegistry& registry)
{
    registry.registerType(
        InnerTxType::Credit,
        "Credit",
        [](InnerApplyContext& ctx) {
            return std::make_unique<InnerCredit>(ctx);
        });

    registry.registerType(
        InnerTxType::Transfer,
        "Transfer",
        [](InnerApplyContext& ctx) {
            return std::make_unique<InnerTransfer>(ctx);
        });

    registry.registerType(
        InnerTxType::Withdraw,
        "Withdraw",
        [](InnerApplyContext& ctx) {
            return std::make_unique<InnerWithdraw>(ctx);
        });
}

}  // namespace inner
}  // namespace xrpl
