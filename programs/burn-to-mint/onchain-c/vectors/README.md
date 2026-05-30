# Golden Vectors

Status: required before release candidate

This directory stores generated burn-to-mint vectors consumed by the C program
review and release gates.

Regenerate from the native Rust spec:

```sh
npm run generate:burn-vectors
```

Check that the committed vectors still match the native source, the C candidate
constants, the C account-rule arrays, and the disposable proof transcript:

```sh
npm run check:burn-vectors
```

Generated vector files:

- `golden-vectors.v1.json`: instruction codec, account layout, quote, counter,
  and selected core-error vectors;
- `account-rules.v1.json`: fixed account order, signer/writable requirements,
  account-count failure policy, and forbidden fee-surface labels;
- `disposable-devnet-proof.v1.json`: disposable devnet proof transaction ids,
  default compute usage, account counts, and burn/mint result.

The vectors are generated from `../native`; do not hand-edit them from the C
program. The account-rule vector mirrors `../onchain/ACCOUNT_ORDER.md` and is
also consumed by `scripts/disposable-migration-plan.mjs`.
