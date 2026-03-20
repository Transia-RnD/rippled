# XLS-62d: Options and Margin Trading

```
Title:       Options and Margin Trading
Type:        Draft
Author:      Denis Angell <dangell@transia.co>
Revision:    8
```

## Abstract

This specification introduces **Options trading** and **Margin/Leverage trading** to the XRP Ledger Protocol. Options are American-style derivative contracts that give holders the right (but not the obligation) to buy or sell an underlying asset at a predetermined strike price before expiration. The margin system enables leveraged positions with risk management via liquidation, insurance vaults, and funding rates.

A single amendment controls this feature:

| Amendment | Description |
|-----------|-------------|
| `featureOptions` | All options and margin trading: OptionPairCreate, OptionCreate, OptionSettle, LeverageTierSet, MarginAccountSet, MarginDeposit, MarginWithdraw, OptionLiquidate, InsuranceVaultCreate, InsuranceDeposit, InsuranceWithdraw |

---

## 1. Ledger Entry Types

### 1.1 OptionPair (`ltOPTION_PAIR`, 0x008A)

A pseudo-account representing an options market for a specific asset pair. Similar to the AMM design pattern, the OptionPair account has its master key disabled and regular key set to account zero.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Account` | AccountID | Yes | Pseudo-account address |
| `Asset` | Issue | Yes | Base asset (underlying) |
| `Asset2` | Issue | Yes | Quote asset (settlement currency) |
| `OracleEntries` | Array | Yes | Array of oracle references for mark price (see Section 3.5) |
| `TradingFeeBps` | UINT32 | Default(0) | Trading fee in 1/10 basis points |
| `AccumulatedFees` | Number | Default(0) | Accumulated trading fees |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `PreviousTxnID` | UINT256 | Optional | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Optional | Previous transaction ledger sequence |

**Keylet**: `keylet::optionPair(Asset, Asset2)` (namespace `'Z'`)

### 1.2 Option (`ltOPTION`, 0x008B)

Defines a specific option contract by its strike price, underlying asset, and expiration.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `StrikePrice` | Amount | Yes | The strike price of the option |
| `Asset` | Issue | Yes | The underlying asset |
| `Expiration` | UINT32 | Yes | Unix timestamp for option expiration |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::option(issuer, currency, strike, expiration)` (namespace `'X'`)

### 1.3 OptionOffer (`ltOPTION_OFFER`, 0x008C)

