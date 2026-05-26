# Signal — Decentralization Synthesis

**Status:** synthesis of [`decentralization.md`](./decentralization.md) (the
federation design that is mostly shipped) and
[`p2p-design.md`](./p2p-design.md) (the pure-P2P design that is speculative).
**Audience:** anyone deciding what hardening work to ship next on the
decentralization stack and wanting to understand how today's federation
relates to a possible pure-P2P endpoint.

This document is short on purpose. The two source docs are exhaustive; this
one only extracts what is genuinely different between them, identifies the
single design knob they disagree on, and orders the open work so that every
step is independently valuable even if the pure-P2P endpoint is never
reached.

## TL;DR

Federation and pure-P2P are not opposites. They share the same crypto
substrate (Ed25519 everywhere, content-addressed cargo, signed chain log,
deterministic station identity) and the same economic model (per-station
ledgers, no global wallet, hauler-as-FX, sovereign-issuer stations). They
disagree on exactly one thing: **who signs each chain-log event**. One
operator's keypair (federation), or a quorum of present shard members
(pure P2P).

A useful one-liner: **federation is P2P with the quorum size pinned to 1.**

Recent shipped work (post the last `decentralization.md` update) has moved
the actual codebase substantially closer to the P2P endpoint without
intending to. The tick-addressed input protocol, the 512-frame
client-side replay buffer, idempotent action IDs, and the dead-reckoning
of remote objects together mean that **the client is already running its
own copy of the deterministic sim every tick and reconciling against the
server's state.** The remaining gap is mostly determinism quality and
whose signature attaches to the result — not who runs the sim.

## What both designs agree on

Lifted from a side-by-side read of the two source docs. Every row here
is identical between federation and pure-P2P:

| Concern | Mechanism | Source |
| --- | --- | --- |
| Player identity | Ed25519 keypair, persisted client-side | [`client/identity.h`](../client/identity.h) |
| Player action authority | Signed input, server validates signature | PR #488 |
| Save record key | Player pubkey, with legacy-token fallback | PR #491, [`/CLAUDE.md`](../CLAUDE.md) |
| Asteroid identity | Deterministic content hash from `(belt_seed, index)` | PRs #486, #487 |
| Fragment identity | Hash binds parent `rock_pub` + fracture seed + index | [`shared/types.h`](../shared/types.h) |
| Cargo identity | Content hash on `cargo_unit_t.pub`, inputs in `parent_merkle` | PR #481 |
| Station identity | Deterministic Ed25519 keypair per station | PR #493 |
| Per-station event history | Signed chain log, 184-byte header + payload | PR #497 |
| Cross-station settlement | Cargo receipt chain verified at zone boundary | Layer D |
| Verifier | `signal_verify` walks log + verifies signatures | Layer E |
| Sim determinism (today) | Fixed-step 120 Hz, identical bit-for-bit across machines running the same world seed | [`decentralization.md`](./decentralization.md) |
| Per-station ledgers | `(station_id, player_pubkey) → balance`, no global wallet | [`/CLAUDE.md`](../CLAUDE.md) |
| Cross-station value transfer | Goods-only; hauler is the FX desk | [`/CLAUDE.md`](../CLAUDE.md) |
| Stations as sovereign issuers | `station_credit_pool()` can go arbitrarily negative; no money supply cap | [`/CLAUDE.md`](../CLAUDE.md) |
| Cargo three-state model | Fragment / bulk float / crate | [`docs/cargo-architecture.md`](./cargo-architecture.md) |

Roughly 80% of either source doc is restating this shared substrate.

## What they actually disagree on

