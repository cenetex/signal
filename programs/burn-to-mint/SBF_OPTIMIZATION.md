# RATi Burn-To-Mint SBF Optimization Plan

Status: draft v0.1
Date: 2026-05-25
Scope: size, compute, and modularity gates for the burn-to-mint program.

## Position

Do not make the migration proof depend on a raised compute-unit limit. The v1
program must pass the disposable migration path under the default transaction
budget, with no Compute Budget instruction in the proof transaction.

The program should stay small, modular, and composable:

- `native/` remains the audited Rust core for instruction layouts, account
  layouts, quoting, counters, deterministic packing, and golden-vector
  generation.
- `onchain-c/` is the active v1 SBF candidate.
- `onchain/` with Pinocchio remains a Rust reference shell only.
- `research/sbf-size-lab/c/` keeps the minimal C floor and measurement harness,
  not the v1 program source.

## Current Measurements

Recorded with:

```sh
npm run size:sbf -- --rpc https://api.devnet.solana.com
```

Current artifacts:

- Rust/Pinocchio shell: 56,960 bytes, 64 loader chunks at 900 bytes.
- C v1 candidate: 39,824 bytes, 45 loader chunks.
- Minimal C deserializer floor: 1,400 bytes, 2 loader chunks.

Rent deltas from the latest devnet RPC measurement:

- Rust/Pinocchio to C v1 candidate: 17,136 bytes and 119,266,560 lamports.
- Rust/Pinocchio to minimal C floor: 55,560 bytes and 386,697,600 lamports.

## Disposable Devnet Proof

The C v1 candidate completed the disposable devnet burn-to-mint flow without
raising compute limits.

Evidence:

```sh
npm run plan:disposable-migration -- --mode evidence --evidence target/disposable-migration/evidence.json
```

Result:

- source burned: 1,000,000 base units;
- destination minted: 1,000,000 base units;
- counters matched token balance deltas.

Default compute-unit consumption:

- `InitializeConfig`: 7,645 CUs.
- `RegisterDestinationMint`: 17,769 CUs.
- `RegisterSourceMint`: 10,656 CUs.
- `SetSourceEnabled`: 5,449 CUs.
- `Migrate`: 18,931 CUs.

The proof artifact is intentionally disposable:

- program ID: `2qSDLNZjhjugAQSkQ2TerqTTqRYfw8kWkiFw2v6EBR9B`;
- source mint: `55hJfYWKMt6R6sGCtJSkz1dpRPe7BKTzRK19WLvU8kcf`;
- destination mint: `5FcJYr4S3o5cwLnGhyJjwqQFMT79rrnYsNp2T8ogybfB`;
- evidence path: `target/disposable-migration/evidence.json`.

Do not promote those addresses.

## Production Direction

Preferred path: promote `onchain-c/` as the v1 on-chain program while keeping
`native/` as the compact Rust spec and vector generator.

The C program still needs:

- generated golden vectors from `native/`;
- direct rent sysvar support or an explicitly approved rent-account strategy;
- local validator and devnet e2e gates in release automation;
- focused C safety review for bounds checks, pointer writes, owners, signers,
  writable accounts, PDA checks, and CPI account ordering.

## Size Gates

For v1:

- target SBF artifact: at or below 45,000 bytes;
- hard review stop: above 50,000 bytes;
- receipt writing stays out unless it fits the same target and default compute
  budget;
- any new dependency must show byte, loader chunk, and CU impact in
  `target/sbf-size-lab/report.json`.

## Compute Gates

The disposable proof must:

- use no Compute Budget instruction;
- keep every setup and migration instruction below the default 200,000 CU
  transaction budget;
- record CU consumption from `solana confirm -v`;
- fail promotion if optimization depends on splitting one logical instruction
  only to hide excess compute.

## Modularity Gates

Keep optional behavior out of the hot migration path:

- receipt writing is a separate measured feature, not part of the default path;
- metadata/permaweb/indexer work stays outside the migration program;
- launch UI, Trebuchet tooling, registry publishing, and evidence generation
  must treat the on-chain program as a small verifier/executor, not an app
  backend.
