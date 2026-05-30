# RATi Burn-To-Mint Release Checklist

Status: draft v0.1
Date: 2026-05-21

## Current Milestone: Disposable Devnet Prototype

- Root repo scope is captured in `../../SCOPE.md`.
- `npm run check` passes.
- On-chain executor implements migration through checked SPL Token burn/mint CPIs
  and post-CPI counter updates.
- `onchain-c/` is the default SBF program path for v1.
- Rust/Pinocchio `onchain/` is reference-only.
- Setup instructions create config, destination config, and source config PDA
  accounts through the system program.
- Default low-cost SBF rejects `create_receipt`; receipt-enabled builds are
  measured separately before use.
- Default migration proof does not use a Compute Budget instruction or raised
  compute-unit limit.
- `programs/burn-to-mint/SBF_OPTIMIZATION.md` records the active byte, rent,
  compute, and modularity gates.
- `npm run plan:disposable-migration` produces a disposable-only command,
  account-order, instruction-byte, and evidence plan.
- `npm run client:disposable-migration` can dry-run the custom setup and
  migrate transactions.
- A real disposable devnet transaction is captured before any devnet success
  claim.
- Disposable source and destination mints are used; final public addresses are
  not used.
- One fixed-ratio Ruby-style migration succeeds on devnet.
- Source and destination counters match token balances after migration.
- Extra fee, fee-recipient, fee-switch, or treasury accounts remain rejected.
- Ruby production source `ruby-pump-current` remains disabled.
- Evidence is captured before moving to final address selection.

## Before Final Address Selection

- Program spec reviewed.
- PDA seeds reviewed.
- Account layouts reviewed.
- Dependency allowlist approved.
- Native core checks pass through `npm run check`.
- On-chain shell checks pass through `npm run check`.
- `npm run build:sbf` passes before any devnet release candidate.
- Receipt builds, if selected, are built explicitly with `receipts` and compared
  against the low-cost default artifact.
- `npm run size:sbf -- --rpc https://api.devnet.solana.com` records SBF bytes,
  loader write count, and ProgramData rent before each release candidate.
- SBF artifact size is at or below the v1 target in
  `SBF_OPTIMIZATION.md`, or the release-candidate freeze is blocked for review.
- Per-instruction compute units are recorded from `solana confirm -v`; default
  setup and migration transactions must fit under the default budget.
- C vectors are generated under `onchain-c/vectors/`, and
  `npm run check:burn-vectors` passes, or the release candidate is blocked. The
  gate covers instruction/account bytes, account rules, fee-surface rejection,
  default receipt rejection, and disposable proof evidence.
- `npm run check:c-safety` passes, or the release candidate is blocked. The
  gate checks C hot-path ordering, exact account count rejection, checked SPL
  Token CPIs, PDA mint signing, and post-CPI counter writes.
- `npm run check:c-negative-runtime` emits the replay plan for disposable C
  runtime negative cases. If RC review requires live replay, run
  `node scripts/c-negative-runtime-harness.mjs --mode send ...` against the
  initialized disposable deployment and validate the resulting evidence with
  `--mode evidence`.
- The C rent strategy uses the approved low-cost fallback while the current SDK
  lacks a compact rent helper. Before RC freeze, run `npm run check:c-rent -- --rpc
  https://api.devnet.solana.com` against the target cluster.
- Disposable migration evidence validates with
  `npm run plan:disposable-migration -- --mode evidence --evidence <PATH>`.
- Disposable devnet deployment passes.
- Ruby source mint verification remains current.
- Per-token mint specs are complete except PDA mint addresses, nonces, and bumps.

## Before Devnet Release-Candidate

- Final program ID public key selected.
- RATi, Kyro, and Ruby PDA mint public keys, nonces, and bumps selected.
- Address manifest updated with public keys.
- Cloud ceremony or offline key ceremony path selected.
- If using cloud ceremony, `npm run kms:solana-signer -- --mode public-key`
  records the KMS-derived program ID, `npm run plan:kms-loader-v3-deploy`
  emits the deploy plan, and signer dry-run evidence is captured for every
  reviewed transaction plan.
- Launch-evidence schema and operator path are reviewed.
- Launch evidence validates with `npm run check:launch-evidence -- --evidence
  <PATH>`.
- Launch-evidence fixtures validate with `npm run check:launch-evidence-fixtures`
  so operators can compare shape before RC.
- The promotion gate passes with `npm run check:release-readiness -- --phase
  devnet-release-candidate --rpc https://api.devnet.solana.com --evidence
  <PATH>`.
- Metadata URIs selected through `../../deployment/PERMAWEB_DEPLOYMENT.md`.
- `npm run plan:permaweb` passes.
- Fee payer selected for devnet.
- Launch multisig or release-candidate authority selected.
- `npm run check` passes.

## Before Mainnet

- Devnet release-candidate passed.
- Program binary hash approved.
- Authority handoff plan approved.
- Launch-evidence path approved for mainnet.
- Permaweb metadata transaction IDs and content hashes recorded.
- Incident response owner selected.
- Mainnet fee payer selected and funded.
- Final program ID keypair temporary copy prepared through key ceremony, or
  KMS signer evidence proves no program ID keypair file exists.
- No private key material exists in the repo tree.

## After Mainnet

- Manifest has mainnet proof fields.
- Registry points to canonical mints.
- Ruby migration source remains disabled until activation review.
- Temporary keypair copies destroyed.
- Release artifact records source commit, registry hash, program hash, and
  authority state.
- Sanitized launch-evidence hash is published or recorded with the release
  artifact.
