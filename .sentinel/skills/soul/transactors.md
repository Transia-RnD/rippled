# Transactors

Transaction processing pipeline: preflight (static validation) -> preclaim (ledger state checks) -> doApply (state mutation). Base class `Transactor` in `src/libxrpl/tx/`.

## Key Invariants

- Pipeline is strict: preflight runs WITHOUT ledger state, preclaim runs WITH read-only view, doApply runs with mutable view
- `preflight` validates all fields exist and are well-formed; this is the ONLY place to reject malformed transactions cheaply
- Fee is always deducted even if the transaction fails (`tecCLAIM` pattern); `payFee` runs before `doApply`
- Sequence/ticket consumption happens in `consumeSeqProxy`; must succeed before any state changes
- Invariant checkers run after `doApply`; they can veto the transaction post-execution

## Common Bug Patterns

- New transaction type missing preflight validation for new fields = malformed transactions reach doApply and corrupt state
- Forgetting to handle `tecCLAIM` in doApply: fee is deducted but no other state changes should occur
- Batch transactions (`Batch` type) have their own signing path (`checkBatchSign`); changes to signing must cover both paths
- `calculateBaseFee` override without updating `minimumFee` causes fee calculation divergence between nodes
- Missing invariant checker update for new ledger entry types = silent constraint violations

## Transactor Template

### Header (`include/xrpl/tx/transactors/MyTx.h`)
```cpp
#pragma once
#include <xrpl/tx/Transactor.h>

namespace xrpl {
class MyTransaction : public Transactor {
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};
    explicit MyTransaction(ApplyContext& ctx) : Transactor(ctx) {}

    static bool checkExtraFeatures(PreflightContext const& ctx);
    static std::uint32_t getFlagsMask(PreflightContext const& ctx);
    static NotTEC preflight(PreflightContext const& ctx);  // NO ledger
    static TER preclaim(PreclaimContext const& ctx);        // read-only
    TER doApply() override;                                 // read-write
};
}
```

### Implementation (`src/libxrpl/tx/transactors/MyFeature/MyTx.cpp`)
```cpp
bool MyTransaction::checkExtraFeatures(PreflightContext const& ctx)
{   // REQUIRED: gate on amendment
    return ctx.rules.enabled(featureMyFeature);
}

NotTEC MyTransaction::preflight(PreflightContext const& ctx)
{   // Static validation — NO ctx.view, NO ledger access
    if (ctx.tx[sfAmount] <= beast::zero)
        return temBAD_AMOUNT;
    return tesSUCCESS;
}

TER MyTransaction::preclaim(PreclaimContext const& ctx)
{   // Read-only — ctx.view.read() only, NO peek/insert/erase
    if (!ctx.view.exists(keylet::account(ctx.tx[sfAccount])))
        return terNO_ACCOUNT;
    return tesSUCCESS;
}

TER MyTransaction::doApply()
{   // Mutable — view().peek(), view().insert(), view().update(), view().erase()
    auto sle = view().peek(keylet::account(account_));
    sle->setFieldAmount(sfBalance, newBal);
    view().update(sle);  // REQUIRED after mutation
    return tesSUCCESS;
}
```

### Registration Checklist
```cpp
// ALL of these are REQUIRED for a new transaction type:
// 1. transactions.macro: TRANSACTION(ttMY_TYPE, N, MyTx, delegation, fields)
// 2. applySteps.cpp:     case ttMY_TYPE: return invoke<MyTransaction>(...);
// 3. features.macro:     XRPL_FEATURE(MyFeature, Supported::yes, DefaultNo)
// 4. Feature.h:          increment numFeatures
// 5. InvariantCheck.cpp:  update if new ledger objects created
// 6. Batch.cpp:          add to disabledTxTypes if not batch-compatible
```

## Transaction Lifecycle

1. `preflight` (static checks, no ledger) -> `PreflightResult`
2. `preclaim` (ledger state, read-only) -> TER
3. `operator()` orchestrates: `checkSeqProxy` -> `checkPriorTxAndLastLedger` -> `checkFee` -> `checkSign` -> `apply`
4. `apply` -> `reset` (deduct fee) -> `doApply` (state changes) -> invariant checks -> metadata generation

## Permission System

- `checkSign` dispatches to `checkSingleSign`, `checkMultiSign`, or `checkBatchSign`
- `checkPermission` validates delegated authority for delegatable transaction types
- Multi-sign requires M-of-N signers matching the signer list; weight threshold must be met

## Key Files

- `src/xrpld/app/tx/detail/Transactor.cpp` - base class and pipeline
- `include/xrpl/protocol/detail/transactions.macro` - type definitions
- `src/xrpld/app/tx/detail/` - per-type implementations (Payment.cpp, OfferCreate.cpp, etc.)
- `src/xrpld/app/tx/detail/InvariantCheck.cpp` - post-execution invariant checks