An individual user's offer to buy or sell an option contract.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Owner` | AccountID | Yes | Account that created the offer |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `StrikePrice` | Amount | Yes | Strike price |
| `Asset` | Issue | Yes | Underlying asset |
| `Expiration` | UINT32 | Yes | Option expiration time |
| `Premium` | Amount | Yes | Price per option contract |
| `Quantity` | UINT32 | Yes | Number of contracts (divisible by 100) |
| `OpenInterest` | UINT32 | Optional | Number of matched contracts |
| `SealedOptions` | Array | Optional | Matched counterparty relationships |
| `MarginPositionID` | UINT256 | Optional | Link to margin position (leveraged) |
| `BookDirectory` | UINT256 | Yes | Option book directory |
| `BookNode` | UINT64 | Yes | Book directory node |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::optionOffer(id, seq)` (namespace `'y'`)

### 1.4 LeverageTier (`ltLEVERAGE_TIER`, 0x008D)

Defines margin parameters for an asset pair at various leverage levels.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Owner` | AccountID | Yes | Account that created the tier |
| `Asset` | Issue | Yes | Base asset |
| `Asset2` | Issue | Yes | Quote asset |
| `MaxLeverage` | UINT32 | Yes | Maximum leverage multiplier |
| `InitialMarginBps` | UINT32 | Yes | Initial margin in 1/10 basis points |
| `MaintenanceMarginBps` | UINT32 | Yes | Maintenance margin in 1/10 basis points |
| `LiquidationBonusBps` | UINT32 | Yes | Liquidation bonus in 1/10 basis points |
| `LeverageTiers` | Array | Optional | Sub-tiers with different leverage levels |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::leverageTier(Asset, Asset2)` (namespace `'W'`)

### 1.5 MarginAccount (`ltMARGIN_ACCOUNT`, 0x008E)

A user's margin account that holds collateral for leveraged positions.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Account` | AccountID | Yes | Account owner |
| `CollateralAsset` | Issue | Yes | Collateral denomination |
| `CollateralBalance` | Number | Default(0) | Current collateral balance |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::marginAccount(account, collateralAsset)` (namespace `'M'`)

### 1.6 InsuranceVault (`ltINSURANCE_VAULT`, 0x008F)

Insurance pool backing an options market against default risk.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Account` | AccountID | Yes | Pseudo-account address |
| `Asset` | Issue | Yes | Base asset |
| `Asset2` | Issue | Yes | Quote asset |
| `InsuranceBalance` | Number | Default(0) | Current insurance pool balance |
| `ShareMPTID` | UINT192 | Yes | MPToken Issuance ID for LP shares |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::insuranceVault(Asset, Asset2)` (namespace `'J'`)

### 1.7 MarginPosition (`ltMARGIN_POSITION`, 0x0090)

A single leveraged position linked to a margin account.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Account` | AccountID | Yes | Position owner |
| `MarginAccountID` | UINT256 | Yes | Associated margin account |
| `OptionPairID` | UINT256 | Yes | Associated option pair |
| `Asset` | Issue | Yes | Base asset |
| `EntryPrice` | Number | Yes | Price at position entry |
| `PositionSize` | Number | Yes | Size of the position |
| `PositionSide` | UINT32 | Yes | `0` = Long, `1` = Short |
| `Leverage` | UINT32 | Yes | Leverage multiplier (2-200) |
| `AllocatedMargin` | Number | Default(0) | Margin allocated to this position |
| `NotionalValue` | Number | Default(0) | Total notional value |
| `LiquidationPrice` | Number | Default(0) | Price triggering liquidation |
| `LastFundingTime` | UINT32 | Default(0) | Last funding rate collection time |
| `LastFundingPayment` | Number | Default(0) | Last funding payment amount |
| `OptionOfferID` | UINT256 | Optional | Linked option offer |
| `MarginAccountNode` | UINT64 | Yes | Margin account directory node |
| `OwnerNode` | UINT64 | Yes | Owner directory node |
| `PreviousTxnID` | UINT256 | Yes | Previous transaction hash |
| `PreviousTxnLgrSeq` | UINT32 | Yes | Previous transaction ledger sequence |

**Keylet**: `keylet::marginPosition(account, seq)` (namespace `'j'`)

---

## 2. Transaction Types

### 2.1 OptionPairCreate (type 85)

