#include <xrpl/tx/transactors/subscription/SubscriptionCancel.h>

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/tx/Transactor.h>

#include <cstdint>

namespace xrpl {

NotTEC
SubscriptionCancel::preflight(PreflightContext const& ctx)
{
    return tesSUCCESS;
}

TER
SubscriptionCancel::preclaim(PreclaimContext const& ctx)
{
    auto const sleSub = ctx.view.read(keylet::subscription(ctx.tx.getFieldH256(sfSubscriptionID)));
    if (!sleSub)
    {
        JLOG(ctx.j.debug()) << "SubscriptionCancel: Subscription does not exist.";
        return tecNO_ENTRY;
    }

    // The owner or the destination may cancel at any time; anyone may cancel
    // once the subscription has expired.
    if (!hasExpired(ctx.view, (*sleSub)[~sfExpiration]))
    {
        AccountID const account = ctx.tx.getAccountID(sfAccount);
        if (account != sleSub->getAccountID(sfAccount) &&
            account != sleSub->getAccountID(sfDestination))
        {
            JLOG(ctx.j.debug()) << "SubscriptionCancel: Account is not the owner "
                                   "or destination of the subscription.";
            return tecNO_PERMISSION;
        }
    }

    return tesSUCCESS;
}

TER
SubscriptionCancel::doApply()
{
    Sandbox sb(&ctx_.view());

    auto const sleSub = sb.peek(keylet::subscription(ctx_.tx.getFieldH256(sfSubscriptionID)));
    if (!sleSub)
    {
        JLOG(ctx_.journal.debug()) << "SubscriptionCancel: Subscription does not exist.";
        return tecINTERNAL;
    }

    AccountID const account{sleSub->getAccountID(sfAccount)};
    AccountID const dstAcct{sleSub->getAccountID(sfDestination)};
    auto viewJ = ctx_.registry.get().getJournal("View");

    std::uint64_t const ownerPage{(*sleSub)[sfOwnerNode]};
    if (!sb.dirRemove(keylet::ownerDir(account), ownerPage, sleSub->key(), true))
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

void
SubscriptionCancel::visitInvariantEntry(bool, SLE::const_ref, SLE::const_ref)
{
    // No transaction-specific invariants yet (future work).
}

bool
SubscriptionCancel::finalizeInvariants(
    STTx const&,
    TER,
    XRPAmount,
    ReadView const&,
    beast::Journal const&)
{
    // No transaction-specific invariants yet (future work).
    return true;
}

}  // namespace xrpl
