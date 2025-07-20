#include <xrpld/app/tx/detail/SetQuantumKey.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Indexes.h>

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

    // TODO: check account reserve

    auto const account = ctx_.tx.getAccountID(sfAccount);
    auto const quantumPublicKey = ctx_.tx.getFieldVL(sfQuantumPublicKey);
    auto const quantumKeylet = keylet::quantum(account, makeSlice(quantumPublicKey));
    auto quantumKey = std::make_shared<SLE>(quantumKeylet);
    quantumKey->setFieldVL(sfQuantumPublicKey, quantumPublicKey);
    ctx_.view().insert(quantumKey);

    // TODO: add to owner directory


    return tesSUCCESS;
}

} // namespace ripple