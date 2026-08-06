#include <xrpl/tx/transactors/coupon/CouponScheduleCreate.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/View.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

#include <variant>

namespace xrpl {

NotTEC
CouponScheduleCreate::preflight(PreflightContext const& ctx)
{
    auto const bondAsset = ctx.tx[sfBondAsset];
    STAmount const couponAmount = ctx.tx[sfCouponAmount];

    // The bond is a token: IOU or MPT, never XRP.
    if (bondAsset.native())
    {
        JLOG(ctx.j.debug()) << "CouponScheduleCreate: bond asset cannot be XRP.";
        return temMALFORMED;
    }

    if (couponAmount <= beast::kZero)
        return temBAD_AMOUNT;

    // A bond paying coupons in itself is rebasing, not interest.
    if (bondAsset == couponAmount.asset())
    {
        JLOG(ctx.j.debug()) << "CouponScheduleCreate: bond and coupon asset are the same.";
        return temMALFORMED;
    }

    if (ctx.tx[sfCouponInterval] == 0)
        return temMALFORMED;

    std::uint32_t const first = ctx.tx[sfFirstCouponTime];
    if (first == 0)
        return temMALFORMED;

    auto const expiration = ctx.tx[~sfExpiration];
    if (expiration && *expiration <= first)
        return temBAD_EXPIRATION;

    if (auto const notice = ctx.tx[~sfCallNoticePeriod]; notice && *notice == 0)
        return temMALFORMED;

    if (auto const earliestCall = ctx.tx[~sfEarliestCallTime])
    {
        // Call protection is meaningless on a non-callable schedule.
        if (!ctx.tx.isFieldPresent(sfCallNoticePeriod))
            return temMALFORMED;
        if (expiration && *earliestCall >= *expiration)
            return temBAD_EXPIRATION;
    }

    return tesSUCCESS;
}

TER
CouponScheduleCreate::preclaim(PreclaimContext const& ctx)
{
    auto const now = ctx.view.header().parentCloseTime.time_since_epoch().count();
    if (ctx.tx[sfFirstCouponTime] <= now)
        return tecEXPIRED;

    AccountID const account = ctx.tx[sfAccount];
    auto const bondAsset = ctx.tx[sfBondAsset];

    // One schedule per (Account, BondAsset): the keylet is derived from
    // the pair, so a duplicate collides here.
    if (ctx.view.exists(keylet::couponSchedule(account, bondAsset)))
        return tecDUPLICATE;

    auto const assetExists = [&](Asset const& asset) -> TER {
        return std::visit(
            [&]<typename T>(T const& issue) -> TER {
                if constexpr (std::is_same_v<T, Issue>)
                {
                    if (!issue.native() && !ctx.view.exists(keylet::account(issue.account)))
                        return tecNO_ISSUER;
                }
                else
                {
                    if (!ctx.view.exists(keylet::mptokenIssuance(issue.getMptID())))
                        return tecOBJECT_NOT_FOUND;
                }
                return tesSUCCESS;
            },
            asset.value());
    };

    if (auto const ter = assetExists(bondAsset); !isTesSuccess(ter))
        return ter;

    STAmount const couponAmount = ctx.tx[sfCouponAmount];
    if (auto const ter = assetExists(couponAmount.asset()); !isTesSuccess(ter))
        return ter;

    return tesSUCCESS;
}

TER
CouponScheduleCreate::doApply()
{
    auto const& tx = ctx_.tx;

    auto const owner = view().peek(keylet::account(accountID_));
    if (!owner)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const bondAsset = tx[sfBondAsset];
    auto schedule = std::make_shared<SLE>(keylet::couponSchedule(accountID_, bondAsset));

    schedule->at(sfAccount) = accountID_;
    schedule->setFieldIssue(sfBondAsset, STIssue{sfBondAsset, bondAsset});
    schedule->at(sfCouponAmount) = tx[sfCouponAmount];
    schedule->at(sfCouponInterval) = tx[sfCouponInterval];
    schedule->at(sfFirstCouponTime) = tx[sfFirstCouponTime];
    if (auto const expiration = tx[~sfExpiration])
        schedule->at(sfExpiration) = *expiration;
    if (auto const notice = tx[~sfCallNoticePeriod])
        schedule->at(sfCallNoticePeriod) = *notice;
    if (auto const earliestCall = tx[~sfEarliestCallTime])
        schedule->at(sfEarliestCallTime) = *earliestCall;

    if (auto const ter = dirLink(view(), accountID_, schedule))
        return ter;  // LCOV_EXCL_LINE
    increaseOwnerCount(view(), owner, {}, 1, j_);
    if (preFeeBalance_ < accountReserve(view(), owner, j_))
        return tecINSUFFICIENT_RESERVE;

    view().insert(schedule);

    return tesSUCCESS;
}

void
CouponScheduleCreate::visitInvariantEntry(bool, SLE::const_ref, SLE::const_ref)
{
    // No transaction-specific invariants yet (future work).
}

bool
CouponScheduleCreate::finalizeInvariants(
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
