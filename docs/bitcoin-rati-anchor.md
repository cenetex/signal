# Bitcoin-Anchored RATi Mining

**Status:** design draft / gap analysis
**Audience:** Signal, RATi, and bridge/verification implementers

## Thesis

RATi is mined by playing Signal, not by contributing Bitcoin hashpower. The
player mines, tows, and smelts physical fragments; Signal derives named cargo
identity from that play; stations sign the resulting history. Arweave stores the
full proof body. Bitcoin should only notarize compact roots over that proof.

The desired verifier sentence is:

> This RATi mint/receipt corresponds to this Signal fragment or ingot, signed by
> this station, stored in this Arweave checkpoint, and timestamped before this
> Bitcoin block.

That makes Bitcoin a timestamped spine for RATi provenance, not a storage layer
or a replacement for Arweave, Solana, station signatures, or Signal's native
settlement history.

## Existing Substrate

Signal already has most of the lower-level primitives:

- `shared/mining.h` derives fragment/callsign outcomes from deterministic
  fracture inputs, player identity material, and a burst nonce.
- `server/sim_asteroid.c` opens a fracture claim window, accepts bounded
  client claims, resolves the best grade, and writes `fragment_pub`.
- `shared/types.h` carries `cargo_unit_t` identity, `parent_merkle`, grade,
  prefix class, origin station, and mined block.
- `server/chain_log.h` defines signed station events for smelt, transfer,
  trade, rock destruction, fragment tow/release, death, construction, and route
  history.
- `docs/cargo-architecture.md` defines the important boundary: fragment identity
  exists in space, but named crate identity is born at smelt/craft time.
- `docs/settlement-event-model.md` describes the future checkpoint/state-root
  path that can be uploaded to Arweave and forward-applied by clients.
- `scripts/deploy-arweave.mjs` already produces Arweave manifest txids and
  manifests for permaweb deploys.
- `docs/packnft-architecture.md` and `docs/yield-split-design.md` already frame
  Signal as the producer of provable game-labor contract blocks, RATi as the
  identity/registry stamp, and downstream chain tooling as release machinery.

The Bitcoin anchor should sit above these, after Signal has produced a
station-signed proof bundle and after Arweave has stored the bundle.

## Proposed Proof Ladder

```
Player action
  -> fracture claim resolution
  -> fragment_pub
  -> smelt/craft into cargo_unit_t
  -> station-signed chain event(s)
  -> settlement segment/checkpoint root
  -> Arweave artifact and discovery manifest
  -> Bitcoin anchor root
  -> optional Solana/RATi mint or yield-split receipt
```

The bridge or mint receipt should point back into this ladder. Bitcoin does not
decide whether the event is valid; it proves the event bundle existed by a
given Bitcoin block and makes later history rewrites obvious.

## Artifact Model

### `rati_mining_receipt_v1`

One receipt describes one high-value RATi-relevant outcome: a RATi-grade
fragment, a named ingot, a commissioned strike, or a downstream RATi vessel
birth.

```json
{
  "version": "rati_mining_receipt_v1",
  "agent_pubkey": "<32-byte Signal/agent pubkey>",
  "wallet_pubkey": "<optional chain wallet pubkey>",
  "station_pubkey": "<32-byte station pubkey>",
  "world_id": 0,
  "world_seq": 0,
  "build_id": "<short build hash>",
  "event": {
    "kind": "smelt_ingot",
    "event_id": 42,
    "event_hash": "<sha256>",
    "payload_hash": "<sha256>",
    "signature": "<ed25519 signature>"
  },
  "mining": {
    "fragment_pub": "<32-byte hash>",
    "cargo_pub": "<32-byte hash>",
    "parent_merkle": "<32-byte hash>",
    "grade": "RATi",
    "prefix_class": "INGOT_PREFIX_RATI",
    "mined_tick": 123456
  },
  "arweave": {
    "segment_tx": "<arweave txid>",
    "checkpoint_tx": "<arweave txid>",
    "manifest_tx": "<arweave txid>"
  },
  "bitcoin": {
    "batch_root": "<sha256>",
    "anchor_txid": "<optional txid>",
    "block_height": 0,
    "block_hash": "<optional block hash>"
  }
}
```

