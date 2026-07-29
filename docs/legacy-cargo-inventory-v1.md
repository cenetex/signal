# Legacy cargo inventory (bounded #676 slice)

This slice removes one unsafe migration shortcut without claiming the full
v78 re-identification design.

On a resumed world, a valid save checksum is treated only as byte-integrity
evidence. Startup no longer calls
`world_anchor_validated_legacy_cargo_origins()` and therefore does not append
new V1 `RECIPE_LEGACY_MIGRATE` CRAFT events over legacy rows whose origin,
semantics, custody, and consumption were never proven. A bounded, read-only
inventory is printed instead. Existing cargo-trust callers continue to reject
rows with no verified origin, so those rows remain inert.

Fresh-world behavior is intentionally different. Server-generated starter
stock still crosses the explicit genesis bootstrap and receives the signed
station-local origin needed by the current game loop.

## Inventory contract

The scanner examines at most 65,536 cargo rows in deterministic holder order.
It reports aggregate counts only; it never prints cargo or actor identities,
receipt bytes, paths, session tokens, or station secrets. Reaching the cap sets
`truncated=true` and the stable `scan_limit_reached` reason. Unexamined cargo
does not gain trust.

Stable holder codes cover:

1. station manifests;
2. loaded player ships;
3. NPC ships;
4. dormant/destroyed ship-asset snapshots;
5. cargo-pod manifest rows;
6. cargo-pod shell frames; and
7. active delivery-shipment cargo.

An assigned ship asset serializes the authoritative live player/NPC ship, so
that alias is counted once under the live holder instead of being
misclassified as a duplicate identity.

Stable additive reason codes cover explicit re-identification required,
malformed unit metadata, duplicate identity, receipt-sidecar mismatch,
receipt-sidecar absence, unresolved stable custodian, invalid holder bounds,
and scan-limit exhaustion. Every legacy/migrate row receives the first reason;
additional reasons may overlap. An aligned but empty manifest receipt slot is
reported as absent: this read-only API has no verified station-log evidence
with which to upgrade absence into proof. A non-empty sidecar is valid only
when the composed station evaluator verifies its signatures, chain links,
pinned origin event, authority lifecycle, and cargo metadata; raw signature
validity alone is not treated as provenance.

Cargo pods have no resident receipt sidecar. A normal pod row is therefore
reported with `receipt_sidecar_absent`. Active delivery cargo is intentionally
serialized both in the delivery envelope and in its physical player-towed pod,
or in an NPC ship manifest. The scanner collapses only an exact, unique
shipment/holder materialization:

- A pod alias must match shipment ID, status, quantity and offset, commodity,
  full cargo-unit bytes, cargo public key, and the corresponding external
  shipment chain slot. The chain is still classified independently, so absent
  or invalid evidence remains visible on both rows. Its aggregate is
  `delivery_pod_aliases`.
- An NPC alias additionally requires a unique stable NPC debtor, a one-to-one
  manifest-row match, and byte-identical aligned receipt chains; chain
  validity/absence is still reported independently. Its aggregate is
  `delivery_npc_ship_aliases`.

A conflicting binding, ambiguous match, or any third logical copy still
receives `duplicate_identity`.

## Deliberate limits

- Player save files that have not been authenticated and loaded are not
  directory-enumerated. Loaded player ships are covered. A later transactional
  migration needs to operate inside the authenticated player-save load/commit
  boundary.
- Construction and scaffold state currently retain no `cargo_unit_t` or
  receipt sidecar after input consumption: construction commits retain signed
  event references to consumed public keys, not resident units. Scaffold
  structs have no cargo-unit storage, scaffold save/load is absent, and current
  world load clears scaffolds. They are therefore audit history, not omitted
  resident holders.
- Raw-ore floats and physical asteroid fragments are not manifest cargo and are
  outside this inventory.
- Candidate discovery deliberately recognizes resident
  `RECIPE_LEGACY_MIGRATE` rows only. It does not yet correlate arbitrary cargo
  identities against verified V0 SMELT/CRAFT history. That requires a bounded
  verified-chain index and remains an uncovered #676 migration input.
- The report does not create a replacement identity, sign a migration record,
  preserve/link legacy evidence, start a new receipt chain, or write a
  quarantine row. Those operations require the complete transactional schema
  specified by #676.
