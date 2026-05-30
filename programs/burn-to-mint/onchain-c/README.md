# RATi Burn-To-Mint C SBF Program

Status: v1 candidate
Date: 2026-05-25
Scope: first-class on-chain burn-to-mint executor for the disposable and
release-candidate paths.

## Why C

The C SBF program is the active v1 candidate because it completed the disposable
devnet burn-to-mint proof under the default compute budget and with a smaller
artifact than the Rust/Pinocchio shell.

Current measured baseline:

- C v1 candidate: about 40.5 KB.
- Rust/Pinocchio reference shell: about 57.0 KB.
- Minimal C deserializer floor: about 1.4 KB.

## Build

```sh
npm run build:sbf
```

The default output is:

```text
programs/burn-to-mint/onchain-c/target/deploy/rati_burn_to_mint_c.so
```

The Rust/Pinocchio shell is kept as a reference implementation and can be built
with:

```sh
npm run build:sbf:rust-reference
```

## Release Gates

Before this program can be used for release-candidate addresses:

- `npm run check` passes.
- `npm run check:burn-vectors` proves the generated native vectors, account
  rule vectors, disposable proof vector, and C review artifact still agree.
- `npm run check:c-rent` proves the fallback rent formula is still the one
  documented in `RENT_STRATEGY.md`.
- `npm run size:sbf` records the C artifact, Rust reference artifact, and C
  floor.
- `node scripts/check-sbf-size-gate.mjs` accepts the C artifact.
- Disposable migration evidence validates on devnet without a Compute Budget
  instruction.
- Every setup and migration transaction records compute units from
  `solana confirm -v`.
- The rent strategy is reviewed against the target cluster. The current C
  program uses the default rent-exempt lamports-per-byte formula because the
  active C SDK headers do not expose a rent sysvar helper. See
  `RENT_STRATEGY.md`.
- Golden vectors exist for instruction bytes, account layouts, expected errors,
  quote outputs, counter updates, account rules, fee-surface rejection, and the
  disposable devnet proof.

## Source Boundaries

- `src/rati_burn_to_mint.c` owns the SBF entrypoint, account checks, PDA
  checks, system-program CPI, and SPL Token checked burn/mint CPI.
- `../native` remains the compact Rust spec/reference for codecs, layouts,
  quotes, and test-vector generation.
- `../onchain` remains a Rust/Pinocchio reference shell only. It is not the
  default SBF artifact.

Do not add UI, registry publishing, metadata upload, indexer, launchpad, or
Trebuchet concerns to this program. Those are off-chain surfaces.
