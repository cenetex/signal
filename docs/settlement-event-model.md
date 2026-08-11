# Signal Settlement Event Model

This document defines the canonical settlement event types, binary
serialization, per-station segmenting rules, forward-apply semantics, and the
boundary between live sim state and settlement-owned state.

It is the design artifact for issue #350 and serves as the reference for #351
(forward-apply engine), #589 (WebRTC mesh), and #591 (Arweave anchor service).

## Status

Draft protocol. The existing dedicated server (`signal_server`),
`chain_event_header_t`, per-station signed log (Layer C of #479), and
`shared/settlement_engine.*` forward-apply substrate are implemented. This
document extends them toward P2P quorum signing and externally anchored
segments that can be verified without trusting one dedicated server.

## Design principles

1. **Settlement owns durable economic state.** Asset ownership, credit
   balances, production outputs, construction milestones, and signal-channel
   anchors live in settlement events. Transient gameplay — ship movement,
   towing physics, mining timing, combat — stays in the sim.

2. **Events are produced by the sim, signed by peers.** In P2P mode, every
   browser peer runs the deterministic sim and produces identical events.
   Any peer can sign. The chain log's `authority` field holds the station
   pubkey (derived from world seed) for single-peer events, or a quorum
   certificate hash for cross-shard events requiring ≥2/3 threshold.

3. **Per-station segmenting.** Each station maintains its own ordered event
   log. Cross-station events (cargo transfer between zones) appear as paired
   entries in both stations' logs, linked by cross-reference.

4. **Forward determinism, not replay-from-genesis.** A peer bootstraps from
   the most recent Arweave-anchored checkpoint, forward-applies later segments,
   and is caught up. Full replay from event zero is supported but not required.

5. **Binary format stability.** Event payloads are packed C structs with
   static-assert size guards. Schema changes bump a version byte. The wire
   format is the canonical representation; JSON is for debugging only.

6. **Content-addressed on Arweave.** Every event, segment, and checkpoint is
   uploaded to Arweave with its SHA-256 hash as the content address. The
   discovery manifest maps station pubkey → latest checkpoint txid.

## Event header

The existing `chain_event_header_t` (184 bytes) carries over with two additions:
a `quorum_root` field for P2P threshold signing, and a version byte extracted
from the existing padding.

```
typedef struct {
    uint64_t epoch;               /* sim tick when authored */
    uint64_t event_id;            /* monotonic per (station, epoch) */
    uint8_t  type;                /* settlement_event_type_t */
    uint8_t  version;             /* schema version (0 = v1 draft) */
    uint8_t  _pad[6];             /* MUST be zero */
    uint8_t  authority[32];       /* station pubkey (single-signer)
                                   * OR SHA-256 of quorum certificate */
    uint8_t  quorum_root[32];     /* 0 = single-signer (auth = station pubkey)
                                   * non-0 = quorum cert root; auth = hash */
    uint8_t  payload_hash[32];    /* SHA-256 of the payload bytes */
    uint8_t  prev_hash[32];       /* SHA-256 of the previous event header */
    uint8_t  signature[64];       /* Ed25519 over [epoch..prev_hash]
                                   * single-signer: station keypair
                                   * quorum: threshold cert over same span */
} settlement_event_header_t;      /* 216 bytes (+32 from quorum_root) */
```

The `quorum_root` field adds 32 bytes. When zero, the event is single-signed
by the station keypair (same as current federation mode). When non-zero, it
is the SHA-256 root of a quorum certificate containing ≥2/3 threshold
signatures from peers in the station's signal cone.

## Event types

### Asset lifecycle

| Type   | Name | Produces | Consumes | Cross-station |
|--------|------|----------|----------|---------------|
| `0x01` | `CLAIM_FRAGMENT` | `fragment_pub` ownership record | fracture claim resolution | No |
| `0x02` | `SMELT_INGOT` | `cargo_unit_t` in station manifest | `fragment_pub` (consumed) | No |
| `0x03` | `PRODUCE_OUTPUT` | `cargo_unit_t`(s) in station manifest | input `cargo_unit_t`s from manifest | No |
| `0x04` | `TRANSFER_CARGO` | receipt chain link | cargo unit removed from source manifest | Yes (paired) |
| `0x05` | `BURN_ASSET` | — (asset destroyed) | `cargo_unit_t` from manifest | No |

### Economy

| Type   | Name | Produces | Consumes | Cross-station |
|--------|------|----------|----------|---------------|
| `0x10` | `BUY_COMMODITY` | player manifest gains unit, ledger debit | station manifest loses unit | No |
| `0x11` | `SELL_COMMODITY` | station manifest gains unit, ledger credit | player manifest loses unit | No |
| `0x12` | `ISSUE_CREDIT_NOTE` | signed IOU in station ledger | — (station extends credit) | No |
| `0x13` | `REDEEM_CREDIT_NOTE` | — (IOU settled) | IOU marked redeemed | Yes (if cross-station) |

### Construction

| Type   | Name | Produces | Consumes | Cross-station |
|--------|------|----------|----------|---------------|
| `0x20` | `START_STATION_SITE` | planned outpost record | — | No |
| `0x21` | `DELIVER_CONSTRUCTION_INPUT` | scaffold build progress | input cargo from manifest | No |
| `0x22` | `COMPLETE_STATION_MODULE` | active module on station | scaffold + completed inputs | No |

### Infrastructure

| Type   | Name | Produces | Consumes | Cross-station |
|--------|------|----------|----------|---------------|
| `0x30` | `POST_SIGNAL_ANCHOR` | signal-channel hash record | — | No |
| `0x31` | `OPERATOR_POST` | persona-authored text | — | No |

### Player lifecycle

| Type   | Name | Produces | Consumes | Cross-station |
|--------|------|----------|----------|---------------|
| `0x40` | `PLAYER_DEATH` | death record (highscore data) | — | No |
| `0x41` | `FRAGMENT_TOW` | tow possession record | — | No |
| `0x42` | `FRAGMENT_RELEASE` | tow end record | tow possession | No |

### Segment metadata

| Type   | Name | Produces | Consumes |
|--------|------|----------|----------|
| `0xF0` | `SEGMENT_COMMIT` | checkpoint (state root + prev root) | all events in segment |

## Payload schemas

### CLAIM_FRAGMENT (0x01)

Binds a resolved fracture claim to a fragment pubkey.

```
typedef struct {
    uint8_t  fragment_pub[32];
    uint8_t  winner_pubkey[32];
    uint8_t  rock_pub[32];
    uint8_t  grade;
    uint8_t  _pad[7];
    uint64_t mined_block;
} settlement_payload_claim_fragment_t;  /* 112 bytes */
```

### SMELT_INGOT (0x02)

A fragment is consumed and a cargo unit is minted into the station manifest.
Byte-compatible with existing `chain_payload_smelt_t`.

```
typedef struct {
    uint8_t  fragment_pub[32];
    uint8_t  ingot_pub[32];
    uint8_t  prefix_class;
    uint8_t  _pad[7];
    uint64_t mined_block;
} settlement_payload_smelt_ingot_t;  /* 80 bytes */
```

### PRODUCE_OUTPUT (0x03)

Input cargo units consumed; output units minted. One event per production batch.

```
typedef struct {
    uint16_t recipe_id;
    uint8_t  input_count;
    uint8_t  output_count;
    uint8_t  _pad[4];
    uint8_t  output_pubs[4][32];
    uint8_t  input_pubs[4][32];
} settlement_payload_produce_output_t;  /* 264 bytes */
```

### TRANSFER_CARGO (0x04)

A cargo unit moves between holders. Cross-station transfers appear in both logs.

```
typedef struct {
    uint8_t  cargo_pub[32];
    uint8_t  from_pubkey[32];
    uint8_t  to_pubkey[32];
    uint8_t  kind;
    uint8_t  cross_ref_station[32];
    uint8_t  cross_ref_event_id[8];
    uint8_t  _pad[7];
} settlement_payload_transfer_cargo_t;  /* 144 bytes */
```

### BUY_COMMODITY (0x10) / SELL_COMMODITY (0x11)

Atomic transfer + ledger delta.

```
typedef struct {
    uint8_t  cargo_pub[32];
    uint8_t  player_pubkey[32];
    uint8_t  station_pubkey[32];
    int64_t  ledger_delta;
    uint8_t  kind;
    uint8_t  direction;   /* 0 = BUY (station→player), 1 = SELL (player→station) */
    uint8_t  _pad[6];
} settlement_payload_trade_t;  /* 112 bytes */
```

### ISSUE_CREDIT_NOTE (0x12) / REDEEM_CREDIT_NOTE (0x13)

Station-issued IOUs. Redemption marks it settled.

```
typedef struct {
    uint8_t  note_id[32];   /* SHA-256 of (station, player, amount, nonce) */
    uint8_t  station_pubkey[32];
    uint8_t  player_pubkey[32];
    int64_t  amount;
    uint64_t nonce;
    uint64_t expiry_tick;
} settlement_payload_credit_note_t;  /* 120 bytes */
```

### START_STATION_SITE (0x20)

A planned outpost is created.

```
typedef struct {
    uint8_t  outpost_pubkey[32];
    uint8_t  founder_pubkey[32];
    char     name[16];
    float    pos_x;
    float    pos_y;
    uint64_t planted_tick;
} settlement_payload_start_site_t;  /* 72 bytes */
```

### DELIVER_CONSTRUCTION_INPUT (0x21)

Input cargo consumed to advance scaffold build progress.

```
typedef struct {
    uint8_t  scaffold_id[32];
    uint8_t  station_pubkey[32];
    uint8_t  input_pubs[3][32];
    uint8_t  input_count;
    uint8_t  module_type;
    uint8_t  ring;
    uint8_t  slot;
    uint8_t  _pad[4];
} settlement_payload_construction_input_t;  /* 168 bytes */
```

### COMPLETE_STATION_MODULE (0x22)

A scaffold's build completes and the module activates.

```
typedef struct {
    uint8_t  scaffold_id[32];
    uint8_t  station_pubkey[32];
    uint8_t  module_type;
    uint8_t  ring;
    uint8_t  slot;
    uint8_t  _pad[3];
    uint64_t completed_tick;
} settlement_payload_complete_module_t;  /* 80 bytes */
```

### POST_SIGNAL_ANCHOR (0x30)

Snapshots the station's signal-channel hash.

```
typedef struct {
    uint8_t  signal_channel_hash[32];
    uint64_t anchor_tick;
} settlement_payload_signal_anchor_t;  /* 40 bytes */
```

### OPERATOR_POST (0x31)

Persona-authored text signed by the station. Same as existing
`chain_payload_operator_post_t`.

### PLAYER_DEATH (0x40), FRAGMENT_TOW (0x41), FRAGMENT_RELEASE (0x42)

Payload-compatible with existing `chain_payload_death_t`,
`chain_payload_fragment_tow_t`, and `chain_payload_fragment_release_t`.

### SEGMENT_COMMIT (0xF0)

Checkpoint closing a segment.

```
typedef struct {
    uint32_t segment_index;
    uint8_t  state_root[32];
    uint8_t  prev_segment_root[32];
    uint32_t event_count;
    uint8_t  first_event_hash[32];
    uint8_t  last_event_hash[32];
} settlement_payload_segment_commit_t;  /* 136 bytes */
```

## Per-station segmenting

Each station maintains its own ordered event log. Events are numbered
`(station_pubkey, event_id)` where `event_id` is monotonic within a segment.
Segments are bounded (typically 256–1024 events) and committed with a
`SEGMENT_COMMIT` event.

### Segment lifecycle

1. **Open:** A new segment starts with `event_id = 1` and `prev_hash = 0`.
2. **Append:** Events are appended with monotonically increasing `event_id`.
3. **Commit:** A `SEGMENT_COMMIT` event closes the segment. Its `prev_hash`
   links to the last game event. The next segment's first event has
   `prev_hash = SHA-256(SEGMENT_COMMIT header)`.
4. **Anchor:** The commit event (and optionally the full segment) is uploaded
   to Arweave. The discovery manifest updates with the new checkpoint txid.

### Cross-station events

When a `TRANSFER_CARGO` crosses station zones:

1. The source station emits `TRANSFER_CARGO` with `cross_ref_station` set to
   the destination's pubkey.
2. The destination station emits a paired `TRANSFER_CARGO` with
   `cross_ref_station` set to the source's pubkey and `cross_ref_event_id`
   set to the source event's id.
3. For P2P: a quorum of peers in the source station's signal cone signs the
   outgoing event; a quorum in the destination's cone signs the incoming event.

## Forward-apply semantics

The forward-apply engine (#351) takes `(prev_checkpoint, segment_events[])`
and produces the next checkpoint.

### State model

Settlement-owned state is a subset of `world_t`:

| Component | Key |
|-----------|-----|
| Station manifests | `(station_pubkey, unit_pub)` |
| Station ledgers | `(station_pubkey, player_pubkey)` |
| Outstanding credit notes | `note_id` |
| Active construction sites | `(station_pubkey, scaffold_id)` |
| Fragment ownership | `fragment_pub` |
| Signal channel hash | `station_pubkey` |
| Player death records | `(player_pubkey, death_tick)` |

Sim-owned state (NOT in settlement):

| Component | Reason |
|-----------|--------|
| Ship positions/velocities | Transient, per-tick |
| Asteroid positions/HP | Transient, physics-driven |
| NPC state machines | Transient AI |
| Tractor/mining physics | Transient gameplay |

### Applying an event

The history resolver verifies each event header's station/quorum signature
before calling the forward-apply engine. For each imported event, the current
engine then:

1. **Verifies** the payload bytes against the signed header's payload hash.
2. **Validates** preconditions (fragment exists, cargo in manifest, etc.).
3. **Preflights provenance** for every transfer, sell, and construction input
   by calling the shared cargo-receipt trust verifier with caller-resolved
   receipt, origin-event, and authority-lifecycle evidence.
4. **Binds custody** by requiring the final receipt recipient to match the
   event's current holder (source actor or construction station).
5. **Applies privately** to a temporary state and publishes state plus
   checkpoint only after the complete segment succeeds.
6. **Produces** deterministic previous- and post-segment state roots.

Missing history and unknown, untrusted, or revoked authorities remain distinct
stable rejection outcomes. Invalid events leave settlement state and caller
checkpoint storage byte-identical. Receipt and origin pointers are borrowed
for the call only and are never retained. The engine performs no I/O and has
no wall-clock dependence.

### State root

The state root is a SHA-256 Merkle tree over all settlement-owned state sorted
by key. For efficiency, an incremental Merkle tree (BLAKE3 MMR) avoids
rebuilding from scratch per event.

## Relationship to existing chain log

The existing `chain_event_header_t` (184 bytes) is the foundation. This design:

- Adds `quorum_root` (32 bytes) for P2P threshold signing.
- Repurposes `version` from the existing `_pad[7]`.
- Adds `SEGMENT_COMMIT` (0xF0) as a new event type.
- Renumbers existing event types into categories (0x01 asset, 0x10 economy,
  0x20 construction, etc.) while keeping payload layouts byte-compatible
  where possible.

The `signal_verify` tool is extended to validate the new header format,
quorum certificates, and segment boundaries.

## Open questions

1. **Quorum certificate format.** The internal structure of a quorum
   certificate (list of peer signatures, threshold parameter) is specified
   separately in `shared/quorum_cert.h`.

2. **Segment commit cadence.** Per N events, per T sim ticks, per dock-out?
   Left as a deployment parameter.

3. **Merkle tree choice.** BLAKE3 MMR vs SHA-256. The forward-apply engine
   (#351) makes the final call.

4. **Credit note expiry.** Whether credit notes expire, and at what cadence,
   is an economic design question.

5. **Signal channel hash format.** Specified in `shared/signal_model.h` and
   referenced here opaquely.

## Version history

| Version | Date | Changes |
|---------|------|---------|
| 0 | 2026-05-30 | Initial draft |
