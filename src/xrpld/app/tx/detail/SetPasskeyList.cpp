//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/tx/detail/SetPasskeyList.h>
#include <xrpld/ledger/ApplyView.h>

#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxFlags.h>

#include <algorithm>
#include <cstdint>

namespace ripple {

NotTEC
SetPasskeyList::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "SetPasskeyList: invalid flags.";
        return temINVALID_FLAG;
    }

    return preflight2(ctx);
}

TER
SetPasskeyList::doApply()
{
    auto viewJ = ctx_.app.journal("View");
    auto const sleAccount = ctx_.view().peek(keylet::account(account_));
    if (!sleAccount)
        return tecINTERNAL;
    
    auto const passkeyID = keylet::passkeyList(account_);
    auto sle = std::make_shared<SLE>(passkeyID);
    sle->setAccountID(sfOwner, ctx_.tx.getAccountID(sfAccount));
    auto const& passkeys = ctx_.tx.getFieldArray(sfPasskeys);
    sle->setFieldArray(sfPasskeys, passkeys);

    auto page = ctx_.view().dirInsert(
        keylet::ownerDir(account_), sle->key(), describeOwnerDir(account_));
    if (!page)
        return tecDIR_FULL;  // LCOV_EXCL_LINE

    (*sle)[sfOwnerNode] = *page;

    adjustOwnerCount(ctx_.view(), sleAccount, 1, viewJ);

    ctx_.view().insert(sle);
    return tesSUCCESS;
}

}  // namespace ripple
