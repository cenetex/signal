# RATi Burn-To-Mint Program Spec

Status: draft v0.1
Date: 2026-05-21
Scope: native Solana program responsible for source-token burn and canonical
RATi, Kyro, and Ruby minting.

## Design Rules

- Native Rust, no Anchor.
- Small dependency allowlist from `../../DEPENDENCY_POLICY.md`.
- Manual account and instruction parsing.
- Integer-only arithmetic.
- No holder snapshots.
- No protocol fee, fee recipient, fee switch, treasury rake, or hidden spread.
- User pays only pass-through Solana costs: transaction fees, rent exemption,
  and optional priority fees.
- No per-wallet or per-source hard cap unless a future registry record changes
  policy.
- Source pricing is explicit per source: fixed-ratio for official v0 migrations
  such as `ruby-pump-current`, bonding-curve only for routes that declare it.
- Program PDA owns destination mint authority.
- RATi, Kyro, and Ruby mint accounts are created at vanity PDA addresses derived
  from the final burn-to-mint program ID.
- Program does not own dashboards, CCG logic, avatar runtime logic, or bridges.

## Accounts

### Config

Global program state.

Fields:

- schema version;
- admin or governance authority;
- pause authority;
- launch status;
- registered destination mint count;
- registered source mint count;
- pending authority and pending-authority flag for two-step admin handoff;
- total minted counters per destination token;
- bump seeds for derived authorities.

### Destination Token Config

One account per canonical destination mint.

Fields:

- destination mint;
- mint PDA vanity nonce and bump;
- token id hash;
- decimals;
- token program;
- mint authority PDA;
- bonding curve parameters;
- status: planned, candidate, enabled, paused, finalized.

### Source Mint Config

One account per accepted source mint.

Fields:

- source mint;
- destination mint;
- source token program;
- source decimals;
- migration mode;
- fixed ratio or bonding curve selector;
- fixed-ratio numerator and denominator when the selector is fixed-ratio;
- enabled flag;
- finalized flag;
- burned base units;
- minted destination base units.

No source config may contain a protocol-fee recipient or protocol-fee basis
points field. The fee switch is intentionally absent from the account layout.

### Receipt

Optional per-migration receipt account. The default low-cost SBF artifact does
not compile receipt creation; build the on-chain crate with the `receipts`
feature only when receipt rent and extra deployment bytes are acceptable.

Fields:

- user wallet;
- source mint;
- destination mint;
- source amount burned;
- destination amount minted;
- reserved slot field, written as `0` by the low-cost executor;
- user nonce;
- receipt bump.

Receipts are audit records, not holder snapshots.

## Instructions

The first implementation wedge lives in `native/`. It defines fixed byte
layouts, instruction decoding, fixed-ratio and bonding-curve quotes, migration
counter updates, and receipt serialization without external dependencies.

The active Solana-facing v1 candidate lives in `onchain-c/`. It implements the
fixed account-count and account-role tables, signer checks, writable-account
checks, PDA checks, system-program PDA account creation, SPL Token checked
burn/mint CPIs, and explicit rejection of extra fee/treasury accounts. The
Rust/Pinocchio shell in `onchain/` is retained as a reference implementation,
not the default SBF artifact. Setup instructions create their own program-owned
PDA state accounts through the system program when those accounts do not
already exist.
Destination registration stores the supplied nonzero token id hash and validates
the destination mint's SPL Token owner, initialized flag, decimals, mint
authority PDA, and null freeze authority before writing config.

### InitializeConfig

Creates global config PDA and records authority policy.

Required checks:

- config PDA matches `PDA_SEEDS.md`;
- program creates the config PDA with rent-exempt lamports when needed;
- payer is not the program ID keypair or any authority;
- authority account is explicit.

### RegisterDestinationMint

Adds RATi, Kyro, or Ruby destination mint.

Required checks:

- signer is admin/governance authority;
- program creates the destination config PDA with rent-exempt lamports when
  needed;
- instruction carries a nonzero token id hash that is stored in destination
  config;
- mint account is initialized and owned by the supplied token program;
- mint decimals match token spec;
- mint authority is the program PDA or is transferred during setup;
- freeze authority is `null` before enabling.

Canonical release preflight must also prove the mint account matches the
manifest and, for final RATi/Kyro/Ruby addresses, is the expected PDA from
`PDA_SEEDS.md`.

### RegisterSourceMint

Adds a source mint such as the Ruby High Pump mint.

Required checks:

- signer is admin/governance authority;
- program creates the source config PDA with rent-exempt lamports when needed;
- source mint matches registry;
- destination mint is already registered;
- source decimals match registry;
- holder snapshots are not used;
- source is disabled until explicitly enabled.

### EnableSourceMint

Activates a source mint for migration.

Required checks:

- source verification is complete off-chain and reflected in registry;
- destination mint is enabled;
- exchange rate parameters are final;
- program is not paused.

### BurnToMint

Burns source tokens and mints destination tokens.

Required checks:

- source config is enabled;
- destination config is enabled;
- user signs;
- source token account belongs to user;
- source token account mint matches config;
- destination token account mint matches config;
- source burn amount is nonzero;
- computed destination amount is nonzero;
- protocol fee is always zero and no fee recipient account is accepted;
- fixed-ratio sources use the configured normalized ratio exactly;
- bonding-curve sources use checked integer arithmetic and the configured
  curve parameters;
- slippage/min-output check passes;
- all arithmetic uses checked integer operations.

Effects:

- CPI burn source token;
- CPI mint destination token through mint authority PDA;
- update counters;
- optionally create receipt.

Current executor boundary:

- source/destination config ownership and relationship checks are implemented;
- source and destination token program checks are implemented;
- user source/destination token-account mint and owner checks are implemented;
- source and destination mint decimal checks are implemented;
- config-level paused/finalized status rejects migration;
- native quote calculation is implemented;
- checked SPL Token burn and mint CPIs are implemented for migration, and counters
  are written only after both CPIs return success;
- default low-cost builds reject `create_receipt`;
- receipt-feature builds implement receipt account creation and on-chain receipt
  serialization with PDA validation.

### Pause

Pauses migration entry points.

Required checks:

- pause authority signs;
- pause cannot change balances, rates, or authorities.

### FinalizeSourceMint

Closes a source migration path.

Required checks:

- admin/governance authority signs;
- source cannot be re-enabled without a new registry version;
- final counters are recorded.

### TransferAuthorityBegin / TransferAuthorityAccept

Transfers admin authority with a two-step handoff.

Required checks:

- current admin signs the begin instruction;
- begin instruction's pending authority matches the supplied pending-authority
  account;
- pending authority signs the accept instruction;
- current admin account still matches config at accept time;
- pending authority is cleared after accept.

## Ruby High Source

Initial verified source mint:

```text
ABHQGzXNoRbJ1sjUsCJ2TmTAo1uMx4EUpV1qYiSVpump
```

Ruby source migration starts disabled until the destination Ruby mint and
burn-to-mint program are both deployed and proof-complete. Its registry rate
mode is fixed-ratio `1:1` after decimal normalization, not bonding-curve.

## Out Of Scope

- Bridging.
- Pack opening.
- Orb burn/reveal mechanics.
- Avatar runtime changes.
- Holder snapshots.
- Direct treasury policy.
- Protocol-fee collection.
