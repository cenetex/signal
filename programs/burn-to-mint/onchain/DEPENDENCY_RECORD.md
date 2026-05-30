# RATi Burn-To-Mint On-Chain Dependency Record

Status: draft v0.1
Date: 2026-05-21
Component: `programs/burn-to-mint/onchain`

## Direct Dependencies

### pinocchio

- Version: `=0.10.0`
- Default features: disabled
- Enabled features: none
- License: Apache-2.0
- Purpose: minimal Solana SBF entrypoint, account view, address, and program
  error surface without Anchor or Solana SDK framework code.
- Why local code is worse: hand-writing the SVM entrypoint deserializer would
  duplicate loader ABI code and increase audit risk.
- Maintenance status: Kyro-compatible Pinocchio line. This is intentionally
  older than the current Pinocchio line because installed Solana SBF tooling
  `2.3.13` uses rustc `1.84.1`, while Pinocchio `0.11.1` requires rustc
  `1.89.0`.
- Known advisories: none recorded in this repo.
- Audit impact: small but nonzero; freeze exact version before devnet release
  candidate and record `cargo tree --locked`.
- Removal plan: replace only if a direct raw SVM entrypoint proves smaller and
  easier to audit than Pinocchio after a measured proof of concept.

### rati-burn-to-mint-core

- Version: local path dependency `../native`
- Default features: none
- Enabled features: none
- License: repo-local unpublished crate
- Purpose: shared dependency-free account layout, instruction codec, quote, and
  migration accounting rules.
- Why local code is worse: duplicating byte layouts and math inside the SBF
  shell risks consensus drift between tested core logic and deployed logic.
- Maintenance status: first-party RATi crate.
- Known advisories: not applicable.
- Audit impact: in scope for program audit.
- Removal plan: none; this is the intended audited core boundary.

### solana-define-syscall

- Version: `=4.0.1`
- Default features: disabled
- Enabled features: `unstable-static-syscalls`
- Target: `cfg(any(target_os = "solana", target_arch = "bpf"))`
- License: Apache-2.0
- Purpose: supplies the syscall definitions needed for the direct CPI and PDA
  syscalls used by the migration path.
- Why local code is worse: this crate owns the syscall hash lowering expected
  by the Solana SBPF toolchain; reproducing it locally would add audit risk.
- Maintenance status: already present transitively through Pinocchio.
- Known advisories: none recorded in this repo.
- Audit impact: low; direct target dependency stays limited to the SBF-facing
  shell, but target-feature behavior must be validated against the active
  Solana verifier before relying on static syscall lowering.
- Removal plan: remove only if the raw shell or toolchain no longer needs these
  syscall definitions and `npm run build:sbf` remains verifier-compatible for
  CPI/PDA syscalls.

## Current Dependency Firewall

- No Anchor.
- No Serde/Borsh.
- No async/runtime/network/database crates.
- No default Pinocchio features.
- No `pinocchio-token` dependency; SPL Token checked CPI payloads are encoded
  locally to avoid pulling the newer Pinocchio line into the Rust 1.84 SBF
  toolchain.
- No fee or treasury dependency surface.
- `Cargo.lock` must be committed; check/test gates must use `--locked`.
- The default SBF build is now the C v1 candidate. The Rust/Pinocchio shell is
  built through `npm run build:sbf:rust-reference`.
- `npm run size:sbf` records the Rust/Pinocchio reference shell size against
  the C v1 candidate and a minimal C deserializer floor before dependency or
  entrypoint rewrites.

## Locked Dependency Tree

Recorded with:

```sh
cargo tree --manifest-path programs/burn-to-mint/onchain/Cargo.toml --locked
```

```text
rati-burn-to-mint-onchain v0.1.0
├── pinocchio v0.10.0
│   ├── solana-account-view v1.0.0
│   │   ├── solana-address v2.0.0
│   │   │   └── solana-program-error v3.0.1
│   │   └── solana-program-error v3.0.1
│   ├── solana-address v2.0.0
│   └── solana-program-error v3.0.1
└── rati-burn-to-mint-core v0.1.0

SBPF target-only direct dependency recorded in `Cargo.lock`:

solana-define-syscall v4.0.1
```
