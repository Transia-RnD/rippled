#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/ledger/helpers/MPTokenHelpers.h>
#include <xrpl/ledger/helpers/RippleStateHelpers.h>
#include <xrpl/ledger/helpers/TokenHelpers.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Concepts.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/TER.h>

namespace xrpl {

// Verify that `account` can pay `amount` to `dest` for an IOU or MPT before any
// state changes. Mirrors payment-side authorization/freeze/liquidity checks.
template <ValidIssueType T>
static TER
canTransferTokenHelper(
    ReadView const& view,
    AccountID const& account,
    AccountID const& dest,
    STAmount const& amount,
    beast::Journal const& j);

template <>
inline TER
canTransferTokenHelper<Issue>(
    ReadView const& view,
    AccountID const& account,
    AccountID const& dest,
    STAmount const& amount,
    beast::Journal const& j)
{
    AccountID const issuer = amount.getIssuer();
    if (issuer == account)
        return tesSUCCESS;

    auto const sleIssuer = view.read(keylet::account(issuer));
    if (!sleIssuer)
        return tecNO_ISSUER;

    auto const sleRippleState =
        view.read(keylet::trustLine(account, issuer, amount.get<Issue>().currency));
    if (!sleRippleState)
        return tecNO_LINE;

    STAmount const balance = (*sleRippleState)[sfBalance];

    // Issuer must be on the correct side of the trust line for its balance sign.
    if (balance > beast::kZero && issuer < account)
        return tecNO_PERMISSION;
    if (balance < beast::kZero && issuer > account)
        return tecNO_PERMISSION;

    if (auto const ter = requireAuth(view, amount.get<Issue>(), account); ter != tesSUCCESS)
        return ter;

    if (auto const ter = requireAuth(view, amount.get<Issue>(), dest); ter != tesSUCCESS)
        return ter;

    if (isFrozen(view, account, amount.get<Issue>()) ||
        isDeepFrozen(view, account, amount.get<Issue>().currency, issuer))
        return tecFROZEN;

    if (isFrozen(view, dest, amount.get<Issue>()) ||
        isDeepFrozen(view, dest, amount.get<Issue>().currency, issuer))
        return tecFROZEN;

    STAmount const spendableAmount = accountHolds(
        view, account, amount.get<Issue>().currency, issuer, FreezeHandling::IgnoreFreeze, j);

    if (spendableAmount <= beast::kZero)
        return tecINSUFFICIENT_FUNDS;

    if (spendableAmount < amount)
        return tecINSUFFICIENT_FUNDS;

    if (!canAdd(spendableAmount, amount))
        return tecPRECISION_LOSS;

    return tesSUCCESS;
}

template <>
inline TER
canTransferTokenHelper<MPTIssue>(
    ReadView const& view,
    AccountID const& account,
    AccountID const& dest,
    STAmount const& amount,
    beast::Journal const& j)
{
    AccountID const issuer = amount.getIssuer();
    if (issuer == account)
        return tesSUCCESS;

    auto const issuanceKey = keylet::mptokenIssuance(amount.get<MPTIssue>().getMptID());
    auto const sleIssuance = view.read(issuanceKey);
    if (!sleIssuance)
        return tecOBJECT_NOT_FOUND;

    if (sleIssuance->getAccountID(sfIssuer) != issuer)
        return tecNO_PERMISSION;

    if (!view.exists(keylet::mptoken(issuanceKey.key, account)))
        return tecOBJECT_NOT_FOUND;

    auto const& mptIssue = amount.get<MPTIssue>();
    if (auto const ter = requireAuth(view, mptIssue, account, AuthType::WeakAuth);
        ter != tesSUCCESS)
        return ter;

    if (auto const ter = requireAuth(view, mptIssue, dest, AuthType::WeakAuth); ter != tesSUCCESS)
        return ter;

    if (isFrozen(view, account, mptIssue))
        return tecLOCKED;

    if (isFrozen(view, dest, mptIssue))
        return tecLOCKED;

    if (auto const ter = canTransfer(view, mptIssue, account, dest); ter != tesSUCCESS)
        return ter;

    STAmount const spendableAmount = accountHolds(
        view, account, mptIssue, FreezeHandling::IgnoreFreeze, AuthHandling::IgnoreAuth, j);

    if (spendableAmount <= beast::kZero)
        return tecINSUFFICIENT_FUNDS;

    if (spendableAmount < amount)
        return tecINSUFFICIENT_FUNDS;

    if (!canAdd(spendableAmount, amount))
        return tecPRECISION_LOSS;

    return tesSUCCESS;
}

}  // namespace xrpl
