# RATi Burn-To-Mint Native Core

This crate is the first implementation wedge for the RATi burn-to-mint program.
It is dependency-free Rust that defines the fixed account layouts, instruction
codec, and migration accounting rules before any Solana runtime wrapper is
added.

It is not yet a deployable SBF program. The next layer should wrap this core in
a native Solana or Pinocchio entrypoint and perform account ownership, signer,
PDA, SPL Token CPI, and rent checks.

Run:

```sh
npm run check:program
```

Current covered rules:

- manual byte parsing;
- instruction pack/unpack round trips;
- dependency-free core data model;
- no unsafe code;
- fixed-ratio migration quoting;
- bonding-curve migration quoting;
- source and destination mint matching;
- proof-only sources cannot quote fungible mint output;
- source and destination enabled/finalized checks;
- slippage ceiling;
- min destination amount;
- max supply cap;
- counter updates;
- receipt serialization.