This should be canonicalized before hashing. Use sorted keys, fixed encodings,
and a domain separator:

```
receipt_hash = sha256("SIGNAL:RATI:RECEIPT:v1" || canonical_receipt_bytes)
```

### `rati_anchor_batch_v1`

Receipts are batched by station and epoch. The batch is the unit committed to
Bitcoin.

```json
{
  "version": "rati_anchor_batch_v1",
  "station_pubkey": "<32-byte station pubkey or all-stations marker>",
  "epoch_start_tick": 120000,
  "epoch_end_tick": 180000,
  "receipt_count": 18,
  "receipt_merkle_root": "<sha256>",
  "settlement_checkpoint_root": "<sha256>",
  "arweave_manifest_tx": "<arweave txid>",
  "previous_batch_root": "<sha256 or zero>",
  "created_at_unix": 0
}
```

Then:

```
bitcoin_anchor_root =
  sha256("SIGNAL:RATI:BTC-ANCHOR:v1" || canonical_batch_bytes)
```

The Bitcoin transaction carries only a short marker plus this 32-byte root.
Everything else stays on Arweave.

## Bitcoin Commitment Path

There are two implementation modes:

1. **OpenTimestamps first.** Stamp the batch file or `bitcoin_anchor_root`,
   upload the `.ots` proof to Arweave, and later upload an upgraded proof after
   Bitcoin confirmation. This is the lowest-friction path and does not require
   wallet or UTXO management.

2. **Direct OP_RETURN later.** Create a Bitcoin transaction with a small
   `OP_RETURN` payload such as:

   ```
   53494752 01 <32-byte-anchor-root>
   ```

   where `53494752` is `SIGR` and `01` is the payload version. Keep the payload
   small even if relay policy allows larger data. Bitcoin should see Signal as a
   timestamping user, not a data publisher.

Direct Bitcoin anchoring should batch roots. Per-receipt anchoring is usually
too expensive, noisy, and operationally brittle.

## How This Ties To RATi Mining

RATi mining becomes a two-stage finality model:

1. **Play finality:** the station observes the mining/smelt event, validates
   the local game rules, signs the chain-log event, and can immediately produce
   a receipt or bridge claim.

2. **Audit finality:** the receipt is included in an Arweave-hosted batch whose
   root is timestamped on Bitcoin. After this point, the public can prove that
   the receipt existed by a specific Bitcoin block.

This avoids putting Bitcoin in the latency-sensitive game loop while still
giving high-value RATi events a hard public time boundary.

For Solana or other minting adapters, this yields three possible policies:

- **Immediate mint:** mint from station/RATi stamp proof; Bitcoin is delayed
  audit evidence.
- **Delayed high-value mint:** common flows mint immediately, but RATi,
  commissioned, vessel-birth, or yield-bearing assets require an anchored batch.
- **Challenge window:** mint immediately but allow a short dispute period until
  the Bitcoin anchor lands; failed verification marks the receipt as slashed or
  revoked in the registry.

The first policy is simplest. The second is probably right for scarce RATi
identity and yield-bearing artifacts.

## Gap Analysis

