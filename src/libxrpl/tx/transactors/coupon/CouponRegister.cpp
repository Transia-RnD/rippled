#include <xrpl/tx/transactors/coupon/CouponRegister.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/View.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/ledger/helpers/CouponHelpers.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

namespace xrpl {

NotTEC
CouponRegister::preflight(PreflightContext const& ctx)
{
    if (ctx.tx[sfCouponScheduleID] == beast::kZero)
        return temMALFORMED;

    STAmount const units = ctx.tx[sfAmount];
    if (units <= beast::kZero || units.native())
        return temBAD_AMOUNT;

    return tesSUCCESS;
}

TER
CouponRegister::preclaim(PreclaimContext const& ctx)
{
    auto const schedule = ctx.view.read(keylet::couponSchedule(ctx.tx[sfCouponScheduleID]));
    if (!schedule)
        return tecNO_ENTRY;

    STAmount const units = ctx.tx[sfAmount];
    if (units.asset() != schedule->at(sfBondAsset))
        return tecWRONG_ASSET;

    // Registering into a schedule with no future coupon dates is always
    // a mistake.
    auto const now = ctx.view.header().parentCloseTime.time_since_epoch().count();
    if (auto const expiration = schedule->at(~sfExpiration);
        expiration && *expiration <= static_cast<std::uint32_t>(now))
        return tecEXPIRED;

    return couponLockPreclaim(ctx.view, ctx.tx[sfAccount], units, ctx.j);
}

TER
CouponRegister::doApply()
{
    auto const& tx = ctx_.tx;
    auto const scheduleID = tx[sfCouponScheduleID];

    auto schedule = view().peek(keylet::couponSchedule(scheduleID));
    if (!schedule)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const holderSle = view().peek(keylet::account(accountID_));
    if (!holderSle)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    STAmount const units = tx[sfAmount];
    STAmount const couponAmount = schedule->at(sfCouponAmount);

    auto const regKeylet = keylet::couponRegistration(scheduleID, accountID_);
    auto registration = view().peek(regKeylet);
    bool const created = !registration;
    if (created)
    {
        registration = std::make_shared<SLE>(regKeylet);
        registration->at(sfAccount) = accountID_;
        registration->at(sfCouponScheduleID) = scheduleID;
        registration->at(sfRegisteredUnits) = STAmount{units.asset()};
        registration->at(sfAccruedAmount) = STAmount{couponAmount.asset()};
        // Any value before FirstCouponTime means "nothing settled yet";
        // couponSettle advances it to the most recent elapsed date, so
        // newly registered units earn nothing for dates already past.
        registration->at(sfLastSettledTime) = 0;

        if (auto const ter = dirLink(view(), accountID_, registration))
            return ter;  // LCOV_EXCL_LINE
        increaseOwnerCount(view(), holderSle, {}, 1, j_);
        if (preFeeBalance_ < accountReserve(view(), holderSle, j_))
            return tecINSUFFICIENT_RESERVE;
    }

    auto const now = view().header().parentCloseTime.time_since_epoch().count();
    if (auto const ter = couponSettle(schedule, registration, static_cast<std::uint32_t>(now), j_);
        !isTesSuccess(ter))
        return ter;

    if (auto const ter = couponLockUnits(view(), accountID_, units, j_); !isTesSuccess(ter))
        return ter;

    registration->at(sfRegisteredUnits) = STAmount(registration->at(sfRegisteredUnits)) + units;

    if (created)
    {
        schedule->at(sfRegistrationCount) = schedule->at(sfRegistrationCount) + 1;
        view().update(schedule);
        view().insert(registration);
    }
    else
    {
        view().update(registration);
    }

    return tesSUCCESS;
}

void
CouponRegister::visitInvariantEntry(bool, SLE::const_ref, SLE::const_ref)
{
    // No transaction-specific invariants yet (future work).
}

bool
CouponRegister::finalizeInvariants(
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
