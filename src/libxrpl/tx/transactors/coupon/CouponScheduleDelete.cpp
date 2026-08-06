#include <xrpl/tx/transactors/coupon/CouponScheduleDelete.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

namespace xrpl {

NotTEC
CouponScheduleDelete::preflight(PreflightContext const& ctx)
{
    if (ctx.tx[sfCouponScheduleID] == beast::kZero)
        return temMALFORMED;

    return tesSUCCESS;
}

TER
CouponScheduleDelete::preclaim(PreclaimContext const& ctx)
{
    auto const schedule = ctx.view.read(keylet::couponSchedule(ctx.tx[sfCouponScheduleID]));
    if (!schedule)
        return tecNO_ENTRY;

    if (ctx.tx[sfAccount] != schedule->at(sfAccount))
        return tecNO_PERMISSION;

    if (schedule->at(sfRegistrationCount) > 0)
        return tecHAS_OBLIGATIONS;

    return tesSUCCESS;
}

TER
CouponScheduleDelete::doApply()
{
    auto const k = keylet::couponSchedule(ctx_.tx[sfCouponScheduleID]);
    auto schedule = view().peek(k);
    if (!schedule)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    if (!view().dirRemove(keylet::ownerDir(accountID_), schedule->at(sfOwnerNode), k.key, false))
    {
        // LCOV_EXCL_START
        JLOG(j_.fatal()) << "CouponScheduleDelete: failed to remove dir link.";
        return tefBAD_LEDGER;
        // LCOV_EXCL_STOP
    }
    decreaseOwnerCount(view(), accountID_, std::nullopt, 1, j_);
    view().erase(schedule);

    return tesSUCCESS;
}

void
CouponScheduleDelete::visitInvariantEntry(bool, SLE::const_ref, SLE::const_ref)
{
    // No transaction-specific invariants yet (future work).
}

bool
CouponScheduleDelete::finalizeInvariants(
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
