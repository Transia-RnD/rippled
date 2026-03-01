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

#include <xrpl/tx/transactors/Option/MarginAccountSet.h>
#include <xrpl/ledger/Sandbox.h>
#include <xrpl/ledger/View.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/TxFlags.h>

namespace xrpl {

NotTEC
MarginAccountSet::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "MarginAccountSet: invalid flags.";
        return temINVALID_FLAG;
    }

    // Validate margin mode: 0 = isolated, 1 = cross
    std::uint32_t const marginMode = ctx.tx[sfMarginMode];
    if (marginMode > 1)
    {
        JLOG(ctx.j.debug())
            << "MarginAccountSet: marginMode must be 0 (isolated) or 1 (cross).";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

TER
MarginAccountSet::preclaim(PreclaimContext const& ctx)
{
    auto const accountID = ctx.tx[sfAccount];
    Asset const collateralAsset = ctx.tx[sfCollateralAsset].get<Issue>();

    auto const marginAcctKeylet =
        keylet::marginAccount(accountID, collateralAsset);
    auto const existingAccount = ctx.view.read(marginAcctKeylet);

    if (existingAccount)
    {
        // If modifying existing account, verify no open positions
        // (cannot switch margin mode with open positions)
        // TODO: In Phase 6, add position count check here
        JLOG(ctx.j.debug())
            << "MarginAccountSet: margin account already exists, updating.";
    }

    return tesSUCCESS;
}

TER
MarginAccountSet::doApply()
{
    Sandbox sb(&ctx_.view());

    Asset const collateralAsset = ctx_.tx[sfCollateralAsset].get<Issue>();
    Issue const collateralIssue = collateralAsset.get<Issue>();
    std::uint32_t const marginMode = ctx_.tx[sfMarginMode];

    auto const marginAcctKeylet =
        keylet::marginAccount(account_, collateralAsset);

    auto sleMarginAcct = sb.peek(marginAcctKeylet);
    bool const isCreate = !sleMarginAcct;

    if (isCreate)
    {
        // Create new margin account
        sleMarginAcct = std::make_shared<SLE>(marginAcctKeylet);
        sleMarginAcct->setAccountID(sfAccount, account_);
        sleMarginAcct->setFieldIssue(
            sfCollateralAsset, STIssue{sfCollateralAsset, collateralIssue});

        // Add to owner directory
        auto const page = sb.dirInsert(
            keylet::ownerDir(account_),
            marginAcctKeylet,
            describeOwnerDir(account_));
        if (!page)
        {
            JLOG(j_.debug())
                << "MarginAccountSet: failed to insert owner dir.";
            return tecDIR_FULL;
        }
        sleMarginAcct->setFieldU64(sfOwnerNode, *page);
    }

    // Set/update margin mode
    sleMarginAcct->setFieldU32(sfMarginMode, marginMode);

    if (isCreate)
    {
        sb.insert(sleMarginAcct);
        adjustOwnerCount(sb, sb.peek(keylet::account(account_)), 1, j_);
    }
    else
    {
        sb.update(sleMarginAcct);
    }

    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace xrpl
