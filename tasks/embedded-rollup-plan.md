# Embedded Rollup: Consensus-Within-Consensus for xrpld

## Context

We want to build a specialized inner chain (starting with options/derivatives) that runs inside the same xrpld binary as the outer chain (mainnet). This is a rollup architecture where:
- The inner chain has fast soft-finality via its own UNL (~1-2s ledgers)
- The outer chain provides hard-finality by committing inner state roots (~4s)
- Value moves between chains via Import/Export (already specced on `transia/options-sidechain`)
- Operators run ONE node binary with TWO UNLs configured

No production system runs two independent BFT consensus loops with separate trust graphs in a single binary. Closest precedents: Avalanche (multi-chain in one binary, but probabilistic consensus) and Polkadot Cumulus (two nodes in one binary, but they're decoupling due to pain).

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    xrpld Binary                           │
│                                                           │
│  ┌────────────────────┐    ┌────────────────────────┐    │
│  │   Outer Chain       │    │    Inner Chain           │    │
│  │   (Standard XRPL)  │    │    (Options/Derivatives) │    │
│  │                     │    │                          │    │
│  │  Consensus<RCL>     │    │  Consensus<InnerAdaptor> │    │
│  │  RCLLedgerMaster    │    │  InnerLedgerMaster       │    │
│  │  ValidatorList      │    │  InnerValidatorList      │    │
│  │  OpenLedger         │    │  InnerOpenLedger         │    │
│  │  NodeStore (NuDB)   │    │  InnerNodeStore (NuDB)   │    │
│  └─────────┬──────────┘    └──────────┬───────────────┘    │
│            │                           │                    │
│            └─────────┬─────────────────┘                    │
│                      │                                      │
│  ┌───────────────────▼───────────────────────────────┐    │
│  │              Shared Infrastructure                  │    │
│  │  - Overlay (P2P, chain_id tagged messages)         │    │
│  │  - io_context + JobQueue                           │    │
│  │  - ValidatorKeys (same key, dual signing)          │    │
│  │  - TimeKeeper (NTP)                                │    │
│  │  - Config                                          │    │
│  └────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

---

## Key Design Decisions

| Decision | Choice | Why |
|----------|--------|-----|
| Network layer | Single overlay, chain_id tagged messages | Avoid Polkadot's doubled-connections pain |
| Storage | Separate NodeStore DBs | Clean rollback, independent I/O tuning |
| State root | Pseudo-tx (voting) + outer header field (verification) | UNLReport pattern for consensus + O(1) lookup |
| Validator keys | Same key signs both chains | Simpler ops, chain-specific payload prevents replay |
| Inner protocol | Compiled-in (not plugins) | Deterministic consensus requires identical code |
| Finality | Inner fast (soft) + Outer confirms (hard) | Fast trading UX with settlement guarantees |

---

## Core Components to Build

### 1. InnerChainEngine (owns the inner consensus loop)
- Location: `src/xrpld/inner/InnerChainEngine.h`
- Owns: `InnerAdaptor`, `Consensus<InnerAdaptor>`, `InnerLedgerMaster`, heartbeat timer
- Instantiated by `ApplicationImpl` when `[inner_chain]` config present
- Amendment-gated: `featureInnerChain`

### 2. InnerAdaptor (satisfies Consensus<T> template)
- Mirrors `RCLConsensus::Adaptor` interface
- Key differences: own ledger type, own tx set type, faster consensus params, routes to InnerLedgerMaster
- Shares: overlay (tagged messages), validator keys, io_context

### 3. Network Message Routing
- Add `optional uint32 chain_id` to TMProposeSet, TMValidation, TMTransaction in `xrpl.proto`
- `PeerImp` routes chain_id=1 messages to `InnerChainEngine`
- Peer handshake includes `Inner-Chain: <network_id>` header for capability discovery
- Inner messages only broadcast to inner-capable peers

### 4. State Root Commitment
- `InnerStateRoot` pseudo-transaction on outer chain (like UNLReport)
- Outer validators who also run inner chain vote on state root every N inner ledgers
- Outer header field `sfInnerStateRoot` for O(1) verification
- Checkpoint/rollback: inner chain reverts to last outer-confirmed state if rejected

### 5. Import/Export Bridge (reuses existing spec)
- **Deposit (outer→inner)**: Outer vault payment → inner ImportCredit pseudo-tx → inner mint
- **Withdrawal (inner→outer)**: Inner Export burn → validator multisig → outer vault release → ExportConfirm
- **Simplification**: Both chains in same process = no XPop/WebSocket needed, direct ledger access
- Reuses: ExportSignatureCollector, ExportVaultState, UNLReport, ticket management

### 6. Inner Chain Protocol
- Own binary codec (separate from outer SField/STObject system)
- Own transaction types: OptionPairCreate, OptionCreate, OptionSettle, MarginAccountSet, etc.
- Own state entries: OptionPair, Option, MarginAccount, InsuranceVault
- Same crypto primitives (secp256k1/ed25519, SHA-512Half, SHAMap)

---

## Config Structure

```ini
[inner_chain]
enabled=1
network_id=1001
ledger_close_ms=1000
state_root_interval=10

[inner_validator_list_keys]
ED2677ABCEE0FC2B...

[inner_node_db]
type=NuDB
path=/var/lib/xrpld/inner_db

[inner_import_vault_address]
rInnerVault...
```

---

## Phased Implementation

### Phase 1: Inner Consensus Skeleton (4-6 weeks)
Two `Consensus<T>` instances in one binary, inner producing empty ledgers.
- [ ] `InnerAdaptor` implementing Consensus template interface
- [ ] `InnerCxLedger`, `InnerCxTx`, `InnerTxSet` types
- [ ] `InnerLedgerMaster` (stores inner ledger chain)
- [ ] `InnerChainEngine` class (owns adaptor + consensus + timer)
- [ ] `[inner_chain]` config parsing
- [ ] `chain_id` field in protobuf messages
- [ ] Message routing in PeerImp
- [ ] Inner chain produces empty ledgers on 1s cadence

### Phase 2: Dual Signing & Inner UNL (2-3 weeks)
Same validator participates in both consensus loops.
- [ ] Second ValidatorList instance for inner chain
- [ ] Wire ValidatorKeys into InnerAdaptor::propose/validate
- [ ] Inner chain peer discovery (handshake header)
- [ ] Inner UNL tracking and quorum
- [ ] Multi-node test: N nodes running dual consensus

### Phase 3: State Root Commitment (3-4 weeks)
Inner state roots anchored in outer ledger.
- [ ] `InnerStateRoot` pseudo-tx on outer chain
- [ ] `featureInnerChain` amendment gate
- [ ] Outer validators vote on inner state root
- [ ] Inner chain checkpoint tracking
- [ ] Rollback mechanism (revert to last checkpoint)
- [ ] `sfInnerStateRoot` in outer ledger header

### Phase 4: Import/Export Bridge (3-4 weeks)
Value moves between outer and inner chains.
- [ ] Port ExportSignatureCollector from options-sidechain branch
- [ ] Deposit flow: outer vault → inner ImportCredit → mint
- [ ] Withdrawal flow: inner Export → multisig → outer release
- [ ] ExportConfirm pseudo-tx for completion
- [ ] Vault state management (tickets)
- [ ] RPC: inner_deposit_status, inner_withdraw_status

### Phase 5: Options Protocol on Inner Chain (6-8 weeks)
Full options/derivatives trading.
- [ ] Inner chain binary codec and transaction format
- [ ] InnerTransactor framework
- [ ] OptionPairCreate, OptionCreate, OptionSettle, OptionLiquidate
- [ ] MarginAccountSet, MarginDeposit, MarginWithdraw
- [ ] InsuranceVault mechanics
- [ ] Oracle price feed from outer chain
- [ ] Funding rate pseudo-tx

### Phase 6: Production Hardening (4-6 weeks)
- [ ] Metrics (Prometheus/StatsD)
- [ ] Admin RPC: inner_chain_info, inner_ledger, inner_account_info
- [ ] Graceful degradation (inner failure doesn't crash outer)
- [ ] Inner chain catch-up from peers
- [ ] Load testing
- [ ] Fuzz testing inner transactors

---

## Critical Files

| Component | Path |
|-----------|------|
| Generic consensus template | `src/xrpld/consensus/Consensus.h` |
| RCL adaptor (reference) | `src/xrpld/app/consensus/RCLConsensus.h` |
| NetworkOPs (heartbeat/timer) | `src/xrpld/app/misc/NetworkOPs.cpp` |
| Application lifecycle | `src/xrpld/app/main/Application.cpp` |
| Protobuf messages | `include/xrpl/proto/xrpl.proto` |
| ValidatorList | `src/xrpld/app/misc/ValidatorList.h` |
| Overlay/PeerImp | `src/xrpld/overlay/detail/PeerImp.h` |
| Import/Export spec | `transia/options-sidechain` branch |

---

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| Inner bug crashes outer | Exception boundary in InnerChainEngine; graceful degradation |
| Timer contention | Inner uses separate strand; job queue prioritizes outer |
| Memory bloat | Inner prunes aggressively (256 ledgers); separate DB |
| Network bandwidth | Inner has smaller UNL = fewer messages; existing compression |
| Rollback complexity | Simple: restore checkpoint, discard everything after |
| Operator confusion | Clear config/log/RPC separation (`inner_` prefix) |

---

## Verification Plan

1. **Phase 1 test**: Start 4-node testnet with `[inner_chain]` enabled; verify inner ledgers advance independently of outer ledgers; check logs show both consensus phases cycling
2. **Phase 2 test**: Verify TMProposeSet/TMValidation messages with chain_id=1 propagate only to inner-capable peers; verify same validator key produces valid signatures for both chains
3. **Phase 3 test**: Verify InnerStateRoot pseudo-tx appears in outer validated ledgers at configured interval; trigger rollback by stopping majority of inner validators and verify inner chain reverts
4. **Phase 4 test**: Deposit XRP from outer to inner account; verify balance on inner; withdraw back to outer; verify round-trip preserves value
5. **Phase 5 test**: Create option pair, trade options, settle at expiry; verify P&L reflected in inner balances; withdraw profits to outer chain
