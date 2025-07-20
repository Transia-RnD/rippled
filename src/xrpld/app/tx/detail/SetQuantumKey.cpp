#include <xrpld/app/tx/detail/SetQuantumKey.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpld/ledger/Dir.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/app/ledger/Ledger.h>

extern "C" {
#include "api.h"
}

#ifndef DILITHIUM_PK_SIZE
#define DILITHIUM_PK_SIZE pqcrystals_dilithium2_PUBLICKEYBYTES 
#endif

namespace ripple {

NotTEC
SetQuantumKey::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureQuantum))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    // TODO: Validate quantum public key format and size
    // Hint: Check sfQuantumPublicKey field exists and is valid Dilithium key
    if (!ctx.tx.isFieldPresent(sfQuantumPublicKey))
        return temMALFORMED;

    if (ctx.tx.getFieldVL(sfQuantumPublicKey).size() != DILITHIUM_PK_SIZE)
        return temMALFORMED;

    return preflight2(ctx);
}

TER
SetQuantumKey::preclaim(PreclaimContext const& ctx)
{
    // TODO: Check if quantum key already exists for this account
    // Hint: Use keylet::quantum() to check if ledger entry exists
    auto const sle = ctx.view.read(keylet::quantum(ctx.tx.getAccountID(sfAccount), 
        makeSlice(ctx.tx.getFieldVL(sfQuantumPublicKey))));
    if (sle)
    {
        JLOG(ctx.j.trace()) << "Quantum key already exists for account "
                            << to_string(ctx.tx.getAccountID(sfAccount));
        return tecDUPLICATE;
    }
    
    return tesSUCCESS;
}

TER
SetQuantumKey::doApply()
{
    // TODO: Create or update the quantum key ledger entry
    // Hint: Use keylet::quantum() to create the entry
    // Set all required fields: sfAccount, sfQuantumPublicKey, etc.

    auto const account = ctx_.tx.getAccountID(sfAccount);
    auto const sleAccount = ctx_.view().peek(keylet::account(account));
    if (!sleAccount)
        return tefINTERNAL;

    // TODO: check account reserve
    {
        auto const balance = STAmount((*sleAccount)[sfBalance]).xrp();
        auto const reserve =
            ctx_.view().fees().accountReserve((*sleAccount)[sfOwnerCount] + 1);

        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;
    }
        
    auto const quantumPublicKey = ctx_.tx.getFieldVL(sfQuantumPublicKey);
    auto const quantumKeylet = keylet::quantum(account, makeSlice(quantumPublicKey));
    auto quantumSle = std::make_shared<SLE>(quantumKeylet);
    quantumSle->setFieldVL(sfQuantumPublicKey, quantumPublicKey);
    ctx_.view().insert(quantumSle);

    // TODO: add to owner directory
    {
        auto const page = ctx_.view().dirInsert(
            keylet::ownerDir(account),
            quantumKeylet,
            describeOwnerDir(account));
        if (!page)
            return tecDIR_FULL;
        (*quantumSle)[sfOwnerNode] = *page;
    }

    // TODO: add to the account's owner directory
    adjustOwnerCount(ctx_.view(), sleAccount, 1, ctx_.journal);


    return tesSUCCESS;
}

} // namespace ripple