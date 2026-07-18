#include <xrpl/ledger/helpers/CouponHelpers.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/Number.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/ledger/helpers/EscrowHelpers.h>
#include <xrpl/ledger/helpers/MPTokenHelpers.h>
#include <xrpl/ledger/helpers/RippleStateHelpers.h>
#include <xrpl/ledger/helpers/TokenHelpers.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/Rate.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/TER.h>

#include <cstdint>
#include <variant>

namespace xrpl {

TER
couponSettle(
    SLE::const_ref schedule,
    SLE::ref registration,
    std::uint32_t parentCloseTime,
    beast::Journal j)
{
    std::uint32_t const first = schedule->at(sfFirstCouponTime);
    std::uint32_t const interval = schedule->at(sfCouponInterval);
    auto const expiration = schedule->at(~sfExpiration);
    std::uint32_t const lastSettled = registration->at(sfLastSettledTime);

    // Coupon dates occur strictly before Expiration.
    std::int64_t effectiveNow = parentCloseTime;
    if (expiration && static_cast<std::int64_t>(*expiration) <= effectiveNow)
        effectiveNow = static_cast<std::int64_t>(*expiration) - 1;

    if (effectiveNow < static_cast<std::int64_t>(first))
        return tesSUCCESS;

    // Coupon dates are first + k * interval. Work in date indices so a
    // LastSettledTime before the first date (the "nothing settled yet"
    // state) needs no sentinel arithmetic.
    std::int64_t const latestK = (effectiveNow - static_cast<std::int64_t>(first)) / interval;
    std::int64_t const lastK =
        lastSettled < first ? -1 : static_cast<std::int64_t>((lastSettled - first) / interval);

    if (latestK <= lastK)
        return tesSUCCESS;

    std::int64_t const count = latestK - lastK;
    STAmount const units = registration->at(sfRegisteredUnits);
    if (units != beast::kZero)
    {
        STAmount const couponAmount = schedule->at(sfCouponAmount);
        STAmount const accrued = registration->at(sfAccruedAmount);

        NumberRoundModeGuard const mg(Number::RoundingMode::Downward);
        Number const addition = Number(count) * Number(units) * Number(couponAmount);
        STAmount const additionAmt{couponAmount.asset(), addition};

        if (!canAdd(accrued, additionAmt))
        {
            JLOG(j.debug()) << "couponSettle: accrual addition loses precision.";
            return tecPRECISION_LOSS;
        }
        registration->at(sfAccruedAmount) = accrued + additionAmt;
    }
    registration->at(sfLastSettledTime) =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(first) + latestK * interval);

    return tesSUCCESS;
}

template <ValidIssueType T>
static TER
couponLockPreclaimHelper(
    ReadView const& view,
    AccountID const& holder,
    STAmount const& units,
    beast::Journal j);

template <>
TER
couponLockPreclaimHelper<Issue>(
    ReadView const& view,
    AccountID const& holder,
    STAmount const& units,
    beast::Journal j)
{
    auto const& issue = units.get<Issue>();
    AccountID const& issuer = units.getIssuer();
    if (issuer == holder)
        return tecNO_PERMISSION;

    auto const sleIssuer = view.read(keylet::account(issuer));
    if (!sleIssuer)
        return tecNO_ISSUER;
    if (!sleIssuer->isFlag(lsfAllowTrustLineLocking))
        return tecNO_PERMISSION;

    auto const sleRippleState = view.read(keylet::trustLine(holder, issuer, issue.currency));
    if (!sleRippleState)
        return tecNO_LINE;

    if (auto const ter = requireAuth(view, issue, holder); !isTesSuccess(ter))
        return ter;

    if (isFrozen(view, holder, issue))
        return tecFROZEN;

    STAmount const spendable =
        accountHolds(view, holder, issue.currency, issuer, FreezeHandling::IgnoreFreeze, j);
    if (spendable <= beast::kZero || spendable < units)
        return tecINSUFFICIENT_FUNDS;

    if (!canAdd(spendable, units))
        return tecPRECISION_LOSS;

    return tesSUCCESS;
}

