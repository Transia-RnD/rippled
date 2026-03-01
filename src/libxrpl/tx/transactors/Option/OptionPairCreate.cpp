//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/ledger/OrderBookDB.h>
#include <xrpl/tx/transactors/AMM/AMMHelpers.h>
#include <xrpl/tx/transactors/Option/OptionPairCreate.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
OptionPairCreate::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "OptionPairCreate: invalid flags.";
        return temINVALID_FLAG;
    }

    Issue const issue = ctx.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx.tx[sfAsset2].get<Issue>();

    if (issue == issue2)
    {
        JLOG(ctx.j.error()) << "OptionPairCreate: tokens can not have the same "
                               "currency/issuer.";
        return temMALFORMED;
    }

    if (auto const err = invalidAMMAsset(issue))
    {
        JLOG(ctx.j.debug()) << "OptionPairCreate: invalid asset1.";
        return err;
    }

    if (auto const err = invalidAMMAsset(issue2))
    {
        JLOG(ctx.j.debug()) << "OptionPairCreate: invalid asset2.";
        return err;
    }

    return tesSUCCESS;
}

XRPAmount
OptionPairCreate::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // The fee required for OptionPairCreate is one owner reserve.
    return view.fees().increment;
}

TER
OptionPairCreate::preclaim(PreclaimContext const& ctx)
{
    auto const accountID = ctx.tx[sfAccount];
    Issue const issue = ctx.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx.tx[sfAsset2].get<Issue>();

    // Check if OptionPair already exists
    if (auto const optionPairKeylet = keylet::optionPair(issue, issue2);
        ctx.view.read(optionPairKeylet))
    {
        JLOG(ctx.j.debug())
            << "OptionPairCreate: ltOPTION_PAIR already exists.";
        return tecDUPLICATE;
    }

    if (auto const ter = requireAuth(ctx.view, issue, accountID);
        ter != tesSUCCESS)
    {
        JLOG(ctx.j.debug())
            << "OptionPairCreate: account is not authorized, " << issue;
        return ter;
    }

    if (auto const ter = requireAuth(ctx.view, issue2, accountID);
        ter != tesSUCCESS)
    {
        JLOG(ctx.j.debug())
            << "OptionPairCreate: account is not authorized, " << issue2;
        return ter;
    }

    // Globally or individually frozen
    if (isFrozen(ctx.view, accountID, issue) ||
        isFrozen(ctx.view, accountID, issue2))
    {
        JLOG(ctx.j.debug()) << "OptionPairCreate: involves frozen asset.";
        return tecFROZEN;
    }

    auto noDefaultRipple = [](ReadView const& view, Issue const& issue) {
        if (isXRP(issue))
            return false;

        if (auto const issuerAccount =
                view.read(keylet::account(issue.account)))
            return (issuerAccount->getFlags() & lsfDefaultRipple) == 0;

        return false;
    };

    if (noDefaultRipple(ctx.view, issue) || noDefaultRipple(ctx.view, issue2))
    {
        JLOG(ctx.j.debug()) << "OptionPairCreate: DefaultRipple not set";
        return terNO_RIPPLE;
    }

    return tesSUCCESS;
}

static TER
applyCreate(
    ApplyContext& ctx_,
    Sandbox& sb,
    AccountID const& account_,
    beast::Journal j_)
{
    Issue const issue = ctx_.tx[sfAsset].get<Issue>();
    Issue const issue2 = ctx_.tx[sfAsset2].get<Issue>();

    auto const optionPairKeylet = keylet::optionPair(issue, issue2);

    // Create pseudo-account for the OptionPair
    auto const maybeAccount =
        createPseudoAccount(sb, optionPairKeylet.key, sfOptionPairID);
    if (!maybeAccount)
    {
        JLOG(j_.error()) << "OptionPairCreate: failed to create pseudo account.";
        return maybeAccount.error();
    }
    auto const account = (*maybeAccount)->getAccountID(sfAccount);

    // Create ltOPTION_PAIR object.
    auto pairSle = std::make_shared<SLE>(optionPairKeylet);
    pairSle->setAccountID(sfAccount, account);
    auto const& [_issue1, _issue2] = std::minmax(issue, issue2);
    pairSle->setFieldIssue(sfAsset, STIssue{sfAsset, _issue1});
    pairSle->setFieldIssue(sfAsset2, STIssue{sfAsset2, _issue2});

    // Set trading fee if provided (in 1/10 basis points)
    // Maximum: 1000000 = 100% (1000000 / 1000000)
    if (ctx_.tx.isFieldPresent(sfTradingFeeBps))
    {
        std::uint32_t const feeBps = ctx_.tx[sfTradingFeeBps];
        if (feeBps > 1000000)
        {
            JLOG(j_.debug()) << "OptionPairCreate: TradingFeeBps too large";
            return temMALFORMED;
        }
        pairSle->setFieldU32(sfTradingFeeBps, feeBps);
    }

    // Add owner directory to link the root account and OptionPair object.
    if (auto const page = sb.dirInsert(
            keylet::ownerDir(account),
            pairSle->key(),
            describeOwnerDir(account)))
    {
        pairSle->setFieldU64(sfOwnerNode, *page);
    }
    else
    {
        JLOG(j_.debug()) << "OptionPairCreate: failed to insert owner dir";
        return tecDIR_FULL;
    }
    sb.insert(pairSle);
    adjustOwnerCount(sb, sb.peek(keylet::account(account)), 1, j_);

    return tesSUCCESS;
}

TER
OptionPairCreate::doApply()
{
    // This is the ledger view that we work against. Transactions are applied
    // as we go on processing transactions.
    Sandbox sb(&ctx_.view());

    if (auto const res = applyCreate(ctx_, sb, account_, j_);
        !isTesSuccess(res))
    {
        JLOG(j_.error()) << "OptionPairCreate: failed to create OptionPair.";
        return res;
    }

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
