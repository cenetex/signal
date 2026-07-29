# CRAFT V1 provenance truthfulness

`CHAIN_EVT_CRAFT` semantics version 1 is a station attestation. It is not an
input-lineage or conservation proof.

The signed V1 payload binds:

- the recipe identifier and exact active input-pubkey list;
- distinct, non-zero active input identifiers and zero unused slots;
- output kind, commodity, grade, quantity, and pubkey;
- the sorted input-pubkey Merkle root;
- one uniquely recoverable output index for the frozen V1 recipe shape; and
- the station authority, event ordering, payload hash, and chain linkage when
  the containing log has been verified.

It does not prove that an input pubkey existed in prior verified history, had
the recipe-required commodity or grade, was in station custody, or was
consumed once. A station can sign arbitrary distinct non-zero input bytes.
Consequently every current surface uses these verdicts:

| Verdict | Meaning |
| --- | --- |
| `station_attested_v1` | The containing event and canonical V1 payload were verified. Input lineage and conservation remain unproven. |
| `structural_v1_unverified` | The V1 bytes are canonical, but the caller skipped or failed full event verification. This is not an attestation. |
| `unbound_v0` | Frozen V0 bytes are audit-readable but omit bound output semantics. This is not cargo trust. |
| `reject_*` | The payload is malformed or noncanonical and cannot create a provenance source. |

Both `input_lineage_proven` and `conservation_proven` are always `false` for
V0 and V1. `RECIPE_LEGACY_MIGRATE` V1 is also only station-attested: its
origin salt is absent from CRAFT bytes, so the output identity cannot be
recomputed from the event.

## Fail-closed V1 validation

`server/cargo_craft_provenance.c` is the bounded interpreter shared by live
transform/trust code and the offline tools. It rejects the wrong payload
length, unknown semantics versions or recipes, wrong input count, zero or
duplicate active inputs, non-zero unused slots, invalid output labels or
quantity, and an output pubkey that does not resolve to exactly one output
index. The frozen V1 recipe shapes are compared with the live catalog in
tests so a future recipe change must introduce a new semantics version rather
than reinterpret historical V1 bytes.

`signal_verify`, `signal_rati_receipt`, and path-backed live origin indexing
copy each opened log once into a bounded anonymous snapshot before
verification and interpretation. The cap is 64 MiB. This makes the two passes
observe the same bytes despite pathname replacement, in-place writes, or
concurrent append; snapshot allocation, I/O, or cap exhaustion fails closed.

Player-facing and offline lineage may display V1 input identifiers and
locally observed parent events, but labels them as station-attested edges.
Walking such a graph is not evidence of custody or consumption.

## Still required for CRAFT V2

This P0 slice intentionally does not define or emit CRAFT V2. Issue #679
remains open until a versioned V2 schema and one shared verdict implement:

- an exact origin event, authority, semantic cargo record, and custody or
  receipt reference for every input;
- current, rotated, and federated authority policy with world/chain
  boundaries and proof-before-craft ordering;
- exact recipe commodity multiset and minimum-grade derivation from verified
  inputs;
- durable, transactional input consumption and output emission;
- replay resistance across recipe slots, events, restarts, rotations, and
  federation imports;
- explicit output index and exact output identity recomputation;
- stable diagnostics for missing, ambiguous, relabelled, untrusted, revoked,
  malformed, duplicate, and replayed proofs;
- bounded cross-authority lookup, cache invalidation, replacement-race, and
  soak coverage; and
- the complete semantic, reserved-byte, authority, custody, ordering, replay,
  and transaction fault-injection matrix.
