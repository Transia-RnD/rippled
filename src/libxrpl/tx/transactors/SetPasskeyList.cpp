#include <xrpl/basics/Log.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/tx/transactors/SetPasskeyList.h>

#include <algorithm>
#include <cstdint>

namespace xrpl {

NotTEC
SetPasskeyList::preflight(PreflightContext const& ctx)
{
    return tesSUCCESS;
}

TER
SetPasskeyList::doApply()
{
    auto viewJ = ctx_.registry.journal("View");
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

}  // namespace xrpl