| Area | Current state | Needed for Bitcoin-anchored RATi | Risk |
|------|---------------|-----------------------------------|------|
| RATi event vocabulary | RATi-grade mining now resolves through `CHAIN_EVT_CLAIM_FRAGMENT` followed by `CHAIN_EVT_SMELT`; `/mine.html` copy was updated to match. | Keep receipt, bridge, and settlement docs aligned around claim+smelt rather than a separate `CHAIN_EVT_MINE_RATI`. | Low. The main risk is future docs drifting back to a fake-specific event name. |
| Fracture proof persistence | `CHAIN_EVT_CLAIM_FRAGMENT` now persists `fracture_seed`, `fragment_pub`, claimant pubkey, burst nonce/cap, grade, and fracture id for fresh logs. Older logs may still only have downstream smelt identity. | Extend migration/backfill policy if old high-value receipts need independent recomputation rather than station attestation. | Medium. Fresh receipts can verify; historical receipts may remain weaker. |
| Agent identity binding | Fracture claims currently derive a player pubkey from session token in the claim path; separate code handles persistent pubkey proof. | Bind RATi-grade receipts to verified player/agent identity, not ephemeral session identity, before bridge/mint eligibility. | High. "Anyone can mine for any agent" needs a precise signed-agent model. |
| Canonical receipt format | `signal_rati_receipt` now emits `rati_mining_receipt_v1` JSON from verified station logs, including claim-backed grade verification for fresh logs. | Freeze the schema, add compatibility fixtures, and decide whether canonical JSON stays the long-term wire format or becomes a binary envelope. | Medium. Bad canonicalization creates unrepeatable hashes. |
| Settlement checkpoints | `docs/settlement-event-model.md` is draft; canonical segment roots and forward-apply checkpointing are not yet implemented. | Implement segment roots, state roots, and discovery manifests for mining/smelt/receipt state. | High. Bitcoin should anchor checkpoint roots, not ad hoc logs forever. |
| Arweave proof storage | Static site deployment to Arweave exists. Settlement/proof artifact upload is planned but not implemented as a general anchor service. | Add a proof artifact upload path separate from site deploys: receipts, batches, segment files, checkpoint files, and anchor receipts. | Medium. Site deploy manifests and settlement manifests should not be conflated. |
| Bitcoin tooling | `build-rati-anchor-batch` now builds deterministic Merkle batches and OP_RETURN-ready payloads. `stamp-rati-anchor` validates the batch and wraps the OpenTimestamps CLI into a proof manifest. Direct Bitcoin RPC broadcast and anchor verification are still missing. | Add `verify-rati-anchor` tooling and decide whether direct OP_RETURN broadcast is needed after OTS. | Medium. OTS first keeps ops simple. |
| Solana bridge semantics | Burn-to-mint program docs mostly describe source-token burn/migration; `/mine.html` describes game-labor minting from chain-log proof. | Reconcile whether RATi mining mints via a bridge, a registry stamp plus contract block, or a separate receipt program. | High. Token issuance policy must be unambiguous before mainnet. |
| On-chain verification | Solana programs cannot cheaply verify Arweave or Bitcoin inclusion. | Keep Bitcoin/Arweave verification off-chain in stamp/release tools, or use delayed registry states that point to anchored batches. | Medium. Do not pretend Bitcoin proof is available inside SBF unless explicitly bridged. |
| Duplicate prevention | Cargo identity and chain event hashes exist, but bridge-level spent-event tracking is not specified. | Track consumed `event_hash` / `cargo_pub` / receipt hash in the mint adapter or registry. | High. This is the core anti-double-mint invariant. |
| Anchor receipt lifecycle | Arweave releases are immutable, but Bitcoin txid/block data is only known after broadcast/confirmation. | Publish a later `bitcoin_anchor_receipt_v1` artifact and update a mutable discovery index or KV pointer. | Low. Simple if designed up front. |
| Fee/cadence policy | No Bitcoin fee budget or anchoring cadence exists. | Define batch cadence: per station epoch, per N RATi receipts, daily, or high-value immediate batch. | Medium. Bad cadence either costs too much or weakens finality. |
| Verifier UX | `signal_verify` exists for chain logs, but not for full RATi -> Arweave -> Bitcoin receipts. | Extend or add a CLI that verifies from Solana tx/receipt to Signal event to Arweave bytes to Bitcoin proof. | Medium. The story only works if third parties can run it. |
| Reorg and confirmation policy | Not defined. | Define confirmation depth for audit finality and how receipts display pending/confirmed/expired. | Low. Easy, but should be explicit. |
| Privacy and doxxing | Receipts may bind wallet, session, station, and world history. | Allow agent pubkey and wallet pubkey separation; decide which fields are public in batch leaves versus private proofs. | Medium. RATi wants portability without unnecessary identity leakage. |

## Suggested Implementation Wedges

### Wedge 1: Off-chain Receipt Builder

Create a local tool that reads a chain log and emits a canonical
`rati_mining_receipt_v1` for one smelted RATi/named ingot. It should verify the
station signature and include enough references for a future Arweave upload.

No Bitcoin or Solana dependency yet.

