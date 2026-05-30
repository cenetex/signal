# C Program Preparedness Checklist

Status: draft v0.1
Date: 2026-05-25

## Required Before Devnet Release Candidate

- C source is the only default SBF build path.
- Rust/native remains the spec and golden-vector source.
- Rust/Pinocchio remains reference-only.
- No raw Rust shell is part of the default build, check, or size report.
- Disposable devnet evidence must be rerun after any C artifact hash change
  before release-candidate promotion.
- No proof transaction includes a Compute Budget instruction.
- SBF size is below the v1 target in `../SBF_OPTIMIZATION.md`.
- Generated vectors pass with `npm run check:burn-vectors`.
- Account-order, signer/writable, account-count, extra-account fee-surface, and
  disposable transcript vectors are checked against the C source.
- The C safety gate passes with `npm run check:c-safety`; it keeps the hot
  migration path ordered as validate, quote, burn, mint, counters, writes.
- The negative-runtime replay plan generates with
  `npm run check:c-negative-runtime`; live send-mode evidence can be captured
  against disposable deployments when RC review asks for runtime failure
  transcripts. See `NEGATIVE_RUNTIME.md` for the latest disposable replay
  summary.
- The local rent formula gate passes with `npm run check:c-rent`.
- The target-cluster rent formula comparison passes with `npm run check:c-rent
  -- --rpc <TARGET_RPC>` before RC freeze.
- Launch evidence validates with `npm run check:launch-evidence -- --evidence
  <PATH>` before promotion.
- Release readiness passes with `npm run check:release-readiness -- --phase
  devnet-release-candidate --rpc <TARGET_RPC> --evidence <PATH>`.
- Account-length, owner, signer, writable, PDA, token-program, mint-authority,
  freeze-authority, and source/destination relationship checks are reviewed.

## Golden Vector Coverage

The `vectors/` files are generated and checked by `npm run check:burn-vectors`.

- packed instruction bytes for every instruction;
- serialized config, destination, source, and receipt account bytes;
- fixed-ratio quote output and counter deltas;
- selected expected core errors;
- expected account counts and writable/signer requirements;
- rejection of extra fee, treasury, fee-recipient, or fee-switch accounts;
- rejection of `create_receipt` in the default build;
- named negative review cases for wrong PDA, system program, token program,
  token-account relationship, mint decimals, mint authority, and freeze
  authority failures;
- disposable devnet transcript evidence for the captured proof artifact.

Remaining expansion before RC:

- address/PDA-specific negative transcript cases, if RC review wants replayable
  C runtime failure evidence beyond the current static gate and Rust reference
  tests.

## Manual Review Points

- Every pointer write is guarded by a length check.
- Every account write requires the transaction account to be writable.
- Every privileged state transition requires the expected signer.
- Authority retirement requires both current admin and current pause authority
  signers, then clears admin, pause authority, and pending authority.
- Every CPI uses the exact account order expected by the invoked program.
- PDA signing seeds include the bump as the final seed.
- Source burn and destination mint CPIs both succeed before counters are
  updated.
- The program contains no metadata, registry, launchpad, or off-chain workflow
  logic.
