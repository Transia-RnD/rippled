#include <xrpl/tx/transactors/coupon/CouponUnregister.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/helpers/CouponHelpers.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

namespace xrpl {

NotTEC
CouponUnregister::preflight(PreflightContext const& ctx)
{
    if (ctx.tx[sfCouponScheduleID] == beast::kZero)
        return temMALFORMED;

    if (auto const units = ctx.tx[~sfAmount]; units && (*units <= beast::kZero || units->native()))
        return temBAD_AMOUNT;

    return tesSUCCESS;
}

TER
CouponUnregister::preclaim(PreclaimContext const& ctx)
{
    auto const scheduleID = ctx.tx[sfCouponScheduleID];
    auto const registration =
        ctx.view.read(keylet::couponRegistration(scheduleID, ctx.tx[sfAccount]));
    if (!registration)
        return tecNO_ENTRY;

    if (auto const units = ctx.tx[~sfAmount])
    {
        STAmount const registered = registration->at(sfRegisteredUnits);
        if (units->asset() != registered.asset())
            return tecWRONG_ASSET;
        if (*units > registered)
            return tecINSUFFICIENT_FUNDS;
    }

    return tesSUCCESS;
}

TER
CouponUnregister::doApply()
{
    auto const& tx = ctx_.tx;
    auto const scheduleID = tx[sfCouponScheduleID];

    auto schedule = view().peek(keylet::couponSchedule(scheduleID));
    if (!schedule)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto registration = view().peek(keylet::couponRegistration(scheduleID, accountID_));
    if (!registration)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const holderSle = view().peek(keylet::account(accountID_));
    if (!holderSle)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const now = view().header().parentCloseTime.time_since_epoch().count();
    if (auto const ter = couponSettle(schedule, registration, static_cast<std::uint32_t>(now), j_);
        !isTesSuccess(ter))
        return ter;

    STAmount const registered = registration->at(sfRegisteredUnits);
    STAmount const release = tx[~sfAmount].value_or(registered);
    if (release > registered)
        return tecINSUFFICIENT_FUNDS;

    if (auto const ter = couponUnlockUnits(
            ctx_.getApplyViewContext(), holderSle, preFeeBalance_, release, accountID_, j_);
        !isTesSuccess(ter))
        return ter;

    registration->at(sfRegisteredUnits) = registered - release;

    if (auto const deleted =
            couponDeleteRegistrationIfEmpty(view(), schedule, registration, accountID_, j_))
    {
        if (!isTesSuccess(*deleted))
            return *deleted;  // LCOV_EXCL_LINE
        view().update(schedule);
    }
    else
    {
        view().update(registration);
    }

    return tesSUCCESS;
}

void
CouponUnregister::visitInvariantEntry(bool, SLE::const_ref, SLE::const_ref)
{
    // No transaction-specific invariants yet (future work).
}

bool
CouponUnregister::finalizeInvariants(
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
