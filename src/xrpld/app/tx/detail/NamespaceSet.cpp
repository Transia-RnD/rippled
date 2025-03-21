//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Nerd Nest XYZ.

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

#include <xrpld/app/tx/detail/NamespaceSet.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/View.h>

namespace ripple {

TxConsequences
NamespaceSet::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, beast::zero};
}

NotTEC
NamespaceSet::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    // Validate: UserToken
    if (ctx.tx.getFieldVL(sfUserName).size() > maxDomainLength)
    {
        return temMALFORMED;
    }

    // Validate: DisplayName
    if (ctx.tx.isFieldPresent(sfDisplayName) &&
        ctx.tx.getFieldVL(sfDisplayName).size() > maxDomainLength)
    {
        return temMALFORMED;
    }

    return preflight2(ctx);
}

TER
NamespaceSet::preclaim(PreclaimContext const& ctx)
{
    AccountID const account = ctx.tx.getAccountID(sfAccount);
    // Validate Duplicate Username
    auto const sle =
        ctx.view.read(keylet::namespace_(ctx.tx.getFieldVL(sfUserName)));
    if (sle && sle->getAccountID(sfOwner) != account)
    {
        return tecNO_PERMISSION;
    }

    auto const sleAccount = ctx.view.read(keylet::account(account));
    if (!sleAccount)
        return terNO_ACCOUNT;

    return tesSUCCESS;
}

TER
NamespaceSet::doApply()
{
    auto const sleAccount = ctx_.view().peek(keylet::account(account_));
    if (!sleAccount)
        return tefINTERNAL;

    if (sleAccount->isFieldPresent(sfUserName))
    {
        // Update
        auto const sle = ctx_.view().peek(
            keylet::namespace_(ctx_.tx.getFieldVL(sfUserName)));
        sleAccount->setFieldVL(sfUserName, ctx_.tx.getFieldVL(sfUserName));
        sle->setFieldVL(sfUserName, ctx_.tx.getFieldVL(sfUserName));
        if (ctx_.tx.isFieldPresent(sfDisplayName))
            sle->setFieldVL(sfDisplayName, ctx_.tx.getFieldVL(sfDisplayName));

        ctx_.view().update(sle);
        return tesSUCCESS;
    }

    // Create
    {
        auto const sle = std::make_shared<SLE>(
            keylet::namespace_(ctx_.tx.getFieldVL(sfUserName)));
        sleAccount->setFieldVL(sfUserName, ctx_.tx.getFieldVL(sfUserName));
        sle->setFieldVL(sfUserName, ctx_.tx.getFieldVL(sfUserName));
        if (ctx_.tx.isFieldPresent(sfDisplayName))
            sle->setFieldVL(sfDisplayName, ctx_.tx.getFieldVL(sfDisplayName));
        sle->setAccountID(sfOwner, account_);

        // Check reserve availability for new object creation
        {
            auto const reserve = ctx_.view().fees().accountReserve(1);
            auto const& balance = sleAccount->getFieldAmount(sfBalance);
            if (balance < reserve)
                return tecINSUFFICIENT_RESERVE;
        }

        // Add ledger object to owner's page
        {
            auto page = ctx_.view().dirInsert(
                keylet::ownerDir(account_),
                sle->key(),
                describeOwnerDir(account_));
            if (!page)
                return tecDIR_FULL;
            (*sle)[sfOwnerNode] = *page;
        }
        adjustOwnerCount(ctx_.view(), sleAccount, 1, ctx_.journal);
        ctx_.view().insert(sle);
    }
    return tesSUCCESS;
}

}  // namespace ripple