Creates a new options market for an asset pair.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Asset` | Issue | Yes | Base (underlying) asset |
| `Asset2` | Issue | Yes | Quote (settlement) asset |
| `OracleEntries` | Array | Yes | Oracle references for price data (1-10 entries) |
| `TradingFeeBps` | UINT32 | Optional | Trading fee in 1/10 basis points |

**Behavior**:
- Validates `OracleEntries` (1-10 entries, each must have `Account` + `OracleDocumentID`)
- Verifies each referenced oracle exists on-ledger (fails with `tecNO_ENTRY` if not)
- Creates a pseudo-account with master key disabled and regular key set to account zero
- Creates the `OptionPair` ledger entry with oracle references stored for mark price lookups
- Fails with `tecDUPLICATE` (148) if the pair already exists

**Privileges**: `createPseudoAcct`

### 2.2 OptionCreate (type 87)

Creates a buy or sell offer for an option contract.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `StrikePrice` | Amount | Yes | Option strike price |
| `Asset` | Issue | Yes | Underlying asset |
| `Expiration` | UINT32 | Yes | Option expiration (Unix timestamp) |
| `Premium` | Amount | Yes | Price per contract |
| `Quantity` | UINT32 | Yes | Number of contracts (must be divisible by 100) |
| `MarginAccountID` | UINT256 | Yes | Margin account for the position |
| `Leverage` | UINT32 | Yes | Leverage multiplier (2-200) |

**Flags**:

| Flag | Value | Description |
|------|-------|-------------|
| `tfPut` | `0x00010000` | Put option (default is Call) |
| `tfMarket` | `0x00020000` | Market order (match immediately at best price) |
| `tfSell` | `0x00080000` | Sell offer (default is Buy) |

**Behavior**:
1. Validates the option pair exists
2. Validates quantity is divisible by 100
3. Creates a `MarginPosition` linked to the offer, backed by the specified margin account
4. Attempts to match against existing counterpart offers on the option book
5. Matched offers create **Sealed Options** linking buyer and seller
6. Premium is transferred from buyer to seller on match
7. Unmatched remainder is placed on the book as a resting offer

**Errors**:
- `temMALFORMED`: Invalid fields, quantity not divisible by 100
- `tecEXPIRED` (148): Option has expired
- `tecDUPLICATE` (149): Duplicate option pair

### 2.3 OptionSettle (type 88)

Settles an option by exercising, closing, or expiring it.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `OptionID` | UINT256 | Yes | The option to settle |
| `OptionOfferID` | UINT256 | Yes | The specific offer to settle |

**Flags** (exactly one must be set):

| Flag | Value | Description |
|------|-------|-------------|
| `tfExpire` | `0x00010000` | Expire an option past its expiration time |
| `tfClose` | `0x00020000` | Close an unexercised position, returning collateral |
| `tfExercise` | `0x00040000` | Exercise the option (buyers only) |

**Behavior**:

- **Exercise** (`tfExercise`):
  - Only the buyer can exercise
  - Transfers the difference between mark price and strike price
  - Updates margin positions if leveraged
  - Removes sealed option relationships

- **Close** (`tfClose`):
  - Returns locked collateral to the seller
  - Updates counterparty references
  - May find replacement offers on the book

- **Expire** (`tfExpire`):
  - Only valid after the expiration timestamp
  - Returns locked collateral to the seller
  - Removes all sealed option relationships

**Errors**:
- `temMALFORMED`: Missing fields or multiple/zero flags set
- `tecEXPIRED`: Option expired (for exercise/close)
- Account doesn't own the offer

### 2.4 LeverageTierSet (type 89)

Configures leverage parameters for an asset pair.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Asset` | Issue | Yes | Base asset |
| `Asset2` | Issue | Yes | Quote asset |
| `MaxLeverage` | UINT32 | Yes | Maximum allowed leverage |
| `InitialMarginBps` | UINT32 | Yes | Initial margin requirement (1/10 bps) |
| `MaintenanceMarginBps` | UINT32 | Yes | Maintenance margin requirement (1/10 bps) |
| `LiquidationBonusBps` | UINT32 | Yes | Liquidation bonus for liquidators (1/10 bps) |
| `LeverageTiers` | Array | Optional | Sub-tier configurations |

### 2.5 MarginAccountSet (type 90)

Creates or modifies a margin account.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `CollateralAsset` | Issue | Yes | Asset used as collateral |

All margin accounts use **isolated margin**: each position has independent collateral. If one position is liquidated, others are unaffected.

### 2.6 MarginDeposit (type 91)

Deposits collateral into a margin account.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `MarginAccountID` | UINT256 | Yes | Target margin account |
| `Amount` | Amount | Yes | Amount to deposit (MPT supported) |

### 2.7 MarginWithdraw (type 92)

Withdraws collateral from a margin account.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `MarginAccountID` | UINT256 | Yes | Source margin account |
| `Amount` | Amount | Yes | Amount to withdraw (MPT supported) |

Withdrawal fails if it would leave the account below maintenance margin requirements.

### 2.8 OptionLiquidate (type 93)

Liquidates an underwater margin position. Can be called by **any account**, not just the position owner.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `MarginPositionID` | UINT256 | Yes | Position to liquidate |