| Concern | Federation (today) | Pure P2P (speculative) |
| --- | --- | --- |
| **Who signs a chain-log event** | The station's operator, using the station's keypair | Deterministic station identity for purely local events; quorum of ≥2/3 active shard members for cross-shard events |
| **Who runs the authoritative sim** | One operator's server per station | Every peer in the shard's signal cone |
| **Cognition** | Operator's server code (free, server-side, unstructured) | Bountied LLM jobs paid from station treasury; deterministic baseline policy as fallback |
| **Persistence** | Operator's `world.sav` + per-station `chain/<base58>.log` on disk | Content-addressed snapshots in a DHT, signed state roots |
| **Sim math** | FP tolerated because the server is canonical | Fixed-point everywhere in the sim (FP only in render) |
| **Failure if the host disappears** | Station offline | No host concept — station never offline, degrades to mechanical baseline |
| **Sybil resistance** | Operator policy | Protocol-level: spawn grace, optional PoW/burn, outpost allowlists |
| **Status** | Shipped (Layers A–E of #479) | Speculative; gated on the fixed-point sim rewrite |

Every cell on the right is downstream of one decision: **collapse the
operator role into the shard.** Once nobody is privileged, persistence
must be content-addressed, cognition must be paid, math must be
cross-platform deterministic, sybil must be a protocol concern, and the
chain-log signature must be a quorum certificate.

## The unifying frame

Both designs treat the **chain log as the authority surface**. What
signs it determines who has authority. The schema does not need to
change between them — `chain_event_header_t.authority` is already a
`uint8_t[32]` pubkey ([`server/chain_log.h`](../server/chain_log.h)). In
federation it's a station keypair held by one operator. In pure P2P it
stays a station keypair for local events (since every peer computes the
same outcome deterministically and the station's seed-derived identity
is enough), and gets a sibling commit certificate for events that move
state between shards.

Same primitive, different governance:

- **Federation:** stations are sovereign nations, each governed by one
  operator who runs its sim and signs its books.
- **Pure P2P:** stations are sovereign nations, each governed by the
  consensus of whoever is currently inside its signal cone, with the
  books computed independently by every governor and a quorum signing
  each epoch.

The economic story (per-station credits, hauler-as-FX, no global wallet,
sovereign issuance) is identical in both.

## What just shipped that matters

The 39 commits between when `decentralization.md` was last updated and
now are mostly about one thing: turning the client from a thin display
of the server's state into a peer that runs its own copy of the sim and
reconciles. None of these commits frame themselves as P2P work, but
collectively they are roughly the first half of it.

The directly relevant ones:

- **Tick-addressed multiplayer movement sync** (commit `735870c`).
  Movement packets carry the client-predicted target tick
  ([`shared/protocol.h`](../shared/protocol.h) `PLAYER_RECORD_SIZE 77`,
  with `input_ack:u16`, `server_tick:u32`, `input_tick_ack:u32`). The
  server only applies an input during the matching `world_sim_step()`,
  so snapshots and prediction frames share a single integer clock
  instead of racing wall-clock packet arrival. This is the foundational
  primitive of lockstep multiplayer.
- **Client-side prediction with replay buffer**
  ([`client/net_sync.c`](../client/net_sync.c),
  [`client/client.h`](../client/client.h) `NET_REPLAY_FRAME_CAP 512`).
  The client records up to 512 frames of predicted input
  (`input_replay_frame_t`) and replays them after each server snapshot
  to recover any state delta that arrived between prediction and ack.
- **Idempotent action IDs**
  ([`shared/protocol.h`](../shared/protocol.h) `ACTION_ACK 0x3A`,
  `ACTION_RESULT 0x3B`). 16-bit action IDs let the client retry dropped
  actions without duplicating effects server-side. This is the same
  shape an inputs-only P2P gossip would need.
- **Dead reckoning of remote world objects** (commit `16c180a`).
  Remote asteroids and NPCs extrapolate locally between snapshots, with
  bounded windows (`ASTEROID_RENDER_EXTRAPOLATE_MAX_SEC 0.75f`,
  `NPC_RENDER_EXTRAPOLATE_MAX_SEC 0.60f`). Every client is now running
  forward-projection logic that, in a pure-P2P design, becomes "running
  the canonical sim."
- **Local obstacle collision prediction** (commit `6831ed0`). The client
  predicts its own collisions instead of waiting for the server to
  resolve them.
- **Net motion telemetry** (commits `68a462b`, `72543f9`).
  `replayed_samples`, `replayed_frames`, latency-compensation counters.
  This is exactly the instrumentation needed to *detect* client/server
  divergence — which is exactly the instrumentation needed to *enforce*
  identical state across peers later.

Implication: the federation doc's "clients trust the sim" framing is no
longer fully accurate. Clients *also run* the sim, predict their own
state, and reconcile. The remaining federation property is that the
server's reconciliation is currently treated as ground truth. Replacing
that with "shard quorum reconciliation" is a smaller change than the
`p2p-design.md` writeup implied.

## The practical synthesis

Given the above, the path from where the codebase is today to a hybrid
or pure-P2P endpoint is a sequence of steps that each pay for themselves
in federation mode. None of them is roadmap-gated on the others, and
none requires committing to pure-P2P as a destination.

1. **Today — federation shipped.** Operators run authoritative servers,
   clients run prediction + replay + dead reckoning, server snapshots
   reconcile divergence. Layers A–E of #479 cover the operator-honesty
   trust model.

2. **Cross-platform determinism work.** Fold the floating-point sim
   paths in `server/sim_*.c` and the mirrored client sim_step
   ([`client/main.c::sim_step`](../client/main.c)) toward identical
   numeric behavior across native and wasm. The cheap version is
   IEEE-strict FP with no fast-math; the durable version is q32.32
   fixed-point in the sim. This is independently valuable today — it
   shrinks the `replayed_samples` counter and makes desync bugs
   reproducible — even before any P2P commitment.

3. **Verifiable mirrors.** Have the client compute a state-root every N
   ticks (BLAKE3 over the sim state slice the snapshot covers) and
   compare against the server's. Mismatches become loud client-side
   instead of silent reconciliations. This is a *defensive* feature for
   federation (rogue-operator detection) and the natural place the
   "shard quorum" surface starts being meaningful.

4. **Quorum-signed cross-shard events.** When cargo crosses a zone
   boundary, add a sibling signature from a quorum of present clients
   alongside the operator's signature. Same `chain_log` schema, two
   signatures instead of one. This narrows the trust radius on the
   highest-stakes event type without rearchitecting anything.

5. **Cognition market.** Even in pure federation, expose station AI as
   a treasury-funded bountied service. Useful immediately: gives
   stations a real spend sink for their own currency and routes player
   attention back into the local economy. Drops in unchanged when
   pure-P2P arrives.

6. **DHT snapshots.** Have archive peers store federation snapshots in
   a content-addressed DHT, and have `signal_verify` accept a snapshot
   + epoch commit certificate instead of just a chain log. Useful for
   federation backups/auditing, mandatory for P2P.

7. **Pure-P2P shards.** After (2) and (4) are real, individual shards
   can run without an operator. Federation shards and pure-P2P shards
   coexist in the same world. A trade run might pass through several
   of each. The chain log's `authority` field carries either an
   operator signature or a quorum commit, and the verifier checks both
   shapes.

Steps 2, 3, 5, and 6 are pure wins regardless of whether pure-P2P ever
ships. They are hardening, instrumentation, and revenue features. Steps
4 and 7 are the only ones that require any actual P2P commitment, and
they're scoped narrowly when they arrive.

## Open questions the synthesis exposes

These are the corners where pulling federation and pure-P2P together
*surfaces* a question that neither doc on its own had to answer:

1. **Where does the client's `sim_step` actually live?** Today the
   server's `world_sim_step` and the client's `sim_step` are sibling
   implementations with overlapping but not identical responsibilities
   (the server runs the canonical sim; the client runs prediction +
   render-side animation). For a converging design they should be the
   *same* function, with the client just choosing which subsystems to
   run authoritatively. Some refactoring would help.

2. **When does a station know it is in federation vs P2P mode?** A
   station running under one operator with no other peers connected
   should look like classic federation. The same station with N players
   in its signal cone running verifying mirrors is a different trust
   regime, but the operator code path is currently the same. The
   chain-event signing function probably wants a `quorum_threshold`
   field that defaults to 1 (operator-only) and rises as the shard
   commits more.

3. **What does the `signal_verify` tool consume in a hybrid world?**
   Today it walks one operator's chain log. In step 4 it would also need
   to walk a sibling quorum certificate. In step 6 it consumes a
   snapshot + commit certificate. The CLI surface wants design before
   the formats land.

4. **Cognition jobs in federation.** If a station runs LLM cognition as
   a bountied job (step 5), the operator's server is the natural first
   provider. But because the bounty is paid in the station's currency
   and any peer can claim it, the operator is just *first among equals*
   from day one. This is a clean way to introduce P2P-shaped economics
   without changing topology.

## What this document is not

- **Not a roadmap.** No milestones, no team allocation, no dates.
- **Not a replacement for either source doc.** `decentralization.md`
  remains the reference for what's shipped. `p2p-design.md` remains the
  reference for the speculative endpoint. This document is the bridge.
- **Not an endorsement of the P2P endpoint.** A pure-P2P Signal is one
  possible end state. A hardened federation with verifying mirrors and
  cross-shard quorum signatures is another, and is probably enough for
  the goals players actually care about.

## Reading list

- [`docs/decentralization.md`](./decentralization.md) — the federation
  reference doc. Authoritative for what's shipped.
- [`docs/p2p-design.md`](./p2p-design.md) — the pure-P2P endpoint sketch.
  Authoritative for the speculative direction.
- [`docs/cargo-architecture.md`](./cargo-architecture.md) — the three-
  state cargo model both designs reuse.
- [`/CLAUDE.md`](../CLAUDE.md) — the per-station credits text both
  designs take as economic input.
- [`server/chain_log.h`](../server/chain_log.h) — the event schema
  whose `authority` field is the single knob the two designs disagree on.
- [`shared/protocol.h`](../shared/protocol.h) — the wire format,
  including the tick-addressed input fields (`input_ack`, `server_tick`,
  `input_tick_ack`) that opened the door to client-side prediction.
- [`client/net_sync.c`](../client/net_sync.c) — the replay buffer and
  reconciliation logic that turn the client into a prediction peer.
- Issue #479 — off-chain decentralization umbrella.
- Issue #480 — on-chain anchoring follow-on.
