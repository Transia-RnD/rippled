# Merge Conflict Resolution Guide

## Description
Use when resolving merge conflicts after merging `develop` into a feature branch. Covers the key files that commonly conflict, field number allocation, namespace conventions, and the correct resolution strategy for each file type.

## General Principles

### Namespace
- All code uses `namespace xrpl { }` (not `ripple`). If a conflict shows `namespace ripple`, take the `xrpl` version.
- Qualified references: `xrpl::sign(...)`, `xrpl::sha256_hasher`, etc. Never `ripple::`.

### Header Guards
- Always `#pragma once`. Never `#ifndef` include guards. If a conflict shows old-style guards, take the `#pragma once` version.

### Copyright Headers
- The `develop` branch has removed copyright/license comment blocks from source files. If a conflict is just about the copyright header, take `develop`'s version (no header).

### File Moves
- Many files have been moved from `src/xrpld/` to `src/libxrpl/` or from `src/xrpld/` headers to `include/xrpl/`. If git shows a file as "deleted in develop" but it exists at a new path, delete the old-path file (`git rm`) and keep the new-path version.
- Common moves:
  - `src/xrpld/app/tx/detail/*.h` → `include/xrpl/tx/transactors/*.h`
  - `src/xrpld/app/tx/detail/*.cpp` → `src/libxrpl/tx/*.cpp` or `src/libxrpl/tx/transactors/*.cpp`
  - `src/xrpld/app/tx/detail/InvariantCheck.cpp` → `src/libxrpl/tx/invariants/InvariantCheck.cpp`

### Transactor Migration
When merging transactors from `src/xrpld/app/tx/detail/` into `develop`, they must be fully migrated to the new libxrpl structure:

**File locations:**
- Header: `src/xrpld/app/tx/detail/Foo.h` → `include/xrpl/tx/transactors/Foo.h`
- Source: `src/xrpld/app/tx/detail/Foo.cpp` → `src/libxrpl/tx/transactors/Foo.cpp`
- Delete the old `.cpp` (both `src/xrpld/` and `src/libxrpl/` are GLOB_RECURSE discovered — duplicates cause linker errors)
- Optionally leave a forwarding header at the old `.h` location: `#include <xrpl/tx/transactors/Foo.h>`

**Header changes:**
- `#pragma once` (not `#ifndef` guards)
- `namespace xrpl` (not `ripple`)
- `#include <xrpl/tx/Transactor.h>` (not `<xrpld/app/tx/detail/Transactor.h>`)

**Source changes:**
- `namespace xrpl` (not `ripple`)
- `#include <xrpl/tx/transactors/Foo.h>` (not `<xrpld/app/tx/detail/Foo.h>`)
- `#include <xrpl/ledger/ApplyView.h>` (not `<xrpld/ledger/ApplyView.h>`)
- `#include <xrpl/ledger/View.h>` for `describeOwnerDir`

**API changes in the new Transactor:**
- `preflight1()` and `preflight2()` are **private** — transactor `preflight()` must NOT call them. The framework calls them automatically.
- Flag checking (`tfUniversalMask`) is handled by the framework via `getFlagsMask()`. Override `getFlagsMask()` only if custom flag handling is needed; otherwise the default handles `tfUniversalMask`.
- A simple `preflight()` that only did flag checks + preflight1/preflight2 should just `return tesSUCCESS;`
- `ctx_.app.journal(...)` → `ctx_.registry.journal(...)`
- `ctx_.app` does not exist in the new `ApplyContext`; use `ctx_.registry` for service access

**transactions.macro entry:**
Every transactor must have a `#if TRANSACTION_INCLUDE` block:
```cpp
#if TRANSACTION_INCLUDE
#   include <xrpl/tx/transactors/Foo.h>
#endif
TRANSACTION(ttFOO, <number>, Foo, ...)
```

### Include Paths
- `develop` uses the new macro-based transactor include system via `transactions.macro` with `TRANSACTION_INCLUDE`. Old-style explicit `#include <xrpld/app/tx/detail/*.h>` lists should be replaced with the macro approach.

## File-Specific Resolution Rules

### `include/xrpl/protocol/detail/features.macro`

New feature amendments go **at the top** of the active list (below the macro guard checks and `// clang-format off`), in reverse chronological order.

```
// Add new amendments to the top of this list.
// Keep it sorted in reverse chronological order.

XRPL_FEATURE(MyNewFeature,              Supported::yes, VoteBehavior::DefaultNo)
XRPL_FIX    (ExistingFix,               Supported::yes, VoteBehavior::DefaultNo)
...
```

Resolution: Keep both sides' new amendments. Place your feature branch's new amendments at the very top.

### `include/xrpl/protocol/detail/sfields.macro`

Fields are grouped by type (UINT32, UINT64, UINT128, etc.) with common and uncommon sections. Each field has a unique **type + field number** pair.