**Liquidation Conditions**:
- Position equity (`(allocatedMargin - accumulatedFunding) + unrealizedPnL`) < maintenance margin

**Liquidation Process**:
1. Calculate remaining collateral: `allocatedMargin + unrealizedPnL`
2. Calculate liquidation bonus: `min(remainingCollateral * liquidationBonusBps / 100000, notionalValue * 1%)`
3. Pay bonus to liquidator
4. Return surplus to position owner
5. If deficit exists, draw from insurance vault

**Errors**:
- `tecCANT_LIQUIDATE` (199): Position is still healthy

### 2.9 InsuranceVaultCreate (type 94)

Creates an insurance vault for an option pair.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Asset` | Issue | Yes | Base asset |
| `Asset2` | Issue | Yes | Quote asset |

**Privileges**: `createPseudoAcct | createMPTIssuance`

Creates a pseudo-account and an MPToken issuance for LP shares.

### 2.10 InsuranceDeposit (type 95)

Deposits funds into the insurance vault.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `InsuranceVaultID` | UINT256 | Yes | Target insurance vault |
| `Amount` | Amount | Yes | Deposit amount (MPT supported) |

**Privileges**: `mayAuthorizeMPT`

Depositors receive proportional MPToken LP shares.

### 2.11 InsuranceWithdraw (type 96)

Withdraws funds from the insurance vault.

**Amendment**: `featureOptions`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `InsuranceVaultID` | UINT256 | Yes | Source insurance vault |
| `Amount` | Amount | Yes | Withdrawal amount (MPT supported) |

**Privileges**: `mayDeleteMPT | mayAuthorizeMPT`

---

## 3. Margin Mathematics

### 3.1 Initial Margin

```
InitialMargin = NotionalValue / Leverage
```

### 3.2 Maintenance Margin

```
MaintenanceMargin = NotionalValue * MaintenanceMarginBps / 100000
```

### 3.3 Liquidation Price

```
Long:  LiquidationPrice = EntryPrice * (1 - 1/Leverage + MaintenanceRate)
Short: LiquidationPrice = EntryPrice * (1 + 1/Leverage - MaintenanceRate)
```

Where `MaintenanceRate = MaintenanceMarginBps / 100000`.

### 3.4 Unrealized PnL

```
Long:  UnrealizedPnL = (MarkPrice - EntryPrice) * PositionSize
Short: UnrealizedPnL = (EntryPrice - MarkPrice) * PositionSize
```

### 3.5 Mark Price (Oracle-Derived)

The mark price is **not stored on any ledger entry**. It is computed at runtime by reading oracle references directly from the `OptionPair` SLE.

Each `OptionPair` stores an `OracleEntries` array (set at creation via `OptionPairCreate`). Each entry contains:

| Field | Type | Description |
|-------|------|-------------|
| `Account` | AccountID | Oracle provider account |
| `OracleDocumentID` | UINT32 | Oracle document identifier |

**Computation** (`margin::getMarkPrice`):

1. Read `OracleEntries` from the OptionPair SLE
2. For each entry, perform a direct O(1) lookup via `keylet::oracle(Account, OracleDocumentID)`
3. Within each Oracle entry, search `sfPriceDataSeries` for entries matching the currency pair (by `sfBaseAsset` and `sfQuoteAsset`)
4. Extract `sfAssetPrice` and `sfScale` from matching entries
5. Compute the **median** of all collected prices

This uses the same oracle lookup pattern as the `get_aggregate_price` RPC, providing O(1) per-oracle access rather than directory scanning.

If no oracle data is found, `getMarkPrice` returns 0, causing dependent transactions (liquidation, withdrawal, exercise) to fail.

**Used by**: OptionLiquidate (preclaim + doApply), MarginWithdraw (health check), OptionSettle (via `exerciseOffer`), `get_margin_status` RPC.

### 3.6 Funding Rate (Lazy Evaluation)

Funding is calculated lazily at settlement, liquidation, or equity check time — no per-position hourly transactions.

```
HoursElapsed = floor((CurrentTime - LastFundingTime) / 3600)
AccumulatedFunding = NotionalValue * FundingRateBps * HoursElapsed / 100000
EffectiveMargin = AllocatedMargin - AccumulatedFunding  (capped at 0)
```

Where `FundingRateBps` defaults to 100 (0.01% per hour), read from the `LeverageTier`.

`LastFundingTime` is set to the current ledger close time at position creation. Legacy positions with `LastFundingTime = 0` accrue no funding.

Accumulated funding is deducted from margin and routed to `OptionPair.AccumulatedFees` at:
- Option close (OptionSettle with tfClose)
- Option expire (OptionSettle with tfExpire)
- Option exercise (OptionSettle with tfExercise)
- Liquidation (OptionLiquidate)

### 3.7 Position Health

```
PositionEquity = (AllocatedMargin - AccumulatedFunding) + UnrealizedPnL
Healthy = PositionEquity >= MaintenanceMargin
```

---

## 4. Sealed Options (Order Matching)

When a buy offer matches a sell offer on the option book:

1. The matched quantity creates **sealed option** objects stored in the `SealedOptions` array on both buyer and seller offers
2. Premium is transferred from buyer to seller
3. The seller's collateral remains locked
4. `OpenInterest` is updated on both offers to reflect matched contracts
5. A single buyer may match multiple sellers (and vice versa)

Sealed options track the counterparty relationship and enable exercise, where the buyer can exercise against specific sealed option entries.

---

## 5. Margin-Backed Options

All option offers are margin-backed. Every OptionCreate transaction requires a `MarginAccountID` and `Leverage` parameter — the seller backs the position with collateral in a `MarginAccount`. Settlement is cash-settled based on the price difference, and the margin system's liquidation mechanism protects against insolvency.

> **Note**: Truly naked (zero-collateral) options are not possible. On a decentralized ledger there is no credit system or legal enforcement to guarantee settlement from an unfunded account. Every sell-side offer must be backed by margin collateral.

---

## 6. RPC Methods

### 6.1 `option_book_offers`

Query active offers for a specific option.

**Request Parameters**:

| Field | Type | Description |
|-------|------|-------------|
| `strike_price` | Amount | Strike price to query |
| `asset` | Issue | Underlying asset |
| `expiration` | UINT32 | (Optional) Expiration filter |
| `limit` | UINT32 | (Optional) Maximum results |
| `marker` | Object | (Optional) Pagination marker |

**Response**: Array of OptionOffer objects sorted by quality (premium).

### 6.2 `get_margin_status`

Query margin account health and position details.

**Request Parameters**:

| Field | Type | Description |
|-------|------|-------------|
| `margin_account` | UINT256 | Margin account ID |

**Response**:

| Field | Description |
|-------|-------------|
| `collateral_balance` | Current collateral |
| `positions` | Array of position details |

Each position includes: `entry_price`, `position_size`, `leverage`, `allocated_margin`, `notional_value`, `unrealized_pnl`, `liquidation_price`, `position_side`, `healthy`, `equity`, `maintenance_margin`.

---

## 7. Invariant Checks

### ValidMarginAccount (`MarginInvariant.h`)

Enforces the following safety invariants on every transaction:

1. `MarginAccount.CollateralBalance` must never be negative
2. `MarginPosition.AllocatedMargin` must never be negative
3. `InsuranceVault.InsuranceBalance` must never be negative
4. Deleted margin positions must have had zero allocated margin

---

## 8. Error Codes

| Code | Name | Description |
|------|------|-------------|
| 148 | `tecEXPIRED` | Option has expired |
| 149 | `tecDUPLICATE` | Option pair already exists |
| 199 | `tecCANT_LIQUIDATE` | Margin position is still healthy |

---

## 9. Serialized Field Reference

### New UINT256 Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfOptionPairID` | 39 | OptionPair identifier (pseudo-account designator) |
| `sfOptionID` | 40 | Option identifier |
| `sfOptionOfferID` | 41 | OptionOffer identifier |
| `sfLeverageTierID` | 42 | LeverageTier identifier |
| `sfMarginAccountID` | 43 | MarginAccount identifier |
| `sfInsuranceVaultID` | 44 | InsuranceVault identifier (pseudo-account designator) |
| `sfMarginPositionID` | 45 | MarginPosition identifier |

