# XLS-XXd: XPOP Import/Export (Cross-Chain Value Transfer)

```
Title:       XPOP Import/Export
Type:        Draft
Author:      Richard Holland <https://github.com/RichardAH>,
             Nic Dudfield <https://github.com/sublimator>,
             Denis Angell <https://github.com/dangell7>,
             Wietse Wind <https://github.com/WietseWind>
Revision:    5
Discussion:  https://github.com/XRPLF/XRPL-Standards/discussions/107
```

## Abstract

This specification introduces **Import** and **Export** transactions to enable secure cross-chain value transfer between XRP Ledger mainnet and XRPL Protocol sidechains. The mechanism uses **XPop** (Cross-Proof of Payment) -- a cryptographic proof that a transaction was validated and included in a specific ledger on a remote chain.

The model is **lock-and-mint / burn-and-release**:

- **Import** (mainnet to sidechain): User sends a Payment to a federated multisig vault on mainnet, locking XRP. An XPop proves the payment. The sidechain mints equivalent XRP to the user's sidechain account.
- **Export** (sidechain to mainnet): User submits an Export transaction on the sidechain, burning XRP. UNL validators automatically sign a deterministic multisig Payment from the vault on mainnet, releasing the locked XRP to the user (see Section 12).

No XRPL mainnet amendments are required. The vault is a standard multisig account.

**Amendment**: `featureImportExport`

---

## 1. Overview

```
Mainnet (Network 0)                         Sidechain (Network N)
========================                     ========================

Alice sends Payment of       ─── XPop ───>  Alice submits Import txn
100 XRP to the vault                        with XPop blob proving the
(locks XRP in multisig)                     Payment. Sidechain mints
                                            100 XRP to Alice's account.

Bob receives Payment from    <── Validators  Bob submits Export txn.
the vault on mainnet                        Sidechain burns 100 XRP.
(validators sign multisig)                  Validators sign a multisig
                                            Payment from the vault.
```

Key design principles:
- **Lock-and-mint**: XRP locked in the vault on mainnet equals XRP minted on the sidechain (1:1)
- **Burn-and-release**: XRP burned on the sidechain is released from the vault on mainnet by validator-signed multisig
- **Trustless verification**: Import validates cryptographic proofs and validator quorum (80%+)
- **Replay protection**: Per-account import sequence and per-VL sequence tracking
- **Account bootstrapping**: First import creates a new account with a startup bonus
- **No mainnet changes**: The vault is a standard XRPL multisig account

---

## 2. Ledger Entry Types

### 2.1 ImportVLSeq (`ltIMPORT_VL_SEQ`, 0x0091)

Tracks the highest validator list (VL) sequence seen from each VL publisher. Prevents replay of XPop proofs using stale validator lists.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `PublicKey` | VL | Yes | VL publisher master public key |
| `ImportSequence` | UINT32 | Yes | Highest VL sequence processed |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::importVLSeq(PublicKey)`

### 2.2 ExportRecord (`ltEXPORT_RECORD`, 0x0092)

Records an export transaction. UNL validators use the ExportRecord to construct and collectively sign a deterministic multisig Payment on mainnet (see Section 12). The ExportRecord is a **standalone ledger entry** -- it is NOT placed in the account's owner directory and does NOT increment `OwnerCount` or require object reserve. The XRP burn itself is sufficient anti-spam protection; requiring additional reserve on top of a burn would be redundant.

ExportRecords are inserted into a **global export directory** (`keylet::exportDir()`) rather than per-account owner directories. This allows validators to efficiently iterate all pending exports for signing, and provides a single queryable index for RPC clients. The directory is a standard XRPL directory page structure.

The ExportRecord remains on-ledger permanently until the export is fulfilled. This guarantees that validators can always discover pending exports and re-sign them, even after node restarts, validator rotation, or extended delays. The user's burned XRP represents a permanent obligation that must remain recoverable.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Account` | AccountID | Yes | Exporting account |
| `Owner` | AccountID | Yes | Account owner (same as Account) |
| `Destination` | AccountID | Yes | Recipient on the target chain |
| `Amount` | Amount | Yes | Export amount (XRP) |
| `ExportSequence` | UINT32 | Yes | Per-account export counter |
| `TicketSequence` | UINT32 | Optional | Assigned mainnet ticket (see Section 12.3) |
| `DestinationTag` | UINT32 | Optional | Recipient destination tag |
| `LedgerSequence` | UINT32 | Yes | Ledger in which the Export was validated |
| `ExportDirNode` | UINT64 | Yes | Export directory page reference |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::exportRecord(account, seq)`
**Directory**: `keylet::exportDir()` (global, singleton root)

### 2.3 AccountRoot Extensions

Two new optional fields are added to the AccountRoot ledger entry:

| Field | Type | ID | Description |
|-------|------|----|-------------|
| `sfImportSequence` | UINT32 | 81 | Highest inner-txn sequence imported (replay protection) |
| `sfExportSequence` | UINT32 | 82 | Per-account export counter |

---

## 3. Transaction Types

### 3.1 Import (type 98)

Imports value from XRPL mainnet by presenting an XPop proof of a Payment to the configured vault address.

**Amendment**: `featureImportExport`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Blob` | VL | Yes | Serialized XPop proof (JSON, max 512 KiB) |

