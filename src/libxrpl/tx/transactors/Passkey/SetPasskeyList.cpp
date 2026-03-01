#include <xrpl/basics/Log.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/tx/transactors/Passkey/SetPasskeyList.h>

#include <algorithm>
#include <cstdint>

namespace xrpl {

NotTEC
SetPasskeyList::preflight(PreflightContext const& ctx)
{
    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.debug()) << "SetPasskeyList: invalid flags.";
        return temINVALID_FLAG;
    }

    auto const& passkeys = ctx.tx.getFieldArray(sfPasskeys);

    // Limit number of passkeys
    if (passkeys.size() > 10)
    {
        JLOG(ctx.j.debug()) << "SetPasskeyList: too many passkeys.";
        return temMALFORMED;
    }

    // Validate each passkey entry
    for (auto const& passkey : passkeys)
    {
        if (!passkey.isFieldPresent(sfPasskeyID) ||
            !passkey.isFieldPresent(sfPublicKey))
        {
            JLOG(ctx.j.debug())
                << "SetPasskeyList: passkey missing required fields.";
            return temMALFORMED;
        }

        // Validate public key is a valid P256 key
        auto const pk = passkey.getFieldVL(sfPublicKey);
        auto const keyType = publicKeyType(makeSlice(pk));
        if (!keyType || *keyType != KeyType::p256)
        {
            JLOG(ctx.j.debug())
                << "SetPasskeyList: passkey public key must be P256.";
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

TER
SetPasskeyList::doApply()
{
    auto const sleAccount =
        ctx_.view().peek(keylet::account(account_));
    if (!sleAccount)
        return tecINTERNAL;

    auto const passkeyListKeylet = keylet::passkeyList(account_);
    auto const& passkeys = ctx_.tx.getFieldArray(sfPasskeys);

    // If passkeys array is empty, delete the existing PasskeyList
    if (passkeys.empty())
    {
        auto const slePasskeyList =
            ctx_.view().peek(passkeyListKeylet);
        if (!slePasskeyList)
            return tecNO_ENTRY;

        if (!ctx_.view().dirRemove(
                keylet::ownerDir(account_),
                (*slePasskeyList)[sfOwnerNode],
                slePasskeyList->key(),
                true))
        {
            JLOG(ctx_.journal.fatal())
                << "Unable to delete PasskeyList from owner.";
            return tefBAD_LEDGER;
        }

        adjustOwnerCount(ctx_.view(), sleAccount, -1, ctx_.journal);
        ctx_.view().update(sleAccount);
        ctx_.view().erase(slePasskeyList);
        return tesSUCCESS;
    }

    // Check if PasskeyList already exists (update case)
    if (auto const sleExisting = ctx_.view().peek(passkeyListKeylet))
    {
        sleExisting->setFieldArray(sfPasskeys, passkeys);
        sleExisting->setFieldH256(
            sfPreviousTxnID, ctx_.tx.getTransactionID());
        sleExisting->setFieldU32(
            sfPreviousTxnLgrSeq, ctx_.view().seq());
        ctx_.view().update(sleExisting);
        return tesSUCCESS;
    }

    // Create new PasskeyList
    {
        auto const balance = STAmount((*sleAccount)[sfBalance]).xrp();
        auto const reserve = ctx_.view().fees().accountReserve(
            (*sleAccount)[sfOwnerCount] + 1);
        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    auto sle = std::make_shared<SLE>(passkeyListKeylet);
    (*sle)[sfOwner] = account_;
    sle->setFieldArray(sfPasskeys, passkeys);
    (*sle)[sfPreviousTxnID] = ctx_.tx.getTransactionID();
    (*sle)[sfPreviousTxnLgrSeq] = ctx_.view().seq();

    auto page = ctx_.view().dirInsert(
        keylet::ownerDir(account_),
        sle->key(),
        describeOwnerDir(account_));
    if (!page)
        return tecDIR_FULL;

    (*sle)[sfOwnerNode] = *page;

    adjustOwnerCount(ctx_.view(), sleAccount, 1, ctx_.journal);
    ctx_.view().update(sleAccount);
    ctx_.view().insert(sle);

    return tesSUCCESS;
}

}  // namespace xrpl
