# XLS-XXd: P256 Passkey Authentication

```
Title:       P256 Passkey Authentication
Type:        Draft
Author:      Elliot Lee <https://github.com/intelliot>,
             Javi Romero <https://github.com/elmurci>,
             Denis Angell <https://github.com/dangell7>
Revision:    1
Discussion:  https://github.com/XRPLF/XRPL-Standards/discussions/236
```

## Abstract

This specification introduces **P256 (NIST prime256v1/secp256r1) elliptic curve** support and **WebAuthn/FIDO2 passkey authentication** to the XRP Ledger Protocol. Accounts can register passkey credentials on-ledger and use them to sign transactions, enabling hardware security key and biometric authentication (Face ID, Touch ID, Windows Hello) for XRP Ledger accounts.

| Amendment | Description |
|-----------|-------------|
| `featurePasskey` | P256 key type, PasskeyList ledger entry, SetPasskeyList transaction, passkey-based transaction signing |

---

## 1. New Key Type: P256

### 1.1 Overview

A new `KeyType::p256` (value `2`) is added alongside the existing `secp256k1` (value `0`) and `ed25519` (value `1`) key types. P256 (also known as NIST P-256, secp256r1, or prime256v1) is the elliptic curve used by WebAuthn/FIDO2 authenticators.

### 1.2 Public Key Format

P256 public keys are encoded as 65 bytes in uncompressed form:

| Byte(s) | Content |
|---------|---------|
| `0` | Prefix byte `0xF6` |
| `1-32` | X coordinate (32 bytes, big-endian) |
| `33-64` | Y coordinate (32 bytes, big-endian) |

The `0xF6` prefix distinguishes P256 keys from secp256k1 (`0x02`/`0x03`) and ed25519 (`0xED`) keys.

### 1.3 Signature Verification

P256 signatures use **SHA-256** hashing (not SHA-512-Half as used by secp256k1) and standard ECDSA signature verification via OpenSSL.

### 1.4 Key Derivation

P256 secret keys are derived from seeds using the same deterministic root key derivation as secp256k1 (SHA-512-Half based). Public keys are derived via EC point multiplication on the P-256 curve.

---

## 2. Ledger Entry Types

### 2.1 PasskeyList (`ltPASSKEY_LIST`, 0x0094)

Stores the list of registered passkey credentials for an account. Each account can have at most one PasskeyList object.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `Owner` | AccountID | Yes | Account that owns this passkey list |
| `Passkeys` | Array | Yes | Array of registered passkey objects |

**Keylet**: `keylet::passkeyList(AccountID)` (namespace `'k'`)

Each entry in the `Passkeys` array is an `sfPasskey` inner object:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `PasskeyID` | VL | Yes | WebAuthn credential ID (opaque identifier) |
| `PublicKey` | VL | Yes | P256 public key (65 bytes, `0xF6` prefix) |

---

## 3. Transaction Types

### 3.1 SetPasskeyList (`ttPASSKEY_LIST_SET`, type 72)

Creates, updates, or deletes the passkey list for the submitting account.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Passkeys` | Array | Yes | Array of passkey objects to register |

**Amendment**: `featurePasskey`
**Delegation**: Delegatable

#### Behavior

- **Create**: If no PasskeyList exists for the account, creates one with the provided passkeys. Requires one owner reserve.
- **Update**: If a PasskeyList already exists, replaces the passkeys array with the new set.
- **Delete**: If the `Passkeys` array is empty, deletes the PasskeyList and frees the owner reserve.

#### Preflight Validation

- Maximum of 10 passkeys per account
- Each passkey must have both `PasskeyID` and `PublicKey` fields
- Each `PublicKey` must be a valid P256 key (65 bytes with `0xF6` prefix)

---

## 4. Serialized Fields

| Field Name | Type | Field ID | Notes |
|------------|------|----------|-------|
| `sfPasskeySignCount` | UINT32 | 87 | Sign counter for replay protection |
| `sfPasskeyID` | VL | 33 | WebAuthn credential identifier |
| `sfAuthenticatorData` | VL | 34 | WebAuthn authenticator data |
| `sfClientDataJSON` | VL | 35 | WebAuthn client data JSON |
| `sfPasskeySignature` | OBJECT | 40 | Passkey signature object (notSigning) |
| `sfPasskey` | OBJECT | 41 | Individual passkey entry |
| `sfPasskeys` | ARRAY | 34 | Array of passkey entries |

---

## 5. WebAuthn Transaction Signing

### 5.1 Overview

When a transaction is signed with a P256 passkey, the signing process follows the WebAuthn/FIDO2 specification rather than the standard XRPL signing flow.

### 5.2 Signing Data Construction

For P256/passkey-signed transactions, the data verified by the signature is:

```
signingData = authenticatorData || SHA256(clientDataJSON)
```

Where:
- `authenticatorData` is the raw authenticator data from the WebAuthn assertion
- `clientDataJSON` is the client data JSON from the WebAuthn assertion
- The `clientDataJSON` contains a `challenge` field which encodes the transaction hash

### 5.3 Transaction Structure

A passkey-signed transaction includes:

| Field | Description |
|-------|-------------|
| `SigningPubKey` | The P256 public key (65 bytes, `0xF6` prefix) |
| `PasskeySignature` | Object containing passkey-specific signature data |

The `PasskeySignature` object contains:

| Field | Description |
|-------|-------------|
| `PasskeyID` | The credential ID identifying which passkey was used |
| `AuthenticatorData` | Raw authenticator data from the WebAuthn assertion |
| `ClientDataJSON` | Client data JSON from the WebAuthn assertion |
| `Signature` | The raw ECDSA P256 signature (DER-encoded) |

Note: The `PasskeySignature` object is marked as `notSigning`, meaning it is excluded from the signing data serialization.

### 5.4 Signature Verification Flow

1. Extract `AuthenticatorData` and `ClientDataJSON` from `PasskeySignature`
2. Compute `clientDataHash = SHA256(ClientDataJSON)`
3. Construct `signingData = AuthenticatorData || clientDataHash`
4. Verify the ECDSA P256 signature over `signingData` using the `SigningPubKey`

---

## 6. Authentication Flow

### 6.1 Single Sign with Passkey

When a transaction is submitted with a P256 signing key, the `checkSingleSign` function in the Transactor performs the following checks in order:

1. **Regular Key**: If the signer's AccountID matches the account's regular key, authorize.
2. **Master Key**: If the master key is not disabled and the signer's AccountID matches the account ID, authorize.
3. **Passkey**: If `featurePasskey` is enabled:
   a. Look up the account's `PasskeyList` ledger object.
   b. Check if any registered passkey's public key derives to the signer's AccountID.
   c. If a match is found, authorize the transaction.
4. Otherwise, reject with `tefBAD_AUTH`.

---

## 7. Security Considerations

- P256 keys are generated using the same deterministic seed derivation as secp256k1, but this is primarily for testing. In production, passkey credentials are generated by hardware authenticators.
- The `PasskeySignature` object is excluded from signing data serialization to prevent circular dependencies.
- The maximum of 10 passkeys per account limits the storage impact on the ledger.
- Each PasskeyList requires one owner reserve, incentivizing cleanup of unused passkey lists.