**ConsequencesFactory**: Custom (amount calculated from inner transaction's delivered amount)

**Behavior**:

1. **Inner transaction validation**: The inner transaction proven by the XPop must be a `tesSUCCESS` `Payment` to the configured vault address (`[import_vault_address]`).
2. **Mint amount**: The `DeliveredAmount` from the inner transaction's metadata determines the XRP minted on the sidechain (1:1).
3. **Account creation**: If the account does not exist on the sidechain, the first Import creates it with a startup bonus of `ReserveBase + (ReserveIncrement * 5)` (or 2 XRP fallback). No fee is charged for account-creating imports.
4. **Master key**: If the first Import is NOT signed with the account's master key, the `lsfDisableMaster` flag is set.
5. **Replay protection**: `ImportSequence` on the AccountRoot is set to the inner transaction's sequence number.

**Validation Flow**:

```
preflight:
  1. featureImportExport enabled (automatic via invokePreflight)
  2. sfBlob present and <= 512 KiB
  3. XPop JSON syntax check
  4. VL master key extraction and validation
  5. Inner transaction extraction and basic checks
     - No TicketSequence
     - Not pseudo/emitted transaction
     - Inner transaction is a Payment (ttPAYMENT)
     - Result is tesSUCCESS (Payment must succeed for DeliveredAmount)
  6. Account match (inner == outer)
  7. Inner txn from Network 0 (no sfNetworkID)
  8. OperationLimit present (value checked in preclaim)
  9. Signing key match (inner == outer, single or multi-sig)
  10. Inner transaction signature verification
  11. Manifest deserialization and verification (signing key not revoked)
  12. UNL blob decode, validate temporal bounds (static), verify signature
  13. Transaction proof verification (Merkle inclusion)
  14. Ledger hash computation and verification
  15. Validator quorum check (>80%)
  16. DeliveredAmount present in metadata, is positive XRP

preclaim:
  17. OperationLimit matches current chain's NetworkID
  18. UNL blob temporal validity against current time
  19. Inner Payment destination matches configured vault address
  20. ImportSequence replay check
  21. Zero fee for new account creation
  22. VL sequence not already used
  23. VL master key in IMPORT_VL_KEYS config

doApply:
  24. Update ImportVLSeq tracking
  25. Extract DeliveredAmount from metadata
  26. Create/update account with credited XRP
  27. Mint XRP (rawDestroyXRP with negative amount)
```

**Errors**:

| Code | Name | Condition |
|------|------|-----------|
| `temDISABLED` | - | `featureImportExport` not enabled |
| `temMALFORMED` | - | Blob missing/too large, invalid JSON, not a Payment, not tesSUCCESS, missing/bad DeliveredAmount, bad proof, failed quorum, revoked manifest |
| `temBAD_FEE` | - | Non-zero fee for account creation |
| `tefINTERNAL` | - | XPop parsing or consistency failures, no vault address configured |
| `tefPAST_IMPORT_SEQ` | - | Inner txn sequence <= account's ImportSequence |
| `tefPAST_IMPORT_VL_SEQ` | - | VL sequence already used |
| `telWRONG_NETWORK` | - | OperationLimit doesn't match current NetworkID |
| `telIMPORT_VL_KEY_NOT_RECOGNISED` | - | VL master key not in IMPORT_VL_KEYS config |
| `tecNO_DST` | - | Inner Payment destination is not the configured vault address |

### 3.2 Export (type 99)

Exports XRP from the sidechain by burning XRP and creating an on-chain record. UNL validators automatically construct and sign a deterministic multisig Payment from the vault on mainnet to release the locked XRP to the specified destination (see Section 12).

**Amendment**: `featureImportExport`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Destination` | AccountID | Yes | Recipient address on mainnet |
| `Amount` | Amount | Yes | Positive XRP amount to export |
| `DestinationTag` | UINT32 | Optional | Destination tag for the recipient |

**ConsequencesFactory**: Blocker (standard fee calculation)

**Behavior**:

1. Debits the export amount from the account balance
2. Increments `ExportSequence` on the AccountRoot
3. Reads the `ExportVaultState` singleton to assign a mainnet ticket
4. Creates a standalone `ExportRecord` ledger entry
5. Inserts the ExportRecord into the global export directory (`keylet::exportDir()`)
6. Increments `NextTicketSeq` in the VaultState
7. Burns the exported XRP from the ledger supply (`rawDestroyXRP` with positive amount)

The ExportRecord is inserted into the global export directory (not the account's owner directory) and does not require object reserve. The XRP burn is the cost -- no additional reserve is needed.

UNL validators observe `ExportRecord` entries and collectively sign corresponding multisig Payments from the vault on mainnet (see Section 12).

**Balance Requirement**:

```
Balance >= Amount + Fee + AccountReserve(OwnerCount)
```

**Errors**:

| Code | Name | Condition |
|------|------|-----------|
| `temDISABLED` | - | `featureImportExport` not enabled |
| `temBAD_AMOUNT` | - | Amount is not positive XRP |
| `terNO_ACCOUNT` | - | Sending account doesn't exist |
| `tecUNFUNDED` | - | Insufficient balance for amount + fee + current reserve, or no tickets available in VaultState |

### 3.3 Pseudo-Transactions

The following consensus-driven pseudo-transactions are emitted by the network, not submitted by users.

#### 3.3.1 UNLReport (type 103)

Emitted at flag ledgers (every 256 ledgers) to maintain the on-chain UNL report. Not gated by any specific amendment.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `LedgerSequence` | UINT32 | Yes | Flag ledger sequence |
| `ActiveValidator` | Object | Optional | Validator to add to the active set |
| `ImportVLKey` | Object | Optional | VL publisher key to register |

#### 3.3.2 ImportCredit (type 104)

Consensus-driven import: validators propose this when the MainnetWatcher detects payments to the vault on mainnet. Mints XRP to the destination account.

**Amendment**: `featureImportExport`
**Privileges**: `createAcct | mintXRP`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Destination` | AccountID | Yes | Recipient account |
| `Amount` | Amount | Yes | XRP amount to mint |
| `SourceTxnID` | UINT256 | Yes | Mainnet transaction hash |
| `ImportSequence` | UINT32 | Yes | Import sequence for replay protection |
| `LedgerSequence` | UINT32 | Yes | Mainnet ledger sequence |
| `InvoiceID` | UINT256 | Optional | Optional invoice identifier |

#### 3.3.3 ExportConfirm (type 105)

Confirms that an export was fulfilled on mainnet. Updates the ExportVaultState singleton.

**Amendment**: `featureImportExport`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `SourceTxnID` | UINT256 | Yes | Mainnet transaction hash confirming the export |
| `LedgerSequence` | UINT32 | Yes | Mainnet ledger sequence |
| `MaxTicketSeq` | UINT32 | Optional | Updates max ticket sequence in VaultState |
| `MainnetSequence` | UINT32 | Optional | Updates vault's mainnet account Sequence |

---

## 4. Additional Ledger Entry Types

### 4.1 UNLReport (`ltUNL_REPORT`, 0x0052)

Stores the on-chain UNL report, listing active validators and recognized VL publisher keys. Used by the export validator trust model (Section 12.2).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ImportVLKeys` | Array | Optional | Array of registered VL publisher key objects |
| `ActiveValidators` | Array | Optional | Array of active validator objects |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::unlReport()`

### 4.2 ImportRecord (`ltIMPORT_RECORD`, 0x0095)

Audit trail for imports. Records each successful import for double-entry bookkeeping.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Account` | AccountID | Yes | Account that received the import |
| `Amount` | Amount | Yes | Amount imported |
| `ImportSequence` | UINT32 | Yes | Import sequence number |
| `SourceTxnID` | UINT256 | Yes | Mainnet transaction hash |
| `LedgerSequence` | UINT32 | Yes | Mainnet ledger sequence |
| `ImportDirNode` | UINT64 | Yes | Import directory page reference |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::importRecord(account, seq)`

---

## 5. XPop Structure

An XPop is a JSON blob containing cryptographic proof that a transaction was included in a validated ledger on another chain.

```json
{
  "ledger": {
    "acroot": "<hex64>",
    "txroot": "<hex64>",
    "phash":  "<hex64>",
    "close":  <uint>,
    "cres":   <uint>,
    "index":  <uint>,
    "flags":  <uint>,
    "pclose": <uint>,
    "coins":  <uint|string>
  },
  "transaction": {
    "blob":  "<hex>",
    "meta":  "<hex>",
    "proof": <proof_tree>
  },
  "validation": {
    "data": {
      "<nodepub_base58>": "<hex_signature>",
      ...
    },
    "unl": {
      "public_key": "<hex>",
      "manifest":   "<base64>",
      "blob":       "<base64>",
      "signature":  "<hex>",
      "version":    <uint>
    }
  }
}
```

### 5.1 Ledger Section

Contains the ledger header fields needed to reconstruct the ledger hash:

| Field | Type | Description |
|-------|------|-------------|
| `acroot` | Hex64 | Account state tree root hash |
| `txroot` | Hex64 | Transaction tree root hash |
| `phash` | Hex64 | Parent ledger hash |
| `close` | Integer | Close time (seconds since Ripple epoch) |
| `cres` | Integer | Close time resolution |
| `index` | Integer | Ledger index/sequence |
| `flags` | Integer | Ledger flags |
| `pclose` | Integer | Parent close time |
| `coins` | Integer/String | Total XRP drops in ledger |

### 5.2 Transaction Section

| Field | Type | Description |
|-------|------|-------------|
| `blob` | Hex | Serialized inner transaction |
| `meta` | Hex | Serialized transaction metadata |
| `proof` | Object/Array | Merkle tree proof of inclusion |

**Proof Formats**:

Two proof tree formats are supported:

**Array Format** (dense tree):
```json
[<hash_or_subtree>, <hash_or_subtree>, ... (16 elements)]
```

**Object Format** (sparse tree):
```json
{
  "hash": "<hex64>",
  "key": "<hex64>",
  "children": {
    "0": { "hash": "...", "key": "...", "children": {...} },
    "F": { ... }
  }
}
```

**Depth Limits**:
- Maximum proof depth: 64
- Maximum Merkle computation depth: 32

### 5.3 Validation Section

**`data`**: Map of validator node public keys (base58) to their hex-encoded signatures over the computed ledger hash.

**`unl`** (Validator List):

| Field | Type | Description |
|-------|------|-------------|
| `public_key` | Hex | VL publisher master public key |
| `manifest` | Base64 | Publisher manifest (contains signing key) |
| `blob` | Base64 | JSON validator list |
| `signature` | Hex | Signature over the blob using signing key from manifest |
| `version` | Integer | Format version |

**Validator List blob** (decoded from `unl.blob`):
```json
{
  "sequence": <uint>,
  "expiration": <uint>,
  "effective": <uint>,
  "validators": [
    {
      "validation_public_key": "<hex>",
      "manifest": "<base64>"
    },
    ...
  ]
}
```

---

## 6. Verification Process

### 6.1 Manifest Verification

1. Deserialize the manifest from `validation.unl.manifest`
2. Verify the manifest's master key matches `validation.unl.public_key`
3. Verify the manifest's signature is valid
4. Extract the signing key from the manifest

### 6.2 UNL Blob Verification

1. Decode the blob from base64
2. Parse as JSON and validate required fields (`sequence`, `expiration`, `validators`)
3. Check temporal validity: `effective <= now < expiration`
4. Verify the blob signature using the signing key from the manifest

### 6.3 Inner Transaction Validation

1. The inner transaction must be a `Payment` (`ttPAYMENT`)
2. The transaction result must be `tesSUCCESS` (the Payment must succeed for `DeliveredAmount` to be meaningful)
3. The `DeliveredAmount` in the metadata must be present and be positive XRP
4. The `Destination` of the Payment must match the configured `[import_vault_address]` (checked in preclaim)

### 6.4 Transaction Proof Verification

1. Serialize the inner transaction: `VL(blob) + VL(meta) + txHash`
2. Compute `SHA512Half(HashPrefix::txNode, serialized)` to get `txHashAndMeta`
3. Verify `txHashAndMeta` is present in the proof tree
4. Compute the Merkle root from the proof tree
5. Verify the computed root matches the `txroot` in the ledger section

### 6.5 Ledger Hash Verification

Compute the ledger hash from all ledger header fields and the verified transaction root. The computed hash must match what the validators signed.

### 6.6 Validator Quorum

1. Parse each validator's manifest from the UNL blob
2. For each entry in `validation.data`, verify the signature over the computed ledger hash
3. Count valid signatures
4. Require **>80%** of validators to have signed

```
threshold = (totalValidators * 80 + 99) / 100    // ceil(totalValidators * 0.8)
Quorum = validationCount >= threshold
```

---

## 7. Replay Protection

### 7.1 Per-Account Import Sequence

Each account's `ImportSequence` field tracks the highest inner transaction sequence number that has been imported. An Import is rejected if:

```
AccountRoot.ImportSequence >= InnerTransaction.Sequence
```

This prevents the same mainnet transaction from being imported twice.

### 7.2 Per-VL Sequence

The `ImportVLSeq` ledger entry tracks the highest VL sequence for each VL publisher. This prevents replay using older validator lists that may have been compromised.

---

## 8. Configuration

### 8.1 IMPORT_VL_KEYS

The sidechain must configure recognized VL publisher master public keys in its node configuration:

```
[import_vl_keys]
ED1234...  # Hex-encoded VL publisher master public key
```

Only XPop proofs signed by validators from recognized VL publishers are accepted.

### 8.2 IMPORT_VAULT_ADDRESS

The sidechain must configure the mainnet vault address that holds locked XRP:

```
[import_vault_address]
rVaultAddress123...  # The multisig vault account on XRPL mainnet
```

Only Payments to this address are accepted for Import. This vault is a standard XRPL multisig account where the UNL validators are the signers (see Section 12).

---

## 9. User Guide: Constructing a Valid Import

To transfer XRP from mainnet to the sidechain:

1. **On mainnet**, submit a `Payment` transaction with:
   - `Destination`: the configured vault address (see `[import_vault_address]`)
   - `Amount`: the XRP to lock (this will be minted 1:1 on the sidechain)
   - `OperationLimit`: set to the sidechain's `NetworkID` (prevents cross-chain confusion)
   - `Account`: your mainnet account (must match your sidechain account)
   - No `sfNetworkID` field (must be network 0 / mainnet)

2. **Obtain the XPop**: After the Payment is validated and included in a closed ledger, an XPop generator extracts the cryptographic proof (XPop JSON blob) from the mainnet ledger.

3. **On the sidechain**, submit an `Import` transaction with:
   - `Blob`: the XPop JSON blob (max 512 KiB)
   - `Account`: same account as the mainnet Payment sender
   - `Fee`: 0 for the first import (account creation), standard fee for subsequent imports
   - Sign with the same key(s) used for the mainnet Payment

The sidechain validates the XPop, verifies the Payment was to the vault, and mints the `DeliveredAmount` to your sidechain account.

---

## 10. Serialized Field Reference

### New UINT32 Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfOperationLimit` | 29 | Network ID validation for inner transaction |
| `sfImportSequence` | 81 | Import replay protection counter |
| `sfExportSequence` | 82 | Per-account export counter |
| `sfNextTicketSeq` | 83 | Next mainnet ticket to assign (ExportVaultState) |
| `sfMaxTicketSeq` | 84 | Highest allocated mainnet ticket (ExportVaultState) |
| `sfExportQuorum` | 85 | Required validator signatures for export (reserved) |
| `sfSignerCount` | 86 | Number of vault signers (reserved) |
| `sfMainnetSequence` | 88 | Vault's mainnet account Sequence (ExportVaultState) |

### New UINT256 Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfSourceTxnID` | 47 | Mainnet transaction hash (ImportCredit, ExportConfirm, ImportRecord) |

### New UINT64 Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfExportDirNode` | 35 | Export directory page reference (ExportRecord) |
| `sfImportDirNode` | 36 | Import directory page reference (ImportRecord) |

### New VL Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfBlob` | 32 | XPop proof data (JSON, max 512 KiB) |

### New Object/Array Fields

| Field | Type | ID | Description |
|-------|------|----|-------------|
| `sfActiveValidator` | Object | 42 | Validator entry in UNLReport |
| `sfImportVLKey` | Object | 43 | VL publisher key entry in UNLReport |
| `sfActiveValidators` | Array | 35 | Array of active validators (UNLReport) |
| `sfImportVLKeys` | Array | 36 | Array of VL publisher keys (UNLReport) |

---

## 11. Security Considerations

1. **Quorum threshold**: The 80% quorum requirement ensures that a supermajority of the source chain's validators confirmed the transaction. This makes forgery infeasible without compromising 80%+ of validators.

2. **VL key whitelisting**: Only recognized VL publishers are accepted, preventing arbitrary validator sets from creating valid XPops.

3. **Temporal bounds**: UNL blobs have `effective` and `expiration` timestamps. Expired or not-yet-valid lists are rejected.

4. **Signature verification**: Both the inner transaction signature and validator signatures are cryptographically verified.

5. **Network isolation**: The `OperationLimit` field must match the destination chain's `NetworkID`, preventing cross-chain confusion.

6. **Account creation security**: New accounts have their master key disabled unless the first Import is signed with the master key, preventing unauthorized account takeover.

7. **Vault address validation**: Only Payments to the configured vault address are accepted. This prevents users from importing arbitrary mainnet transactions.

8. **Vault backing**: The lock-and-mint model ensures that all sidechain XRP is backed 1:1 by XRP locked in the vault on mainnet. The total sidechain supply can never exceed the vault balance.

9. **Validator-signed exports**: The Export path is fully decentralized. UNL validators are the signers on the mainnet vault's multisig, eliminating the need for external witness services. This gives exports the same trust model as imports (80%+ validator quorum).

10. **On-chain validator set**: Export signing trusts the on-chain `UNLReport.sfActiveValidators` rather than static node configuration. This ensures all nodes agree on who can sign, prevents config drift, and automatically tracks UNL changes.

11. **Ephemeral signature collection**: Export signatures are collected in-memory via overlay gossip, never stored on-ledger. This avoids O(n^2) metadata bloat that would result from accumulating signatures in ledger transactions. Only the final assembled multisig Payment is exposed via RPC.

12. **Export recovery**: ExportRecords remain on-ledger permanently, ensuring burned XRP is always recoverable. Validators re-sign pending exports every ledger, making the system self-healing across node restarts, network partitions, and validator rotation.

---

## 12. Validator-Signed Exports

### 12.1 Overview

The `featureImportExport` amendment includes validator-signed exports, making the sidechain's UNL validators the signers on the mainnet vault multisig. This eliminates the need for external witness services.

When an Export transaction is validated, each UNL validator automatically:
1. Constructs a deterministic mainnet Payment from the ExportRecord
2. Signs it as a multisig signer
3. Piggybacks the signature on its TMValidation message

Once 80%+ of validators have signed (quorum), any relayer can retrieve the assembled multisig Payment via RPC and submit it to mainnet.

```
Sidechain                                          Mainnet
==========                                         ==========

1. User submits Export tx
   → Burns XRP, creates ExportRecord
   → Assigns ticket from VaultState

2. Each validator, during validation:
   → Scans for pending ExportRecords
   → Constructs deterministic Payment
   → Signs with validator key
   → Attaches signature to TMValidation msg
   → Re-broadcasts cached sigs every ledger

3. All nodes collect signatures
   → Two-phase verification (see 12.7)
   → When 80%+ verified, assemble
     full multisig Payment blob

4. Any relayer reads assembled Payment   ───────>  5. Submits multisig Payment
   from sidechain RPC                               → Vault releases XRP
                                                     → Destination receives funds
```

### 12.2 Export Validator Trust (UNLReport)

Export signing uses a **three-tier trust model** to determine which validators may sign export transactions:

1. **Standalone mode**: All validators are trusted (for testing)
2. **Early ledgers** (seq < 256): Use the node's local trusted validator configuration (bootstrap fallback)
3. **Normal operation**: Use `UNLReport.sfActiveValidators` from the on-chain UNL report. Fall back to local config if UNLReport is not yet available.

This differs from Import validation, which uses `[import_vl_keys]` config to verify a foreign chain's UNL. Export signing uses on-chain UNLReport because the signers are *this chain's own* validators, and the on-chain set is the canonical source of truth.

**Quorum calculation**:

```
threshold = ceil(unlSize * 0.8) = (unlSize * 80 + 99) / 100
```

Where `unlSize` is the number of active validators from UNLReport (or local config as fallback, minimum 1).

### 12.3 ExportVaultState (`ltEXPORT_VAULT_STATE`, 0x0093)

A singleton ledger entry tracking the state of the mainnet vault as known to the sidechain.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `NextTicketSeq` | UINT32 | Yes | Next mainnet ticket to assign |
| `MaxTicketSeq` | UINT32 | Yes | Highest allocated mainnet ticket |
| `MainnetSequence` | UINT32 | Optional | Vault's mainnet account Sequence number |
| `SignerListHash` | UINT256 | Optional | Hash of current validator-derived signer list |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::exportVaultState()` (singleton)

### 12.4 Ticket Assignment

The Export transactor:

1. Reads the VaultState to get the next available mainnet ticket
2. Assigns the ticket to the ExportRecord (`sfTicketSequence`)
3. Increments `NextTicketSeq` in the VaultState

If `NextTicketSeq > MaxTicketSeq`, the Export fails with `tecUNFUNDED`.

### 12.5 Overlay: Piggybacking on TMValidation

Export signatures are carried as a **repeated bytes field** on the existing `TMValidation` protobuf message, rather than introducing a new message type. This is efficient: no new protocol negotiation, no separate gossip topology, and signatures naturally propagate with validation messages which already have optimized network paths.

```protobuf
message TMValidation {
  // ...existing fields...
  repeated bytes exportSignatures = N;
  // Each entry: txnHash (32 bytes) + serialized sfSigner STObject
}
```

Each `exportSignatures` entry contains:
- **txnHash** (32 bytes): Hash of the deterministic unsigned mainnet Payment (used as the key for signature collection)
- **sfSigner** (variable): A serialized STObject containing `sfAccount`, `sfSigningPubKey`, and `sfTxnSignature`

Messages are only processed from validators trusted per the UNLReport trust model (Section 12.2). The HashRouter deduplicates by `hash(txnHash + validatorKey)`.

### 12.6 Deterministic Payment Construction

All validators must produce **byte-identical** unsigned mainnet Payment transactions from the same ExportRecord. This is essential because XRPL multisig requires all signers to sign the exact same transaction bytes.

- `TransactionType`: Payment
- `Flags`: `tfFullyCanonicalSig`
- `Account`: Vault address (from `[import_vault_address]` config)
- `Destination`: From ExportRecord
- `Amount`: From ExportRecord
- `Sequence`: 0 (using ticket)
- `TicketSequence`: From ExportRecord
- `Fee`: `(signerCount + 1) * 15` drops
- `SigningPubKey`: Empty `Blob{}` (required for multisig)
- `DestinationTag`: From ExportRecord (if present)

Each validator computes the per-signer multisign hash:
```
SHA512Half(HashPrefix::txMultiSign || serializedTxFields || signerAccountID)
```

Where `signerAccountID = calcAccountID(validatorSigningKey)`.

### 12.7 Signature Collection and Assembly

Each node maintains an `ExportSignatureCollector` -- a thread-safe, mutex-protected in-memory collector keyed by `(account, exportSequence)`.

**Two-phase signature verification** handles the race condition where signatures may arrive from peers before the local node has processed the Export transaction:

1. **On receipt** (from peer TMValidation): If the transaction data is already cached locally, verify the signature immediately against the expected multisign hash. If transaction data is not yet available (race condition), store the signature as **unverified**.

2. **On local signing** (validator processes the Export): Cache the transaction data. Retroactively verify all previously-stored unverified signatures, **pruning any that fail**.

This ensures no valid signatures are dropped due to timing, while invalid signatures are always caught.

**Identity binding**: Every signature is checked for consistency -- the signer's `sfSigningPubKey` and `sfAccount` must match the validator who sent it (`calcAccountID(validatorKey) == signerAccount`). This prevents signature misattribution.

**Assembly**: Once quorum is reached, the collector assembles the final multisig Payment by sorting all verified signer objects by AccountID (ascending, as required by XRPL multisig) into an `sfSigners` array attached to the unsigned Payment.

**Stale cleanup**: Entries older than 256 ledgers without quorum are pruned from memory to prevent leaks.

### 12.8 Sign-Once, Broadcast-Many

Validators sign each ExportRecord **once** but **re-broadcast their cached signature every validation cycle** (every ledger) as long as the ExportRecord remains on-ledger. This makes the system self-healing:

- Late-joining validators catch up automatically
- Network partitions resolve on reconnect
- Node restarts recover by re-signing from on-ledger ExportRecords

This is why the ExportRecord must remain on-ledger permanently -- it is the signal that tells validators "this export still needs signatures." Without it, validators would have no way to discover pending exports after a restart.

### 12.9 RPC Endpoints

**`export_status`**: Returns status of a pending export's signature collection.

```json
{
  "account": "rExporter...",
  "export_sequence": 5,
  "destination": "rMainnetDst...",
  "amount": "100000000",
  "ticket_sequence": 42,
  "signatures_collected": 18,
  "signatures_required": 26,
  "quorum_reached": false
}
```

**`export_payment`**: Returns the assembled multisig Payment blob (hex-encoded, ready to submit to mainnet).

```json
{
  "account": "rExporter...",
  "export_sequence": 5,
  "mainnet_payment_blob": "1200002200000000...",
  "submit_ready": true
}
```

A relayer (any client or service) calls `export_payment`, and if `submit_ready` is true, submits the `mainnet_payment_blob` directly to a mainnet node via the standard `submit` RPC.

### 12.10 Validator Rotation

At flag ledgers (every 256 ledgers), the sidechain detects UNL changes by comparing the hash of the current validator-derived signer list (from UNLReport) against `VaultState.SignerListHash`. If changed, validators collectively sign a mainnet `SignerListSet` transaction to update the vault's signer list, using reserved export sequence `0xFFFFFFFE`.

### 12.11 Ticket Management

Mainnet tickets (max 250) are pre-allocated on the vault. The sidechain assigns them sequentially via `VaultState.NextTicketSeq`. When the pool runs low (<25% remaining, checked at flag ledgers), validators collectively sign a mainnet `TicketCreate` transaction to replenish it, using reserved export sequence `0xFFFFFFFF`.

### 12.12 Signer Identity

Each validator's signer AccountID on the mainnet SignerList is derived from their current signing (ephemeral) public key: `calcAccountID(signingPubKey)`. When keys rotate via manifests, the SignerList is updated accordingly (Section 12.10).
