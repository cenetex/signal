# C Rent Strategy

Status: approved low-cost fallback, target-cluster check required
Date: 2026-05-25

The active C SBF program creates the config, destination-config, and
source-config PDA accounts on chain. The current Solana C SDK headers available
to this repo do not expose a rent sysvar helper, so the C candidate uses the
same default rent-exempt formula that devnet currently reports for these small
accounts:

```text
lamports = (space + 128) * 6960
```

Current spaces and formula results:

- config: 116 bytes -> 1,698,240 lamports.
- destination config: 188 bytes -> 2,199,360 lamports.
- source config: 150 bytes -> 1,934,880 lamports.

This is the approved low-cost fallback while the current SDK lacks a rent sysvar
helper. It avoids adding another account to setup instructions and avoids
manual sysvar parsing in the SBF artifact.

Before each RC freeze, the target cluster must still match the formula. If a
future Solana C SDK exposes a compact rent sysvar helper, replace this fallback
and remeasure SBF size and setup compute before promoting it.

The deterministic local gate is:

```sh
npm run check:c-rent
```

The RC comparison gate is:

```sh
npm run check:c-rent -- --rpc https://api.devnet.solana.com
```
