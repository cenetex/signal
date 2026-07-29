# Contract and delivery ownership

Issue #668 is being migrated in bounded slices. Contract claimants,
delivery-shipment debtors, and cargo-pod player tow custody now use durable
actors instead of recyclable runtime slots.

## Authoritative representation

`contract_t.claimed_by_principal` and
`delivery_shipment_t.debtor_principal` are authoritative. The retained
`claimed_by` and `debtor_player` fields are compatibility projections rebuilt
from the live actor resolver; they are not authorization inputs and are not
written by save version 81 or later.

Only canonical `PLAYER` and `NPC` principals may own these rows. `NONE` means
an open contract, but only when its quarantine binding is also zero.
`STATION`, `SYSTEM`, `UNATTRIBUTED`, malformed principals, duplicate live
identities, and quarantined legacy rows fail closed.

Player principals require a finalized, challenge-verified public key. Slot
reuse, token rotation, reconnect, grace-period movement, and offline saves do
not change the principal. An ownership mutation is staged on a copy first and
committed only after every operation that can fail has completed.

NPC principals currently use a domain-separated SHA-256 hash of the persisted
eight-byte NPC session token. This is a bounded compatibility principal for
the existing save model, not a collision-resistant actor birth identity.
Slot-aware authorization proves token uniqueness with at most
`MAX_NPC_SHIPS - 1` raw-token comparisons and derives one hash; the generic
resolver's bounded hash scan is reserved for lookup from an unknown principal.
Replacing this compatibility identity with a dedicated, immutable NPC actor
ID remains required.

## Save version 81

Version 81 writes, for every contract and delivery shipment:

- the canonical actor principal; and
- a one-time ownership-quarantine record ID.

Runtime slot projections are rebuilt after load and after a player-slot
transfer.

For pre-v81 saves:

- an open contract remains open;
- a player slot is unproven and becomes a bound quarantine row;
- an NPC pool code is canonicalized only when that saved NPC slot still has a
  unique non-zero persisted token;
- malformed, missing, or otherwise ambiguous actor codes become bound
  quarantine rows; and
- inactive rows are cleared instead of retaining stale ownership.

A quarantined row cannot be reclaimed by a future occupant of the same slot.
The migration is covered by an exact v80 test writer rather than by relabeling
current bytes as an older format.

## Save version 82: cargo-pod tow custody

`cargo_pod_t.tow_owner_principal` is authoritative for player-held pods.
`cargo_pod_t.tractor` and `ship_t.towed_pods` remain live physics/wire
projections. A verified player attach records the exact `PLAYER` principal;
proof finalization upgrades a pod already held by that same live anonymous
session. `UNATTRIBUTED` custody is otherwise live-session-only and is never
rebound after restart.

Version 82 writes the canonical tow principal and its one-time quarantine
record binding instead of `towed_by`. On load, an offline owner remains
unprojected. An exact verified reconnect resolves the principal to its current
slot and rebuilds the tow link. A different actor occupying the saved numeric
slot cannot attach, PRESENT, or route the pod through a station module.

For v81 and older saves, a raw player `towed_by` slot has no stable proof. The
loader preserves the pod contents, custody, geometry, and hardpoint, clears the
live player projection, and creates a bound
`cargo_pod_tractor/legacy_slot_unproven` quarantine row. Conflicting legacy
player and module tags are quarantined as conflicting authority. Quarantined
pods remain inert to player and module attachment; operator diagnostics expose
only the record ID, source, reason, pod index, and legacy slot—not bearer
tokens or reconnect secrets.

Explicit live release, station handoff, pod consumption, or physical
destruction clears current player tow custody. Link expiry and ship-slot
teardown clear only the live projection so genuine restart/reconnect recovery
can still resolve the durable principal.

## Fulfillment boundary

Authoritative player and NPC paths now require an open contract or the same
canonical claimant before they can:

- claim or advertise ordinary work;
- pick up, deliver, black-market sell, or clear a linked delivery shipment;
- deliver a towed raw fragment;
- deliver receipt-backed ship cargo;
- transfer ordinary NPC haul cargo for a contract; or
- fracture a contract target.

Receipt-backed player delivery preflights the ledger, receipt append, source
removal, and destination insertion before the append. After a successful
append, ownership, cargo stores, ledger state, and contract progress are
committed with allocation-free swaps. A foreign, quarantined, malformed, or
unverified actor cannot mutate contract-specific state through these paths.

The marked starter-refit work order is intentionally public and unclaimable.
Players may discover and contribute to it while it is open; NPCs and the
ordinary pod, fragment, and haul paths skip it. Its completion credit rule is
separate from durable ownership.

## Remaining #668 inventory

The parent issue remains open. The following durable or grace-sensitive slot
fields still need their own actor-principal migration and compatibility
projection policy:

- `station_t.planned_owner`;
- `station_t.pending_scaffolds[].owner`;
- `station_t.placement_plans[].owner`;
- asteroid `last_towed_by` and `last_fractured_by` attribution (stable-token
  companions exist, but the dedicated migration is unresolved);
- `ship_asset_t.operator_slot` load/rebind refinement (the canonical asset
  owner already exists);
- transient `scaffold_t.owner` if its lifetime is extended across reconnect
  grace or persistence;
- reserved fracture, thrown-rock, tow, and outpost-founder quarantine tags
  that have not yet been promoted to a complete durable-actor model; and
- a dedicated collision-resistant NPC actor birth ID to replace the
  session-token compatibility hash.
