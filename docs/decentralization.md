# Signal — Decentralization Architecture

This document is the architectural overview for Signal's off-chain decentralization
stack: the identity layers, the trust model, and what it means to run a station as
an independent operator. It is the companion to
[`operator-onboarding.md`](./operator-onboarding.md), which is the practical
recipe-style guide.

The work tracked here is the off-chain half of issue #479. The on-chain anchoring
work (state-root commitments, wrap contracts, bounty payouts) lives in the Solana
ticket #480 and is not implemented in this stack — anywhere this document says
"future" or "post-#480" it means exactly that.

The intended reader is a developer or operator who has been told "I'd like to host
a Signal station" and wants to understand what they're signing up for before they
generate a keypair and ship a server. The tone is technical. There is no
marketing copy.

## What is Signal?

Signal is a multiplayer space-station game. Live build:
[signal.ratimics.com/play](https://signal.ratimics.com/play). Players fly a small
ship around an asteroid belt, fracture rocks for ore, smelt and refine at
stations, and throw the leftover rocks at each other. The simulation is fully
authoritative on the server, fixed-step at 120 Hz, and identical bit-for-bit
across every machine that runs the same world seed.

The relevant fact for this document is that there is no global player wallet and
no global ledger. **Credits are per-station** (`station_t.ledger[]` in
[`shared/types.h`](../shared/types.h)). Each station owns its own books, its own
specialty supply chain, and — after Layer B of #479 — its own Ed25519 keypair.
Federation is the natural extension of a primitive that was already in the
codebase.

## The identity stack

Every long-lived object in Signal has a content-addressed identity: an asteroid,
a fragment chipped off that asteroid, an ingot smelted from that fragment, a
finished frame fabricated from that ingot, the player who initiated the chain,
and the station that authored each transformation. The hashes nest. Knowing one
layer is enough to verify the layer above it; knowing the chain log is enough to
verify the lot.

The layers, from substrate up:

### `rock_pub` — terrain asteroids

Seed-origin asteroids carry a stable 32-byte pubkey baked at world birth:
`asteroid_t.rock_pub` ([`shared/types.h`](../shared/types.h)). The hash is
deterministic in `(belt_seed, asteroid_index)` so every server with the same
world seed agrees on every rock's identity. PRs #486 and #487 made this layer
permanent; the rock-ledger is sorted and binary-searched at runtime.

### `fragment_pub` — fracture children

When a player fractures an asteroid, each child fragment gets its own pubkey:
`asteroid_t.fragment_pub` ([`shared/types.h`](../shared/types.h)). The
fragment hash binds in the parent's `rock_pub`, the fracture seed, and the
fragment index, so the lineage from terrain rock to handheld pebble is auditable
without trusting the server's word for it.

Once a fragment claim resolves, the fragment is what downstream layers point at;
`rock_pub` becomes zero on the fracture children themselves and is preserved
only in the destroyed-records anchor.

### `cargo_unit.pub` — ingots and finished goods

PR #481 unified the named-ingot store and the bulk inventory under a single
`cargo_unit_t`. Every ingot, every frame, every laser module, every tractor
coil, every repair kit is one row in a per-holder manifest with a 32-byte
content hash:

```c
typedef struct {
    uint8_t  kind;              /* cargo_kind_t */
    uint8_t  commodity;
    uint8_t  grade;
    uint8_t  prefix_class;
    uint16_t recipe_id;
    uint8_t  origin_station;
    uint8_t  quantity;          /* 1 for current live production */
    uint64_t mined_block;
    uint8_t  pub[32];           /* content hash */
    uint8_t  parent_merkle[32]; /* sorted-input merkle root */
} cargo_unit_t;                 /* 80 bytes */
```

(See [`shared/types.h`](../shared/types.h) for the canonical definition and
[`shared/manifest.h`](../shared/manifest.h) for the helpers.)

For ingots, `pub = hash_ingot(commodity, grade, fragment_pub, output_index)` —
the smelt event's identity is fully determined by its inputs. For finished
goods, `pub = hash_product(recipe_id, sorted_inputs, output_index)`. Identical
inputs always yield the same `pub`; that is what makes the manifest portable
between holders without a central authority renaming the rows.

### Station authority — `station_pubkey`

Layer B of #479 (PR #493) gave each station its own Ed25519 keypair, derived
deterministically from operator-held secret material plus public world data:

- **Seeded stations** (Prospect Refinery, Kepler Yard, Helios Works — indices
  0/1/2) derive their seed as
  `SHA256("signal-station-v1" || operator_secret || world_seed_u32 || station_index_u32)`.
  Every server running the same operator secret and world seed agrees on every
  seeded station's pubkey bit-for-bit. See
  [`server/station_authority.c`](../server/station_authority.c) and the seeded
  bootstrap in [`server/game_sim.c`](../server/game_sim.c).
- **Player-planted outposts** (indices 3+) derive their seed as
  `SHA256("signal-outpost-v1" || operator_secret || founder_pub[32] || station_name[16] || planted_tick_u64)`.
  An operator with the station authority secret and founding event can rederive
  the outpost's pubkey.

The private key is rederivable on demand only with that operator secret; it is
**never** serialized to disk and **never** sent over the wire. The struct layout
enforces this — `station_secret` is the last field of `station_t` and a
`_Static_assert` keeps it that way ([`shared/types.h`](../shared/types.h)).
Losing the disk does not leak the private key.

### Player identity — `player_identity_t`

Layers A.1 through A.4 of #479 (PRs #484, #485, #488, #491) put a per-player
Ed25519 keypair in the client. The client generates the keypair on first launch,
persists it under a platform-appropriate path (POSIX:
`$XDG_DATA_HOME/signal/identity.key` at mode 0600 inside a 0700 directory; macOS:
`~/Library/Application Support/signal/identity.key`; web: `localStorage`), and
sends the pubkey on connect. The server validates state-changing actions
against the player's signature, keys the save file by pubkey, and offers a
"claim my legacy save" handshake for pre-A.4 anonymous saves.

See [`client/identity.h`](../client/identity.h) for the load/save surface and
[`client/identity.c`](../client/identity.c) for the platform-path resolution.

The HUD shows the first 8 base58 chars of the player's pubkey as their permanent
identity. That prefix is the callsign.

### The chain log — Layer C of #479

Layer C (PR #497) closed the loop: every state-mutation event a station authors
is signed by the station and chained to the previous event by SHA-256 hash. The
file [`server/chain_log.h`](../server/chain_log.h) defines the schema; the
implementation is [`server/chain_log.c`](../server/chain_log.c). The result is
that any auditor with the on-disk chain log and the station's pubkey can prove
that no event was inserted, removed, or altered after the fact.

Event types currently emitted (from `chain_event_type_t` in
[`server/chain_log.h`](../server/chain_log.h)):

| Event | Meaning |
| --- | --- |
| `CHAIN_EVT_SMELT` | Physical fragment smelted into an ingot at this station |
| `CHAIN_EVT_CRAFT` | Input cargo units fabricated into a finished product |
| `CHAIN_EVT_TRANSFER` | Cargo unit moved between holders |
| `CHAIN_EVT_TRADE` | Transfer + ledger delta, atomic |
| `CHAIN_EVT_LEDGER` | Station-side credit balance mutation |
| `CHAIN_EVT_ROCK_DESTROY` | Asteroid fractured to terminal state |
| `CHAIN_EVT_OPERATOR_POST` | Station-authored MOTD, chatter, rarity text, build/world anchor, or other operator copy |
| `CHAIN_EVT_FRAGMENT_TOW` | Player tractor takes possession of an in-space fragment |
| `CHAIN_EVT_FRAGMENT_RELEASE` | Fragment tow ended without a smelt |
| `CHAIN_EVT_DEATH` | Player run ended; highscores are replayed from these events |

Every event is exactly 184 bytes of header followed by `uint16` payload length
and the payload bytes. The header includes `epoch` (sim tick), `event_id`
(monotonic per `(station, epoch)`), `type`, `authority` (the station's pubkey),
`payload_hash` (SHA-256 of the payload), `prev_hash` (the SHA-256 of the
previous event's full header), and a 64-byte Ed25519 `signature` over the
unsigned header span. A `_Static_assert` pins the size to 184 bytes so the
on-disk format cannot drift silently
([`server/chain_log.c`](../server/chain_log.c)).

## The trust model

Three concentric circles. Each one is independently verifiable; each one widens
the radius of mutual mistrust the system tolerates.

### Inside the sim

Clients trust the sim. The sim is deterministic, fixed-step at 120 Hz, and
identical for everyone connected to the same server. There is no client-side
authority over physics, mining outcomes, or production. Cheating against the
sim is server-side enforcement and always has been; the identity stack does not
change anything here.

### Inside the chain log

A chain log carries the station's signature on every event and a hash chain
linking every event to its predecessor. Anyone with the on-disk log file plus
the station's pubkey can replay the log, verify each signature, and verify the
prev-hash linkage. This is the surface that `signal_verify` (Layer E) exposes
as a standalone tool.

The trust assumption shrinks to: "the operator who holds this station's private
key is honest about what their station did." If they aren't, the chain log
catches them — the sole remaining attack is a fork (the operator publishes two
divergent logs to two audiences), which the on-chain anchoring work in #480 is
designed to prevent.

### Across federation

When a second operator joins, they bring their own pubkey-keyed station, run
their own chain log, and sign their own events. They do not have to trust the
first operator's server; they only have to verify the first operator's
signatures on the cargo units and receipts that cross the zone boundary. Layer
D is the off-chain receipt layer: cargo units carry a chain of signed transfer
receipts back to the originating event, and the destination station verifies the
chain on dock before accepting the unit.

Every station is sovereign within its zone. Prospect's operator decides
Prospect's price curves. Helios's operator decides what Helios pays for
crystal. The economy is per-zone because the authority is per-zone.

## What stations actually are

A station is, mechanically, a `station_t` ([`shared/types.h`](../shared/types.h))
with:

- A position, a ring layout, a slot grid, and a manifest of installed modules.
- A per-station ledger keyed by player pubkey
  (`station_t.ledger[]`, `station_t.currency_name`).
- A 32-byte `station_pubkey`, a 64-byte `station_secret` (rederivable, not
  serialized), and `chain_last_hash` + `chain_event_count` so the chain
  survives restart.
- For outposts: an `outpost_founder_pubkey` and `outpost_planted_tick` so the
  keypair can be rederived deterministically on save load when the operator
  secret is present.

Authority is structural: the keypair *is* the station. There is no separate
operator identity registered with the station — whoever holds the private key
*is* the operator. Station authority custody is the configured station secret;
world seed and outpost provenance identify the station, but no longer reveal
the signing key.

## Per-station ledgers and per-zone economics

The CLAUDE.md at the repo root has the canonical text on this; see the
"Economy: per-station credits" section of
[`/CLAUDE.md`](../CLAUDE.md). The summary, for federation:

- A player has no global wallet. Credits earned at Prospect are spendable at
  Prospect only. The string `currency_name` ("prospect credits", etc.) is
  station-local, because the underlying authority is station-local.
- Prices are dynamic on hopper fullness (buy 1.0× → 0.5×) and stock fullness
  (sell 2.0× → 1.0×). See `station_buy_price` / `station_sell_price` and the
  `test_dynamic_ore_price_*` tests.
- Carrying value between stations means carrying *goods*, not currency.
  Hauling is first-class, and the hauling primitive is now backed by portable
  cargo receipt chains; cross-operator policy and UX are the remaining
  hardening work.

The reason this maps so cleanly onto federation is that the ledger was already
per-station before any of the identity work. Layer B simply attached the
authority to the ledger; Layer C simply attached the audit trail.

## The signed event log on disk

On-disk layout, per station:

```
chain/<base58(station_pubkey)>.log
```

The base directory defaults to `chain/` and can be overridden for tests via
`chain_log_set_dir()` ([`server/chain_log.h`](../server/chain_log.h)). Each
entry is the 184-byte header followed by `uint16 payload_len` and `payload_len`
bytes of payload. Entries are append-only; existing entries are never
rewritten.

Write semantics: `chain_log_emit` opens the per-station log file in append
mode, writes header + payload-length + payload, calls `fflush`, and closes.
Crash safety relies on the append-only property — a partial last-entry write
is detected on the next verify walk and treated as chain damage, not as an
invitation to rewrite history. Startup verification records per-station chain
health in `station_t`; `failed` or `mismatch` stations block future
`chain_log_emit` calls until an operator repairs the save/log pairing.

In-memory state: `station_t.chain_last_hash` is the SHA-256 of the most
recently authored event header. The next event's `prev_hash` is set to this
value, linking the log into a hash chain. `chain_event_count` is the
monotonic per-station counter within the current segment, stamped into
`event_id` on every emit. Both fields are persisted by the world save (v41+),
so the chain survives a server restart cleanly. The actual event records live
in the side files under `chain/` — they are not part of `world.sav`.

Segment semantics: a later event with `event_id == 1` and `prev_hash == 0`
starts a fresh append-only segment in the same file. This covers fresh-world
or intentionally reset sessions without rewriting older bytes. Verifiers still
check every segment strictly; arbitrary broken linkage, non-zero reset hashes,
or missing middle events remain failures. Startup continuation uses the final
segment's tail event id and tail hash, not the total event count across all
segments.

Startup policy: the on-disk log is authoritative when it verifies. If the disk
tail is ahead of `world.sav` because the previous process appended events and
died before autosave, `world_load()` adopts the verified disk tail before any
startup anchor emits. If the log fails verification, or if a valid disk tail and
saved continuation pointer disagree, the station becomes operator-visible
health `failed` or `mismatch` and appends are blocked to avoid creating a fork.

Verification semantics (`chain_log_verify`,
[`server/chain_log.h`](../server/chain_log.h)):

1. Walk the on-disk log entry by entry.
2. For each entry: verify the Ed25519 signature against the asserted authority
   pubkey, which must equal the station's `station_pubkey`. Verify
   `payload_hash` matches the SHA-256 of the stored payload bytes. Verify
   `prev_hash` matches the SHA-256 of the previous entry's full header inside
   the segment (or zero for a segment's first entry).
3. If a later entry starts with `event_id == 1` and zero `prev_hash`, start a
   new segment and continue verification from a zero hash. Report total
   events, segment count, segment resets, and final-segment tail counters.
4. If `out_event_count` is non-NULL, write the final segment's tail event id
   through, regardless of whether the walk eventually failed. If
   `out_last_hash` is non-NULL, write the SHA-256 of the last successfully
   walked header through.
5. Returns `true` iff every event verifies. A missing log file returns `true`
   with zero events walked — the empty chain is trivially valid.
6. The server's startup wrapper stores this result as chain health and exposes it
   through `/health` and `/api/station/<id>/state`.

## Cross-station settlement (Layer D — shipped off-chain)

Cargo units crossing a zone boundary carry a chain of signed transfer
receipts. The destination station verifies the chain before accepting the
unit, and emits its own `CHAIN_EVT_TRANSFER` (and, if the dock-side trade is
atomic, an accompanying `CHAIN_EVT_LEDGER`) into its own log.

The schema is split deliberately: `cargo_unit_t.parent_merkle`
([`shared/types.h`](../shared/types.h)) names the producing inputs, while
`cargo_receipt_chain_t` stores the portable station-signed transfer trail. The
origin receipt pins to the hash of the originating `SMELT` or `CRAFT` chain
event header; later receipts link to the prior receipt hash.

Remote multiplayer now mirrors that bearer state in both directions. Buy/deliver
flows send `NET_MSG_CARGO_RECEIPT_BUNDLE` as the full carried receipt chain for
the named cargo unit, and the client caches the chain by `cargo_pub` so the next
manifest refresh can reattach it to the local ship entry. Before queued
sell/deliver actions, the client actively presents matching verified chains via
`NET_MSG_PRESENT_RECEIPT_CHAIN`. The server accepts that message for cargo
currently carried by a pubkey-identified player: the presented chain must
verify, the head receipt must name that player, and any local chain already
attached to the cargo must be an exact prefix. This keeps the WebSocket and
WebRTC transports aligned with the future mesh rule: the peer carrying cargo
also carries the portable proof of custody.

The handoff ticket primitive now lives in
[`shared/handoff_ticket.h`](../shared/handoff_ticket.h). A source authority can
sign a portable envelope over source/destination authority pubkeys, player
pubkey, expiry, a canonical ship-state hash, and a cargo root that includes the
manifest plus attached receipt chains. The destination can verify the envelope
against the presented ship snapshot before accepting cross-zone state.

The first handoff product path is now wired over the WebSocket protocol:
`NET_MSG_HANDOFF_REQUEST` asks a source station to sign a ticket,
`NET_MSG_HANDOFF_TICKET` returns it, `NET_MSG_HANDOFF_PRESENT` carries that
ticket plus a field-packed ship/cargo snapshot, and `NET_MSG_HANDOFF_RESULT`
reports destination accept/reject. The destination verifies the source is a
known station authority, then checks source signature, destination authority,
player pubkey, expiry, ship hash, cargo root, and a runtime consumed-ticket
cache before hydrating the destination ship.

The remaining Layer D work is boundary automation and federation hardening:
persist consumed-ticket replay state across restarts, route tickets between
separate operators/nodes, harden chain compaction/backfill, define operator
policy, and build UX around failed verification.

## The verifier tool (Layer E — shipped)

Layer E ships `signal_verify` as a standalone binary that takes one or more
chain log files. By default it derives each station pubkey from the
`chain/<base58(station_pubkey)>.log` filename; renamed files can be checked
with `--station-pubkey=<base58>`. It reports:

- Total events walked and total bytes consumed.
- Segment count, accepted segment resets, and final-segment tail event id.
- First failed event (if any), with a short reason: `bad signature`,
  `bad prev-hash linkage`, `payload hash mismatch`, `truncated tail`.
- The final `chain_last_hash`, suitable for cross-checking against the
  in-memory state of a running server.
- Optional JSON output, epoch windows, decoded operator-post text, warning
  strictness, and multi-station receipt/provenance checks.

The verifier wraps the same `chain_log_verify` walker that runs at server
startup; the only difference is the CLI surface and a non-zero exit code on
failure for CI integration.

## The asset inventory tool (Layer E — shipped)

Layer E also ships `signal_chain_assets`, a read-only chain-log analyzer that
exports one row per unique `cargo_unit_t.pub`. It accepts the same station log
files, or a directory containing `*.log` files, and emits JSON by default:

```sh
cmake --build build --target signal_chain_assets
./build/signal_chain_assets chain --out /tmp/signal-assets.json
./build/signal_chain_assets --format=csv chain --out /tmp/signal-assets.csv
```

The JSON schema is `signal.chain_assets.v1`. Each asset row includes the cargo
pubkey, source event, source station, world cursor if present, segment id,
strict verification status, commodity/recipe/prefix metadata, mined block,
parent fragment, craft input pubs, mint count, and transfer count. The tool
recognizes clean verifier segment resets separately from unexpected linkage
or monotonic breaks, reporting all of them as file diagnostics instead of
rewriting or repairing the log.

## Off-chain vs on-chain

| Concern | Where it lives today |
| --- | --- |
| Player identity | Local Ed25519 keypair (PR #484) |
| State-changing action authority | Player Ed25519 signature (PR #488) |
| Save record key | Player pubkey (PR #491) |
| Asteroid identity | Deterministic from world seed (#486–#487) |
| Cargo unit identity | Content hash on `cargo_unit_t.pub` (#481) |
| Station identity | Deterministic Ed25519 keypair (PR #493) |
| Per-station event history | Signed chain log (PR #497) |
| Cross-station verification | Cargo receipts (Layer D, shipped off-chain) |
| Standalone verifier | `signal_verify` (Layer E, shipped) |
| Asset inventory export | `signal_chain_assets` (Layer E, shipped) |
| Provenance-aware demand | Heritage contract recipe requirements (off-chain) |
| Cross-operator anchor | On-chain state-root commitment (#480, future) |
| Asset extraction | On-chain wrap contract (#480, future) |
| Bounty payouts | On-chain bounty program (#480, future) |

Anything in the bottom three rows is explicitly *not* shipped. The off-chain
stack stops at "anyone with a chain log file plus a pubkey can verify the
station told the truth." The on-chain stack will close the remaining gap by
periodically anchoring the chain-tip hash to a public ledger so a malicious
operator cannot publish two divergent logs to two audiences.

## Glossary

- **`rock_pub`** — 32-byte content hash for a seed-origin asteroid, baked at
  world birth, deterministic in `(belt_seed, index)`.
- **`fragment_pub`** — 32-byte hash for a fracture child, binds parent
  `rock_pub` + fracture seed + index.
- **`cargo_unit`** — the unified manifest row for an ingot or finished good
  ([`shared/types.h`](../shared/types.h)). Carries the content hash
  `pub` and the input merkle root `parent_merkle`.
- **`manifest`** — an array of cargo units owned by a holder (a player's
  ship, a station's hold). [`shared/manifest.h`](../shared/manifest.h).
- **`ledger`** — `station_t.ledger[]`, the per-station credit-balance table
  keyed by player pubkey.
- **`EVT_*` / `CHAIN_EVT_*`** — event types in the signed chain log. See
  `chain_event_type_t` in [`server/chain_log.h`](../server/chain_log.h).
- **callsign** — the first 8 base58 chars of the player's pubkey, shown in
  the HUD as their permanent identity.
- **prefix class** — the optional named-ingot prefix (`INGOT_PREFIX_RATI`,
  etc.) carried on `cargo_unit_t.prefix_class`. Anonymous for non-ingot
  kinds.
- **RATi** — the named-ingot prefix reserved for the RATi Foundation's
  bounty path. Currently a tag; bounty payouts attach to it post-#480.
- **station authority** — the Ed25519 keypair bound to a station via
  [`server/station_authority.h`](../server/station_authority.h).
- **chain log** — the per-station append-only signed event log. File path:
  `chain/<base58(station_pubkey)>.log`.
- **federation** — the model in which different operators run different
  stations under different keypairs and verify each other's chain logs at
  the zone boundary.

## Reading list

- [`/CLAUDE.md`](../CLAUDE.md) — repo-level architecture notes, including the
  canonical "Economy: per-station credits" text.
- [`docs/operator-onboarding.md`](./operator-onboarding.md) — practical guide
  to standing up a station.
- [`server/chain_log.h`](../server/chain_log.h) — chain log schema and
  verifier surface.
- [`server/station_authority.h`](../server/station_authority.h) — per-station
  keypair derivation.
- [`shared/signal_crypto.h`](../shared/signal_crypto.h) — the Ed25519 surface.
- [`shared/manifest.h`](../shared/manifest.h) — manifest helpers and the
  finished-good lifecycle invariants.
- [`client/identity.h`](../client/identity.h) — client-side player identity
  load/save.
- Issue #479 — the umbrella for off-chain decentralization.
- Issue #480 — the on-chain follow-on.
- Issue #496 — substrate-attached player birth (Layer A.5 of #479).
