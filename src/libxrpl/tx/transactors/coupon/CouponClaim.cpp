#include <xrpl/tx/transactors/coupon/CouponClaim.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/ledger/helpers/CouponHelpers.h>
#include <xrpl/ledger/helpers/MPTokenHelpers.h>
#include <xrpl/ledger/helpers/RippleStateHelpers.h>
#include <xrpl/ledger/helpers/TokenHelpers.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

#include <variant>

namespace xrpl {

NotTEC
CouponClaim::preflight(PreflightContext const& ctx)
{
    if (ctx.tx[sfCouponScheduleID] == beast::kZero)
        return temMALFORMED;

    if (auto const amount = ctx.tx[~sfAmount]; amount && *amount <= beast::kZero)
        return temBAD_AMOUNT;

    return tesSUCCESS;
}

TER
CouponClaim::preclaim(PreclaimContext const& ctx)
{
    auto const scheduleID = ctx.tx[sfCouponScheduleID];
    AccountID const holder = ctx.tx[sfAccount];

    auto const registration = ctx.view.read(keylet::couponRegistration(scheduleID, holder));
    if (!registration)
        return tecNO_ENTRY;

    auto const schedule = ctx.view.read(keylet::couponSchedule(scheduleID));
    if (!schedule)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    STAmount const couponAmount = schedule->at(sfCouponAmount);
    auto const& couponAsset = couponAmount.asset();

    if (auto const amount = ctx.tx[~sfAmount]; amount && amount->asset() != couponAsset)
        return tecWRONG_ASSET;

    if (!couponAsset.native())
    {
        AccountID const payer = schedule->at(sfAccount);
        bool const mpt = couponAsset.holds<MPTIssue>();

        // Coupon-asset eligibility on both sides.
        for (auto const& party : {payer, holder})
        {
            if (party == couponAsset.getIssuer())
                continue;
            if (auto const ter = requireAuth(ctx.view, couponAsset, party); !isTesSuccess(ter))
                return ter;
            if (isFrozen(ctx.view, party, couponAsset))
                return mpt ? tecLOCKED : tecFROZEN;
        }
    }

    return tesSUCCESS;
}

TER
CouponClaim::doApply()
{
    auto const& tx = ctx_.tx;
    auto const scheduleID = tx[sfCouponScheduleID];

    auto schedule = view().peek(keylet::couponSchedule(scheduleID));
    if (!schedule)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto registration = view().peek(keylet::couponRegistration(scheduleID, accountID_));
    if (!registration)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const now = view().header().parentCloseTime.time_since_epoch().count();
    if (auto const ter = couponSettle(schedule, registration, static_cast<std::uint32_t>(now), j_);
        !isTesSuccess(ter))
        return ter;

    STAmount const accrued = registration->at(sfAccruedAmount);
    if (accrued <= beast::kZero)
        return tecNO_PERMISSION;

    STAmount const requested = tx[~sfAmount].value_or(accrued);
    STAmount const pay = std::min(requested, accrued);

    AccountID const payer = schedule->at(sfAccount);
    auto const& couponAsset = pay.asset();

    // The Issuer does not escrow coupons: claims pull from the live
    // balance. A failed claim is the visible, timestamped record that
    // the coupon was presented and not honored.
    if (couponAsset.native())
    {
        if (xrpLiquid(view(), payer, 0, j_) < pay.xrp())
            return tecINSUFFICIENT_FUNDS;
    }
    else if (payer != couponAsset.getIssuer())
    {
        STAmount const spendable = std::visit(
            [&]<typename T>(T const& issue) {
                if constexpr (std::is_same_v<T, Issue>)
                    return accountHolds(
                        view(),
                        payer,
                        issue.currency,
                        issue.account,
                        FreezeHandling::ZeroIfFrozen,
                        j_);
                else
                    return accountHolds(
                        view(),
                        payer,
                        issue,
                        FreezeHandling::ZeroIfFrozen,
                        AuthHandling::IgnoreAuth,
                        j_);
            },
            couponAsset.value());
        if (spendable < pay)
            return tecINSUFFICIENT_FUNDS;
    }

    // Reserve-gated auto-creation of the holder's trust line / MPToken.
    if (!couponAsset.native() && accountID_ != couponAsset.getIssuer())
    {
        auto const ter = std::visit(
            [&]<typename T>(T const& issue) {
                return addEmptyHolding(
                    ctx_.getApplyViewContext(), accountID_, preFeeBalance_, issue, j_);
            },
            couponAsset.value());
        // An existing holding is fine.
        if (!isTesSuccess(ter) && ter != tecDUPLICATE)
            return ter;
    }

    if (auto const ter = accountSend(view(), payer, accountID_, pay, j_); !isTesSuccess(ter))
        return ter;

    registration->at(sfAccruedAmount) = accrued - pay;

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
CouponClaim::visitInvariantEntry(bool, SLE::const_ref, SLE::const_ref)
{
    // No transaction-specific invariants yet (future work).
}

bool
CouponClaim::finalizeInvariants(STTx const&, TER, XRPAmount, ReadView const&, beast::Journal const&)
{
    // No transaction-specific invariants yet (future work).
    return true;
}

}  // namespace xrpl
