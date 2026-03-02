#include <xrpl/basics/Log.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/tx/transactors/SetPasskeyList.h>

#include <algorithm>
#include <cstdint>
#include <set>

namespace xrpl {

NotTEC
SetPasskeyList::preflight(PreflightContext const& ctx)
{
    auto const& passkeys = ctx.tx.getFieldArray(sfPasskeys);

    if (passkeys.empty())
    {
        JLOG(ctx.j.debug()) << "SetPasskeyList: empty passkeys array.";
        return temMALFORMED;
    }

    // Validate each passkey entry and check for duplicates
    std::set<Blob> seenPasskeyIDs;
    std::set<Blob> seenPublicKeys;
    for (auto const& passkey : passkeys)
    {
        if (!passkey.isFieldPresent(sfPasskeyID) ||
            !passkey.isFieldPresent(sfPublicKey))
        {
            JLOG(ctx.j.debug())
                << "SetPasskeyList: missing required fields.";
            return temMALFORMED;
        }

        // Check for duplicate PasskeyIDs
        auto const passkeyID = passkey.getFieldVL(sfPasskeyID);
        if (!seenPasskeyIDs.insert(passkeyID).second)
        {
            JLOG(ctx.j.debug())
                << "SetPasskeyList: duplicate PasskeyID.";
            return temMALFORMED;
        }

        // Check for duplicate PublicKeys
        auto const pk = passkey.getFieldVL(sfPublicKey);
        if (!seenPublicKeys.insert(pk).second)
        {
            JLOG(ctx.j.debug())
                << "SetPasskeyList: duplicate PublicKey.";
            return temMALFORMED;
        }

        // Validate public key is a valid P256 key
        auto const keyType = publicKeyType(makeSlice(pk));
        if (!keyType || *keyType != KeyType::p256)
        {
            JLOG(ctx.j.debug())
                << "SetPasskeyList: invalid P256 public key.";
            return temMALFORMED;
        }
    }

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