template <>
TER
couponLockPreclaimHelper<MPTIssue>(
    ReadView const& view,
    AccountID const& holder,
    STAmount const& units,
    beast::Journal j)
{
    AccountID const issuer = units.getIssuer();
    if (issuer == holder)
        return tecNO_PERMISSION;

    auto const& mptIssue = units.get<MPTIssue>();
    auto const issuanceKey = keylet::mptokenIssuance(mptIssue.getMptID());
    auto const sleIssuance = view.read(issuanceKey);
    if (!sleIssuance)
        return tecOBJECT_NOT_FOUND;

    if (!sleIssuance->isFlag(lsfMPTCanEscrow))
        return tecNO_PERMISSION;

    if (!view.exists(keylet::mptoken(issuanceKey.key, holder)))
        return tecOBJECT_NOT_FOUND;

    if (auto const ter = requireAuth(view, mptIssue, holder, AuthType::WeakAuth);
        !isTesSuccess(ter))
        return ter;

    if (isFrozen(view, holder, mptIssue))
        return tecLOCKED;

    if (auto const ter = canTransfer(view, mptIssue, holder, issuer); !isTesSuccess(ter))
        return ter;

    STAmount const spendable = accountHolds(
        view, holder, mptIssue, FreezeHandling::IgnoreFreeze, AuthHandling::IgnoreAuth, j);
    if (spendable <= beast::kZero || spendable < units)
        return tecINSUFFICIENT_FUNDS;

    return tesSUCCESS;
}

TER
couponLockPreclaim(
    ReadView const& view,
    AccountID const& holder,
    STAmount const& units,
    beast::Journal j)
{
    return std::visit(
        [&]<typename T>(T const&) { return couponLockPreclaimHelper<T>(view, holder, units, j); },
        units.asset().value());
}

TER
couponLockUnits(ApplyView& view, AccountID const& holder, STAmount const& units, beast::Journal j)
{
    AccountID const issuer = units.getIssuer();
    if (issuer == holder)
        return tecINTERNAL;  // LCOV_EXCL_LINE

    if (units.holds<MPTIssue>())
        return lockEscrowMPT(view, holder, units, j);

    return directSendNoFee(view, holder, issuer, units, true, j);
}

TER
couponUnlockUnits(
    ApplyViewContext ctx,
    SLE::ref sleHolder,
    XRPAmount preFeeBalance,
    STAmount const& units,
    AccountID const& holder,
    beast::Journal j)
{
    return std::visit(
        [&]<typename T>(T const&) {
            return escrowUnlockApplyHelper<T>(
                ctx,
                kParityRate,
                sleHolder,
                preFeeBalance,
                units,
                units.getIssuer(),
                holder,  // sender and receiver are the same
                holder,
                true,
                j);
        },
        units.asset().value());
}

std::optional<TER>
couponDeleteRegistrationIfEmpty(
    ApplyView& view,
    SLE::ref schedule,
    SLE::ref registration,
    AccountID const& holder,
    beast::Journal j)
{
    STAmount const units = registration->at(sfRegisteredUnits);
    STAmount const accrued = registration->at(sfAccruedAmount);
    if (units != beast::kZero || accrued != beast::kZero)
        return std::nullopt;

    if (!view.dirRemove(
            keylet::ownerDir(holder), registration->at(sfOwnerNode), registration->key(), false))
    {
        // LCOV_EXCL_START
        JLOG(j.fatal()) << "couponDeleteRegistrationIfEmpty: failed to remove dir link.";
        return tefBAD_LEDGER;
        // LCOV_EXCL_STOP
    }
    decreaseOwnerCount(view, holder, std::nullopt, 1, j);
    view.erase(registration);

    std::uint32_t const regCount = schedule->at(sfRegistrationCount);
    if (regCount == 0)
        return tefINTERNAL;  // LCOV_EXCL_LINE
    schedule->at(sfRegistrationCount) = regCount - 1;

    return tesSUCCESS;
}

}  // namespace xrpl
