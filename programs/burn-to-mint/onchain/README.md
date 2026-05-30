# RATi Burn-To-Mint On-Chain Shell

This crate is the Solana SBF-facing shell for the RATi burn-to-mint program.

It is intentionally separate from `../native`:

- `native/` owns deterministic byte layouts, instruction encoding, quoting,
  counter updates, and receipt serialization with no external dependencies.
- `onchain/` owns Solana account order, signer checks, PDA ownership checks,
  rent checks, and SPL Token CPI calls.

Current status: reference executor, not the optimized release-candidate path.
The crate decodes instructions through the native core, enforces fixed account
counts, checks required signers and writable accounts, rejects any attempt to
append fee-related accounts, and writes the program-owned native layouts for
config, destination mint, source mint, pause/finalize, source-enable, and
authority-transfer/retirement instructions.
Initialize/register setup instructions create their program-owned PDA state
accounts through the system program when needed. Destination registration also
stores a nonzero token id hash and verifies the destination mint is initialized,
owned by the declared token program, uses the declared decimals, has the
mint-authority PDA, and has no freeze authority.

The default low-cost migration path now validates source/destination
relationships, token program ownership, user token-account mint/owner fields,
mint decimals, and the native quote. It then executes checked SPL Token
burn/mint CPIs and updates counters only after both CPIs return success. A
separate receipt-writing path remains explicitly deferred until receipt account
creation and rent handling are added.

For the active size and compute direction, see
`../SBF_OPTIMIZATION.md`. The optimized proof path must run under the default
compute budget rather than relying on a Compute Budget instruction.

Build checks:

```sh
cargo check --manifest-path programs/burn-to-mint/onchain/Cargo.toml --locked
cargo test --manifest-path programs/burn-to-mint/onchain/Cargo.toml --locked
npm run build:sbf
```

Account order is fixed in `ACCOUNT_ORDER.md`.
