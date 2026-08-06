#include <xrpl/tx/transactors/dex/SubscriptionCancel.h>

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STAccount.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
SubscriptionCancel::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureSubscription))
        return temDISABLED;

    return tesSUCCESS;
}

TER
SubscriptionCancel::preclaim(PreclaimContext const& ctx)
{
    auto const sleSub = ctx.view.read(
        keylet::subscription(ctx.tx.getFieldH256(sfSubscriptionID)));
    if (!sleSub)
    {
        JLOG(ctx.j.debug())
            << "SubscriptionCancel: Subscription does not exist.";
        return tecNO_ENTRY;
    }

    return tesSUCCESS;
}

TER
SubscriptionCancel::doApply()
{
    Sandbox sb(&ctx_.view());

    auto const sleSub =
        sb.peek(keylet::subscription(ctx_.tx.getFieldH256(sfSubscriptionID)));
    if (!sleSub)
    {
        JLOG(ctx_.journal.debug())
            << "SubscriptionCancel: Subscription does not exist.";
        return tecINTERNAL;
    }

    AccountID const account{sleSub->getAccountID(sfAccount)};
    AccountID const dstAcct{sleSub->getAccountID(sfDestination)};
    auto const& viewJ = j_;

    std::uint64_t const ownerPage{(*sleSub)[sfOwnerNode]};
    if (!sb.dirRemove(
            keylet::ownerDir(account), ownerPage, sleSub->key(), true))
    {
        JLOG(j_.fatal()) << "Unable to delete subscription from source.";
        return tefBAD_LEDGER;
    }

    std::uint64_t const destPage{(*sleSub)[sfDestinationNode]};
    if (!sb.dirRemove(keylet::ownerDir(dstAcct), destPage, sleSub->key(), true))
    {
        JLOG(j_.fatal()) << "Unable to delete subscription from destination.";
        return tefBAD_LEDGER;
    }

    auto const sleSrc = sb.peek(keylet::account(account));
    sb.erase(sleSub);

    adjustOwnerCount(sb, sleSrc, -1, viewJ);

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