### New Amount Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfStrikePrice` | 32 | Option strike price |
| `sfPremium` | 33 | Option premium (price per contract) |

### New UINT32 Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfOpenInterest` | 69 | Matched contract count |
| `sfQuantity` | 70 | Number of contracts |
| `sfMaxLeverage` | 71 | Maximum leverage multiplier |
| `sfInitialMarginBps` | 72 | Initial margin (1/10 bps) |
| `sfMaintenanceMarginBps` | 73 | Maintenance margin (1/10 bps) |
| `sfLiquidationBonusBps` | 74 | Liquidation bonus (1/10 bps) |
| `sfMarginMode` | 75 | Reserved (unused) |
| `sfLeverage` | 76 | Position leverage |
| `sfPositionSide` | 77 | 0=long, 1=short |
| `sfTradingFeeBps` | 78 | Trading fee (1/10 bps) |
| `sfFundingRateBps` | 79 | Funding rate (1/10 bps) |
| `sfLastFundingTime` | 80 | Last funding collection timestamp |

### New UINT64 Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfStrike` | 32 | Strike as integer (directory ordering) |
| `sfInsuranceVaultNode` | 33 | Insurance vault directory node |
| `sfMarginAccountNode` | 34 | Margin account directory node |

### New Number Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfCollateralBalance` | 18 | Margin account collateral balance |
| `sfEntryPrice` | 19 | Position entry price |
| `sfMarkPrice` | 20 | Current mark price (not stored; computed at runtime from Oracle entries, see Section 3.5) |
| `sfPositionSize` | 21 | Position size |
| `sfAllocatedMargin` | 22 | Margin allocated to position |
| `sfInsuranceBalance` | 23 | Insurance vault balance |
| `sfNotionalValue` | 24 | Total position notional value |
| `sfLiquidationPrice` | 25 | Liquidation trigger price |
| `sfAccumulatedFees` | 26 | Accumulated trading fees |
| `sfLastFundingPayment` | 27 | Last funding payment amount |

