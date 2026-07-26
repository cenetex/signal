# Authoritative state digest

`signal.authoritative_state.v1` is the canonical SHA-256 commitment for a
post-tick Signal simulation state. Peers may compare the root only when they
are at the same committed tick and the same ordered-input frontier.

The implementation is `signal_authoritative_state_digest()` in
`server/state_digest.c`. It packs every included value explicitly in
little-endian order, hashes IEEE-754 float bits exactly, and never hashes a C
struct, pointer, enum representation, padding byte, or allocation capacity.

## Coverage contract

| Domain | Included authority | Deliberate exclusions |
| --- | --- | --- |
| World clock and scheduling | RNG, belt seed, world sequence, tick/time, spawn/gravity/NPC/frontier timers, frontier counters and decisions, monotonic IDs, player-only mode | Regression telemetry counters |
| Stations | Public identity and authored text, geometry/dynamics, prices, inventories/residue, services/factions, modules and production progress, ledgers, pending construction/builds/plans, policy decisions, cargo plus receipt chains, repair cadence, chain tail, knowledge and decision-driving HNN memory | `station_secret`, chain-log verification health, dirty flags, stored-hull summary, padding, flow diagnostics, HNN retrieval diagnostics |
| Ships and hull assets | Generation-safe component slots; complete physics, upgrades, tow projections, cargo, stats, knowledge; asset ownership/custody/provenance and stored-ship state when the asset is actually stored | Component pointers and the stale `stored_ship` snapshot of an assigned asset |
| Player controllers | Live/grace player membership, identity, hull reference, semantic input, pending movement commands by apply tick/sequence, docking, mining/tow action state, autopilot/teacher state, freshness IDs, damage attribution, pubkey proof state and signed-action nonce | Connections, replication baselines, arrival timestamps, analytics, ACK/result snapshots |
| NPC controllers | Role/state, hull reference, semantic input, route/job state, identity and decision-driving HNN memory | Inspect/job diagnostics, display tint, pointers and HNN retrieval diagnostics |
| Physical world | Active asteroids and origin metadata, fracture quorum state, destroyed-rock ledger, scaffolds, cargo pods and exact payloads, live tow links, all generation counters/liveness bits, ship-birth assemblies and character registry | Network dirty bits, pod summary projections, inactive entity payload bytes |
| Economy and trust | Contracts, delivery debt/settlement records with exact cargo and receipt chains, pubkey registry, handoff replay guard | Operator-local files and secrets |
| Signal channel | Logical message order, IDs, timestamps, sender, bounded text/audio, entry hashes and chain tail | Unused ring-buffer slots |

The following `world_t` members are intentionally outside consensus:

- `connections`, `replications`, `pending_resolves`, `events`, and
  `interactions` are transport or current-tick output;
- `belt`, `asteroid_grid`, and `signal_cache` are rebuilt projections;
- `signal_field` and `signal_field_decay_tick` are explicitly documented as
  local advisory memory, not authority;
- `hopper_smelt_events` and `hopper_smelt_units` are regression telemetry.

If an excluded field begins influencing a future simulation transition,
settlement decision, authorization decision, or persistent public state, the
digest schema must change before that behavior ships.

## Versioning rule

The schema string and numeric version are part of the hash preimage and are
emitted beside replay roots. Any change to field inclusion, ordering,
normalization, numeric encoding, or entity ordering requires a new schema
constant and version. Adding a field to `world_t` or to an included nested
type therefore requires one of:

1. explicitly encode it and bump the schema, or
2. document why it is derived, transport-only, secret, diagnostic, or
   otherwise outside consensus.

The legacy replay `prefix_state_hash` and `state_hash` remain diagnostic
compatibility fields. `prefix_state_root` and `state_root` are the versioned
peer/quorum commitments.

## Regression gates

`tests/c/test_state_digest.c` verifies:

- stable schema/version reporting and repeatability;
- root sensitivity across global, station, ship, asteroid, tow, pod,
  delivery, HNN, signal-channel, ordered-input, and identity domains;
- root invariance for transport caches, secrets, retry queues, derived pod
  summaries, local signal memory, and diagnostics.

Native/WASM replay comparison is the cross-runner encoding gate: matching
JSONL rows now also require matching versioned `prefix_state_root` and
`state_root`.