**Resolution strategy:**
1. Keep both sides' new fields.
2. Check for field number collisions within each type. Use the next available number for your feature's fields.
3. Common fields come first, uncommon fields after (there's a comment separator).

To find the next available field number for a type:
```bash
grep "TYPED_SFIELD.*UINT32" include/xrpl/protocol/detail/sfields.macro | sed 's/.*UINT32, *//;s/).*//' | sort -n
```

Similarly for OBJECT and ARRAY types (using `UNTYPED_SFIELD`):
```bash
grep "UNTYPED_SFIELD.*OBJECT" include/xrpl/protocol/detail/sfields.macro | sed 's/.*OBJECT, *//;s/).*//' | sort -n
grep "UNTYPED_SFIELD.*ARRAY" include/xrpl/protocol/detail/sfields.macro | sed 's/.*ARRAY, *//;s/).*//' | sort -n
```

### `include/xrpl/protocol/detail/transactions.macro`

Transaction types have a unique **transaction type number**. New feature transactions go **at the bottom** of the active transaction list (before the system transactions starting at 100+).

**Resolution strategy:**
1. Keep all of `develop`'s transactions.
2. Add your feature's transactions at the bottom with the next available number.
3. Use the new 7-argument `TRANSACTION` macro format:

```cpp
/** Description of the transaction */
#if TRANSACTION_INCLUDE
#   include <xrpl/tx/transactors/MyTransactor.h>
#endif
TRANSACTION(ttMY_TX, <next_number>, MyTx,
    Delegation::delegable,
    featureMyFeature,
    noPriv, ({
    {sfSomeField, soeREQUIRED},
}))
```

To find the next available transaction number:
```bash
grep "^TRANSACTION(" include/xrpl/protocol/detail/transactions.macro | sed 's/.*,\s*\([0-9]*\),.*/\1/' | sort -n
```

Note: Transaction numbers 100+ are reserved for system transactions (amendments, fees, UNL).

### `include/xrpl/protocol/detail/ledger_entries.macro`

Ledger entry types have a unique **ledger type number** (hex). New entries go **at the bottom** of the active list.

**Resolution strategy:**
1. Keep all of `develop`'s entries.
2. Add your feature's entries at the bottom with the next available hex number.
3. Check for collisions:

```bash
grep "^LEDGER_ENTRY(" include/xrpl/protocol/detail/ledger_entries.macro | grep -o '0x[0-9a-fA-F]*'  | sort
```

### `src/libxrpl/protocol/Indexes.cpp`

The `LedgerNameSpace` enum assigns a unique single character to each ledger entry type for index hashing.

**Resolution strategy:**
1. Keep both sides' entries.
2. Check for character collisions. Each entry needs a unique char.
3. Find used characters:

```bash
grep -E "^\s+[A-Z_]+ = " src/libxrpl/protocol/Indexes.cpp | sed "s/.*= '//;s/'.*//" | sort | tr -d '\n'
```

Also check the deprecated chars (reserved, cannot reuse):
```bash
grep "deprecated" src/libxrpl/protocol/Indexes.cpp | sed "s/.*= '//;s/'.*//"
```

### `src/libxrpl/protocol/InnerObjectFormats.cpp`

Inner object formats define the fields allowed inside array elements (e.g., `sfSigners`, `sfPasskeys`).

**Resolution:** Keep both sides' `add(...)` calls. No numbering conflicts here — just ensure no duplicate registrations.

### `src/libxrpl/protocol/STTx.cpp`

The `singleSignHelper` function was refactored in `develop`:
- Parameter name: `sigObject` (not `signer`)
- Signature retrieval: `sigObject.getFieldVL(sfTxnSignature)` (not `getSignature(signer)`)
- No `fullyCanonical` parameter — canonical sig checking was simplified

**Resolution:** Take `develop`'s function signatures and patterns. Adapt any feature-specific logic to match.

### `src/libxrpl/protocol/SecretKey.cpp`

**Resolution:** Use `namespace xrpl`. Keep any additional `#include` directives your feature needs (e.g., OpenSSL headers for new key types).

### `src/libxrpl/tx/Transactor.cpp`

The `checkSign` / `checkSingleSign` / `checkMultiSign` functions were refactored in `develop`:
- `checkSingleSign` no longer takes a `Rules` parameter
- `checkSign` has a new overload taking `PreclaimContext`
- `checkBatchSign` uses the simplified `checkSingleSign` call

**Resolution:** Take `develop`'s function signatures. Add any new authentication checks (e.g., passkey verification) into `checkSingleSign` before the final `return tefBAD_AUTH`, using `view.read(...)` to check ledger state.

### `src/libxrpl/tx/applySteps.cpp`

**Resolution:** Always take `develop`'s macro-based include approach:
```cpp
#include <xrpl/tx/applySteps.h>
#pragma push_macro("TRANSACTION")
#undef TRANSACTION

#define TRANSACTION(...)
#define TRANSACTION_INCLUDE 1

#include <xrpl/protocol/detail/transactions.macro>

#undef TRANSACTION
#pragma pop_macro("TRANSACTION")
```

Never use the old explicit `#include <xrpld/app/tx/detail/*.h>` list.

### `src/test/jtx/impl/utility.cpp`

The `sign` function was refactored in `develop`:
- Takes `sigObject` parameter: `sign(Json::Value& jv, Account const& account, Json::Value& sigObject)`
- Has an overload: `sign(Json::Value& jv, Account const& account)` that calls `sign(jv, account, jv)`
- Uses `xrpl::sign(...)` not `ripple::sign(...)`

**Resolution:** Take `develop`'s function signatures. Adapt feature-specific signing logic to the new pattern.

## Conflict Resolution Checklist

1. List all conflicted files: `git diff --name-only --diff-filter=U`
2. Check which have actual conflict markers vs just unmerged: `grep -l "<<<<<<< HEAD" <file>`
3. For files without markers: stage them or `git rm` if they were moved
4. For each file with markers:
   - Take `develop`'s structural changes (namespace, function signatures, macro formats)
   - Preserve your feature's additions (new fields, entries, transactions, logic)
   - Resolve any numbering collisions by incrementing to the next available number
5. Stage resolved files: `git add <file>`
6. Verify no remaining markers: `grep -r "<<<<<<< " --include="*.cpp" --include="*.h" --include="*.macro"`
7. Verify no remaining unmerged: `git diff --name-only --diff-filter=U`