### New Object/Array Fields

| Field | Type | ID | Description |
|-------|------|----|-------------|
| `sfSealedOption` | Object | 38 | Single matched option pair |
| `sfLeverageTier` | Object | 39 | Single leverage tier config |
| `sfOracleEntry` | Object | 44 | Single oracle reference (`Account` + `OracleDocumentID`) |
| `sfSealedOptions` | Array | 32 | Array of matched option pairs |
| `sfLeverageTiers` | Array | 33 | Array of leverage tier configs |
| `sfOracleEntries` | Array | 37 | Array of oracle references |

### New Issue Fields

| Field | ID | Description |
|-------|-----|-------------|
| `sfCollateralAsset` | 5 | Collateral asset denomination |

---

## 10. Ledger Namespace Characters

| Namespace | Character | Description |
|-----------|-----------|-------------|
| `OPTION_PAIR` | `'Z'` | OptionPair keylet |
| `OPTION` | `'X'` | Option keylet |
| `OPTION_OFFER` | `'y'` | OptionOffer keylet |
| `LEVERAGE_TIER` | `'W'` | LeverageTier keylet |
| `MARGIN_ACCOUNT` | `'M'` | MarginAccount keylet |
| `INSURANCE_VAULT` | `'J'` | InsuranceVault keylet |
| `MARGIN_POSITION` | `'j'` | MarginPosition keylet |

---

## 11. Existing Ledger Entry Extensions

### 11.1 AccountRoot

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `OptionPairID` | UINT256 | Optional | Pseudo-account designator linking to the OptionPair this account represents |

### 11.2 DirectoryNode

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `Strike` | UINT64 | Optional | Strike price for option book directory ordering |
| `Expiration` | UINT32 | Optional | Expiration timestamp for option book directory ordering |
