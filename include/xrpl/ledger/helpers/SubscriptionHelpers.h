#pragma once

#include <xrpl/basics/Log.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/ledger/helpers/MPTokenHelpers.h>
#include <xrpl/ledger/helpers/RippleStateHelpers.h>
#include <xrpl/ledger/helpers/TokenHelpers.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Concepts.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/TER.h>

namespace xrpl {

/**
 * Check whether `account` can transfer `amount` to `dest`.
 *
 * Used by Subscription transactors to validate a pending periodic payment
 * before it is executed.  Checks issuer existence, trust-line state,
 * authorization, freeze, and spendable balance.
 */
template <ValidIssueType T>
TER
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
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Issuer does not exist.";
        return tecNO_ISSUER;
    }

    auto const sleRippleState =
        view.read(keylet::trustLine(account, issuer, amount.get<Issue>().currency));
    if (!sleRippleState)
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Trust line does not exist.";
        return tecNO_LINE;
    }

    STAmount const balance = (*sleRippleState)[sfBalance];

    if (balance > beast::kZero && issuer < account)
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Invalid trust line state.";
        return tecNO_PERMISSION;
    }
    if (balance < beast::kZero && issuer > account)
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Invalid trust line state.";
        return tecNO_PERMISSION;
    }

    if (auto const ter = requireAuth(view, amount.get<Issue>(), account);
        !isTesSuccess(ter))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Account not authorized.";
        return ter;
    }

    if (auto const ter = requireAuth(view, amount.get<Issue>(), dest);
        !isTesSuccess(ter))
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Destination not authorized.";
        return ter;
    }

    if (isFrozen(view, account, amount.get<Issue>()) ||
        isDeepFrozen(
            view,
            account,
            amount.get<Issue>().currency,
            amount.get<Issue>().account))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Account is frozen.";
        return tecFROZEN;
    }

    if (isFrozen(view, dest, amount.get<Issue>()) ||
        isDeepFrozen(
            view,
            dest,
            amount.get<Issue>().currency,
            amount.get<Issue>().account))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Destination is frozen.";
        return tecFROZEN;
    }

    STAmount const spendableAmount = accountHolds(
        view,
        account,
        amount.get<Issue>().currency,
        issuer,
        FreezeHandling::IgnoreFreeze,
        j);

    if (spendableAmount <= beast::kZero)
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Spendable amount is zero or negative.";
        return tecINSUFFICIENT_FUNDS;
    }

    if (spendableAmount < amount)
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Insufficient spendable balance.";
        return tecINSUFFICIENT_FUNDS;
    }

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

    auto const issuanceKey =
        keylet::mptokenIssuance(amount.get<MPTIssue>().getMptID());
    auto const sleIssuance = view.read(issuanceKey);
    if (!sleIssuance)
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: MPT issuance does not exist.";
        return tecOBJECT_NOT_FOUND;
    }

    if (sleIssuance->getAccountID(sfIssuer) != issuer)
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: MPT issuer mismatch.";
        return tecNO_PERMISSION;
    }

    if (!view.exists(keylet::mptoken(issuanceKey.key, account)))
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Account does not hold MPT.";
        return tecOBJECT_NOT_FOUND;
    }

    auto const& mptIssue = amount.get<MPTIssue>();

    if (auto const ter =
            requireAuth(view, mptIssue, account, AuthType::WeakAuth);
        !isTesSuccess(ter))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Account not authorized.";
        return ter;
    }

    if (auto const ter = requireAuth(view, mptIssue, dest, AuthType::WeakAuth);
        !isTesSuccess(ter))
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Destination not authorized.";
        return ter;
    }

    if (isFrozen(view, account, mptIssue))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Account is locked.";
        return tecLOCKED;
    }

    if (isFrozen(view, dest, mptIssue))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: Destination is locked.";
        return tecLOCKED;
    }

    if (auto const ter = canTransfer(view, mptIssue, account, dest);
        !isTesSuccess(ter))
    {
        JLOG(j.trace()) << "canTransferTokenHelper: MPT cannot be transferred.";
        return ter;
    }

    STAmount const spendableAmount = accountHolds(
        view,
        account,
        amount.get<MPTIssue>(),
        FreezeHandling::IgnoreFreeze,
        AuthHandling::IgnoreAuth,
        j);

    if (spendableAmount <= beast::kZero)
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Spendable amount is zero or negative.";
        return tecINSUFFICIENT_FUNDS;
    }

    if (spendableAmount < amount)
    {
        JLOG(j.trace())
            << "canTransferTokenHelper: Insufficient spendable balance.";
        return tecINSUFFICIENT_FUNDS;
    }

    if (!canAdd(spendableAmount, amount))
        return tecPRECISION_LOSS;

    return tesSUCCESS;
}

}  // namespace xrpl
