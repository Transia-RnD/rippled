#pragma once

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/XRPAmount.h>

#include <cstdint>
#include <optional>

namespace xrpl {

/**
 * Settle coupon accrual on a registration.
 *
 * Computes the coupon dates elapsed since the registration's
 * LastSettledTime (strictly before the schedule's Expiration, if any),
 * adds count * RegisteredUnits * CouponAmount to AccruedAmount —
 * rounded once, downward, to the coupon asset's precision — and
 * advances LastSettledTime to the latest elapsed date. O(1) in the
 * number of elapsed dates. The caller must view.update() the
 * registration on tesSUCCESS.
 */
[[nodiscard]] TER
couponSettle(
    SLE::const_ref schedule,
    SLE::ref registration,
    std::uint32_t parentCloseTime,
    beast::Journal j);

/**
 * Eligibility for locking bond units into a registration.
 *
 * Mirrors the token Escrow lock gates: issuer opt-in
 * (lsfAllowTrustLineLocking / lsfMPTCanEscrow), authorization, freeze
 * and lock state, and the holder's spendable balance.
 */
[[nodiscard]] TER
couponLockPreclaim(
    ReadView const& view,
    AccountID const& holder,
    STAmount const& units,
    beast::Journal j);

/**
 * Lock bond units: IOU balances park on the bond issuer's trust line,
 * MPT amounts move into sfLockedAmount — the token Escrow mechanics.
 */
[[nodiscard]] TER
couponLockUnits(ApplyView& view, AccountID const& holder, STAmount const& units, beast::Journal j);

/**
 * Release locked bond units back to the holder's spendable balance.
 * No transfer fee applies (round trip to self).
 */
[[nodiscard]] TER
couponUnlockUnits(
    ApplyViewContext ctx,
    SLE::ref sleHolder,
    XRPAmount preFeeBalance,
    STAmount const& units,
    AccountID const& holder,
    beast::Journal j);

/**
 * Delete a registration if its RegisteredUnits and AccruedAmount are
 * both zero: remove it from the holder's directory, refund the owner
 * reserve, and decrement the schedule's RegistrationCount. The caller
 * must view.update() the schedule when a deletion is reported.
 *
 * @return std::nullopt if the registration is not empty (not deleted);
 *         tesSUCCESS if it was deleted; an error TER on failure.
 */
[[nodiscard]] std::optional<TER>
couponDeleteRegistrationIfEmpty(
    ApplyView& view,
    SLE::ref schedule,
    SLE::ref registration,
    AccountID const& holder,
    beast::Journal j);

}  // namespace xrpl