Initial implementation: `tools/signal_rati_receipt.c` builds the
`signal_rati_receipt` CLI. It verifies the station log with
`chain_log_verify_with_pubkey`, then emits JSON receipts for `CHAIN_EVT_SMELT`
events. It supports `--event-id`, `--segment-id`, `--cargo-pub`, and
`--min-prefix=RATi` filters. Fresh logs include `CHAIN_EVT_CLAIM_FRAGMENT`, so
the receipt builder can recompute `fragment_pub` and mining grade; older logs
without that event still emit receipts with `grade_verified: false`.

### Wedge 2: Batch Root and OTS Proof

Create a batch from one or more receipts, compute the Merkle root and
`bitcoin_anchor_root`, stamp it with OpenTimestamps, and write:

- `rati-anchor-batch.json`
- `rati-anchor-batch.ots`
- `rati-anchor-batch.manifest.json`

Upload those files to Arweave once the proof service exists.

Initial implementation: `scripts/build-rati-anchor-batch.mjs` reads one or more
receipt files, rejects receipts without `grade_verified` unless explicitly
given `--allow-unverified`, sorts by `receipt_hash`, writes
`rati_anchor_batch_v1`, and exposes the `bitcoin_anchor_root` plus an
OP_RETURN-ready payload (`SIGR` + version + root). `make rati-anchor-batch`
wraps the builder, and `scripts/test-rati-anchor-batch.mjs` checks deterministic
batch output and the unverified-receipt guard.

The next layer is also present: `scripts/stamp-rati-anchor.mjs` validates a batch
before invoking `ots stamp`, writes the `.ots` proof, and emits
`signal.rati_anchor_stamp_manifest.v1` with batch, proof, and Arweave upload
metadata. `--dry-run` writes the manifest without claiming a timestamp, which is
useful on hosts without the OpenTimestamps CLI.

### Wedge 3: Full Verifier

Extend `signal_verify` or add a companion verifier:

```
verify-rati-anchor receipt.json
  -> verify canonical hash
  -> verify station event signature
  -> verify receipt inclusion in batch
  -> verify batch bytes from Arweave
  -> verify OTS or Bitcoin OP_RETURN proof
```

### Wedge 4: Bridge Policy

Decide whether game-labor RATi minting is:

1. a mode of the existing burn-to-mint program,
2. a separate receipt/mint program,
3. a stamped contract-block release flow, or
4. delayed registry authorization consumed by Trebuchet/packnft.

Until that is resolved, Bitcoin anchoring should be treated as audit evidence,
not mint authority.

### Wedge 5: Direct Bitcoin OP_RETURN

After OTS and verifier flow are boring, add direct Bitcoin anchoring through a
descriptor wallet or external PSBT signer. This should not be required for the
first public proof demo.

## Open Decisions

1. How should old receipts without `CHAIN_EVT_CLAIM_FRAGMENT` be treated:
   station-attested only, backfilled from replay, or excluded from high-value
   Bitcoin-anchored issuance?
2. Should external verifiers recompute the grade from fracture preimages, or
   trust station/quorum signatures over resolved `fragment_pub`?
3. Is Bitcoin audit finality required before minting scarce RATi artifacts, or
   only after minting as public evidence?
4. What is the first public artifact: a mined RATi receipt, a yield-split ingot,
   or a RATi vessel-birth identity?
5. Does the bridge bind to Signal agent pubkeys, Solana wallets, or a separate
   RATi namespace key that can map to either?
6. What batching cadence makes the product feel real without spending fees as
   theater?

## Near-Term Recommendation

Start with OTS-backed batch roots over Arweave-hosted receipts. Do not direct
broadcast Bitcoin transactions until receipt format, verifier behavior, and
bridge policy are settled.

The first useful milestone is a public command:

```
verify-rati-anchor <receipt-or-solana-tx>
```

It should end with a human-readable proof:

```
RATi receipt verified.
Signal event: CHAIN_EVT_SMELT #42, station signature valid.
Cargo: RATi ingot <callsign>, parent fragment <fragment_pub>.
Arweave: checkpoint <txid>.
Bitcoin: timestamped by block <height>.
```

That is the bridge between the fantasy and the substrate: the player mined a
thing, the station remembered it, Arweave preserved it, and Bitcoin made its
place in time hard to erase.
