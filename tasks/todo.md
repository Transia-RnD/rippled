# XLS-0103 follow-ups

## Close cross-implementation seed-derivation divergence (Python publisher)

**Context.** XLS-0103 §3.17.2 / §8.9: the three implementations expand a 16-byte XRPL seed into the ML-DSA-44 keygen DRBG seed differently.

| Stack | Path | SHA-512 bytes taken | Status |
|---|---|---|---|
| `xrpld` server | `generateSecretKey(KeyType::dilithium, seed)` → `sha512HalfS` | first **32** | ✓ canonical |
| `xrpl.js` (`dilithium` branch) | `ripple-keypairs/.../dilithium/index.ts` → `Sha512.half(entropy)` | first **32** | ✓ matches server |
| `xrpld-publisher` TypeScript | uses `@transia/ripple-keypairs` (above) | first **32** | ✓ matches server |
| `xrpld-publisher` Python | `xrpld_publisher/dilithium.py::sha512_48` → `ML_DSA_44.set_drbg_seed` | first **48** | ✗ diverges |

If the Python module is ever used to generate a publisher key, that key cannot be re-derived from the same XRPL seed by rippled or by the TS tooling. The user is on the TS publisher today, so this is not a live operational issue — but it MUST be closed before XLS-0103 reaches `Final`.

### Tasks

- [ ] **Patch `py/xrpld_publisher/dilithium.py`.** Change `sha512_48` (first 48 bytes of SHA-512) to `sha512_half` (first 32 bytes). Update `derive_keypair` to call `ML_DSA_44.set_drbg_seed(sha512_half(entropy))`. Verify the call accepts a 32-byte seed; if `dilithium-py`'s DRBG strictly requires 48 bytes of input, switch derivation strategy (e.g. SHAKE-256(seed, 48)) — but the resulting *node identity* MUST still match what the server derives from the same XRPL seed.

- [ ] **Add a known-answer-test fixture.** Generate `tests/vectors/dilithium.json` from rippled (`xrpld-quantum`) using a fixed XRPL seed (e.g. `"masterpassphrase"`-derived). Record `seed_entropy_hex`, `public_key_hex`, `node_public_base58`, `account_id_base58`, `message_hex`, `signature_hex`.

- [ ] **Wire the fixture as a conformance test in all three implementations.**
  - `xrpld`: extend `src/test/protocol/SecretKey_test.cpp` to load and assert against the JSON.
  - `xrpl.js` (`dilithium` branch): extend `packages/ripple-keypairs/test/api.test.ts` with the same vectors.
  - `xrpld-publisher` Python: extend `py/tests/unit/test_dilithium.py` to load the JSON and assert keypair + signature match. This is the test that catches the `sha512_48` regression.
  - `xrpld-publisher` TS: extend `ts/test/unit/deterministic.test.ts` to load the same JSON.

- [ ] **Remove the §3.17.2 parenthetical caveat from XLS-0103** (the note that says "the reference Python implementation uses the first 48 bytes…") once the Python patch lands.

- [ ] **Resolve §8.9 in XLS-0103 to a single sentence** ("All three implementations derive the ML-DSA-44 DRBG seed as `sha512Half(seed)`, the first 32 bytes of SHA-512.") once the patch and fixture are in place. The security-considerations divergence note can then be dropped entirely.

### Acceptance

- The same XRPL seed produces the same `(public_key_hex, account_id_base58, node_public_base58)` across all four code paths.
- `tests/vectors/dilithium.json` is checked into the `xrpld` repo and consumed by tests in all three downstream repos.
- XLS-0103 §3.17.2 reads as a single canonical rule with no implementation caveats.
