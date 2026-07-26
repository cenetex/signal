# Cargo receipt trust

Signal separates three questions that are easy to blur:

1. **Witness chain:** are the portable custody receipts correctly signed,
   cargo-bound, and linked?
2. **Origin:** does the oldest receipt pin an actual `SMELT` or `CRAFT` event
   that produced this cargo identity?
3. **Seal policy:** does the evaluating station currently accept the producing
   authority, accept it as a rotated historical key, or classify it as unknown,
   untrusted, or revoked?

A valid witness chain alone does not prove production origin or local trust.
Station policy is local: a lawful station can reject an unknown issuer while a
black market accepts it. Revoked issuers are rejected everywhere.

## Standalone verification

`signal_receipt_verify` consumes a binary receipt chain made by concatenating
canonical 208-byte `cargo_receipt_t` records in chronological order. The
origin proof and authority lifecycle decision are explicit inputs:

```sh
./build/signal_receipt_verify \
  --report=json \
  --cargo-pub=<cargo-identity-hex> \
  --origin-event=smelt \
  --origin-hash=<producing-event-header-sha256> \
  --origin-authority=<station-pubkey-hex-or-base58> \
  --origin-event-id=42 \
  --origin-epoch=12000 \
  --authority-trust=current \
  receipt-chain.bin
```

Use `--origin-event=missing` when history is unavailable; do not synthesize an
origin hash. Accepted authority values are `current`, `rotated`, `unknown`,
`untrusted`, and `revoked`.

Text output leads with semantic copy such as `accepted/trusted`,
`rejected/no-origin`, or `rejected/revoked`. JSON output uses the stable
`signal.cargo_receipt_trust.v1` schema and includes:

- `accepted`, the final policy result;
- stable numeric and string verdict, chain, origin-event, and authority fields;
- an `audit` object retaining cargo identity, exact origin hash and authority,
  event coordinates, first receipt authority, and receipt-head hash.

Exit status is 0 for an accepted proof, 1 for a semantic rejection, and 2 for
malformed arguments or binary input.

The CLI, cargo inspection HUD, gameplay gates, and settlement import all call
the same `cargo_receipt_trust_verify()` verdict contract. The short semantic
label also comes from the shared receipt layer, so audit and gameplay wording
cannot silently drift apart.
