# Signal — Pure P2P Design Sketch

**Status:** design exploration, not an implementation plan.
**Audience:** anyone thinking about where Signal's decentralization stack could
go if the server/client split were removed entirely.
**Relationship to** [`decentralization.md`](./decentralization.md)**:** that
document describes the federation model — many operators, each running an
authoritative server for one or more stations, signing each other's chain log
events at zone boundaries. This document describes a different end state:
**no operators, no servers**, just peers running the same deterministic
simulation and gossiping inputs. The two designs share their cryptographic
substrate (Ed25519 identities, content-addressed cargo, per-station ledgers,
signed chain log) but differ on who runs the sim.

This is a sketch. Nothing here is shipped. Lifted text in code blocks is
illustrative, not normative.

## TL;DR

- Make the sim a pure function `state_{t+1} = f(state_t, inputs_t)` in
  fixed-point arithmetic. Every peer runs it identically. There is no
  server.
- Shard the world by signal range: one station + its signal cone = one
  shard. Each shard runs its own 120 Hz lockstep among its active members.
- Station behavior splits into three layers — **mechanical**
  (deterministic, runs in the sim), **authored** (content-addressed text),
  **inferred** (LLM, paid out of the station's own treasury).
- Cargo `pub` + `parent_merkle` and the per-station chain log already give
  us most of the on-chain primitives; in P2P they're consumed by every
  peer instead of one server.
- Persistence is a content-addressed DHT of signed state-root snapshots.
  The current `world.sav` becomes a peer-local cache.
- The current ratimics.com server becomes a well-known seed peer with no
  special authority.

## The core move: deterministic lockstep

Everything else in this document is a consequence of one rewrite: the
simulation must be a pure function

```
state_{t+1} = f(state_t, inputs_t)
```

evaluable identically on every machine. Once that holds, only **inputs**
need to cross the wire — never state. Each peer recomputes the entire
shard from the input batch and arrives at the same `state_{t+1}` as
everyone else.

This is the same trick used by Factorio, Starcraft, Age of Empires, and
every other modern RTS-style multiplayer engine. It is the prerequisite
for both byte-perfect replays and trustless consensus on simulation
outcomes — which are the same problem.

Practical implications for the current codebase:

- **Fixed-point everywhere in the sim.** All physics, all production math,
  all pricing, all PRNG state moves to q32.32 (or similar). Floating point
  stays in the render path and is never written back into authoritative
  state.
- **Seeded PRNG.** Random draws come from a PRNG keyed by
  `(world_seed, shard_id, tick)`. No `rand()`, no time-based seeds, no
  thread-local nondeterminism. The existing `world_seed_u32` already
  threads through the rock pub derivation
  ([`docs/decentralization.md`](./decentralization.md#rock_pub--terrain-asteroids))
  and would extend to every other random source.
- **No wall-clock reads in the sim.** Tick number is the only clock.
- **NPC drones already qualify.** Stations currently dispatch drones via
  deterministic policy on shard state; in P2P every peer computes the same
  dispatches at the same tick. See "Station behavior" below.

The determinism rewrite is the irreducible engineering cost of the rest
of this design. Everything else (gossip, BFT, NFTs, DHT) is library work;
the sim rewrite is not.

## Sharding by signal

120 Hz lockstep is too fast to gossip globally, so the world has to shard.
The elegant fact is that **signal range already gives the boundary**.

- **One shard = one station + its signal cone.** Membership consists of
  every peer with a ship currently inside the cone, plus any peer who has
  chosen to host that station's cognition (see below).
- **Backbone shards.** Stations linked by trade or scaffold form a chain
  of overlapping signal cones. A peer haulier between two stations
  participates in both shards' lockstep for the duration of the crossing.
- **Deep space.** A ship outside all signal coverage runs in a degenerate
  "personal shard" — cheap, single-member, no consensus required. The
  personal shard merges into a station shard as the ship enters its
  signal cone.
- **Cross-shard handoff.** When a ship crosses a signal boundary, the
  handoff transcript (manifest tail + cargo receipt heads) is committed
  in both shards' next state-root. If either side rejects, the ship snaps
  back to the last agreed shard.

The signal mechanic is already first-class gameplay — weak signal
throttles mining speed and pushes ships back toward the connected chain.
In P2P, being inside a station's signal cone *is* the act of joining its
shard's consensus. Being out of signal already means you're slow and
isolated; in P2P it just additionally means your local state isn't
witnessed.

## Tick consensus

Per shard, per tick:

1. Each member signs an input frame `(tick_n, pubkey, input_bytes)` and
   gossips it on the shard mesh.
2. The tick "closes" when ≥2/3 of active members have submitted either an
   input or a signed "no input" pass.
3. Every peer applies the agreed input batch to `state_n` to produce
   `state_{n+1}`.
4. Every N ticks (~1 sec, 120 ticks) every peer signs a Merkle root over
   shard state. The set of signatures on a given root is the shard's
   commit certificate for that epoch.
5. Divergence at a commit point is a fork: the minority side is dropped
   from the shard. Their inputs after the last agreed root are slashed
   (excluded from the canonical history). They can rejoin by replaying
   from the last agreed snapshot.

Tick rate adapts to the worst active RTT in the shard, capped (target
120 Hz, floor 30 Hz). Shards are kept small (≤16 active participants) so
the worst-case RTT stays cheap. A shard at capacity that wants new
members has to wait for someone to leave, or it forks.

The signed Merkle root is the natural extension of the existing chain log:
`chain_last_hash` ([`server/chain_log.h`](../server/chain_log.h)) becomes
a per-shard state-root signed by ≥2/3 of the shard's active members each
epoch, instead of by one operator's station key. Existing
`chain_event_type_t` values continue to make sense as in-tick events; the
per-epoch commit is one additional event type (`CHAIN_EVT_SHARD_COMMIT`,
say) that carries the multi-signature.

## Station behavior, three layers

The crucial reframing for P2P: stations in current Signal already operate
autonomously by deterministic algorithm. The only thing not deterministic
from shard state is the optional intelligence layer. Splitting station
behavior into three clean layers makes the design fall out:

### 1. Mechanical (deterministic, free)

Smelting, hopper pricing (1.0× → 0.5× of `base_price`), product sell
pricing (2.0× → 1.0×), drone dispatch, contract spawn timing, ore
intake, fragment claim handling, ledger mutation. All of this is already
algorithmic and lives in current server code. In P2P every peer in the
shard computes it identically from `state_n` and the input batch. No
keys, no oracle, no host required. The station as economic actor falls
out of the lockstep sim for free.

### 2. Authored (deterministic, content-addressed)

Station persona text, hail templates, faction strings, MOTD, contract
narrative templates. This is just *content*, baked into the world (for
seeded stations) or attached when the station was planted (for outposts).
It lives in the shard state and is hashed into the state root like any
other data. Anyone can verify it; nobody has to generate it at runtime.

### 3. Inferred (non-deterministic, paid)

LLM-generated utterances: situational hail responses, custom contract
text, AI negotiation, novel pricing calls during edge conditions, station
diplomacy. This is the **only** layer that needs an actual compute
provider, and it's the question the design has to answer: who supplies
the AI?

## The cognition market

Make the station pay for its own thinking. Concretely:

- Each station has a **treasury** in its own currency (already implicit
  in `station_credit_pool()`).
- When the sim emits an inference job — triggered by a player hail, a
  contract negotiation, a strategic re-pricing — the job is broadcast on
  the shard mesh as `(station_id, model_id, prompt_hash, seed, max_tokens,
  bounty_in_local_credits, deadline_tick)`.
- Any peer in the shard can pick up the job, run the inference locally,
  sign the result, and submit it. The bounty is paid from the station's
  treasury into the providing peer's ledger *at that station* — fully
  consistent with the existing per-station credit model.
- Because `(model_id, prompt_hash, seed)` is pinned, the output is
  reproducible. A second peer can re-run and verify. First valid signed
  completion wins; a peer that submits garbage is caught on re-execution
  and forfeits.

Two properties fall out of this:

- **Cognition is endogenously funded.** A station that wants to be smart
  has to be valuable enough that someone is willing to be paid in its
  currency to compute for it. Stations whose AI degrades because they
  can't afford inference become less interesting to visit, which
  depreciates their currency further, which makes them harder to think
  for. The death spiral is a real market force, not a coded mechanic.
  This is exactly on-theme: a frontier station whose books are bad
  literally cannot think.
- **No privileged peer required.** Any client in the shard can be the
  inference provider. The same player mining your asteroids might be
  running your hail responses for pocket change. "I'm Helios' brain this
  session" becomes a flex.

### The deterministic floor

Every station always has a **baseline policy** — pure algorithm, runs in
the sim, no LLM needed. If nobody picks up the bounty in time, the
station falls back to baseline: scripted hails, formulaic contracts,
mechanical prices. **Stations never go silent; they go boring when
broke.** That's the right failure mode for a frontier economy.

This also solves bootstrap. A freshly planted outpost has zero treasury
and runs on baseline only. It earns currency by being mechanically useful
(smelt, fab, host). Once it has revenue, the planter chooses how much to
spend on cognition. Player-built stations effectively decide: how clever
do I want my outpost to be, and can I afford it?

## What this kills

- **"Who supplies the AI?"** — a paid service, anyone in the shard can
  provide.
- **"Who holds the station key?"** — no operator key. Seeded stations
  derive identity from world seed (already true,
  [`server/station_authority.c`](../server/station_authority.c)); outposts
  derive identity from `(founder_pub, name, planted_tick)` (already true).
  Mechanical action needs no signature — it's computed by every peer.
  Only LLM outputs are signed, and they're signed by whoever provided
  the inference, not by the station.
- **"What if the host goes offline?"** — there is no host. The station
  *is* the deterministic policy in the sim. Cognition is best-effort,
  baseline is always-on, the station is never "down."

## Cargo and ledgers in P2P

The existing identity stack already does most of the work:

- **`cargo_unit_t.pub` and `parent_merkle`**
  ([`shared/types.h`](../shared/types.h)) make every ingot and finished
  good a content-addressed object with verifiable provenance. In P2P this
  becomes the canonical record of ownership: any peer can validate a
  crate's history without trusting any other peer.
- **Per-station ledgers** are per-shard state. `(station_id,
  player_pubkey) → balance` is mutated by signed inputs (sell, buy,
  contract complete) and witnessed by everyone in the shard.
- **Cross-station transfer** stays goods-only — exactly as it is today.
  The hauler is the bridge between shards. There is no global wallet,
  there is no global currency, and the FX desk is the player carrying
  ore through a third currency zone.
- **Issue #480** (cross-currency wrapping) becomes: any peer publishes a
  swap order on a public market shard, and whoever physically hauls the
  goods earns the spread.

The chain log surface in
[`server/chain_log.h`](../server/chain_log.h) keeps its schema; the
authority field stops meaning "this station's operator's key" and starts
meaning "this shard's commit certificate" for global-effect events. Local
events (smelt, craft, transfer, ledger) still have a natural single
authority — the station's deterministic identity — and don't need
multisig.

## Persistence

No `world.sav` on a server. Instead:

- Each shard's signed state-root is the canonical pointer.
- Snapshots are content-addressed (BLAKE3 over the fixed-point state
  dump), shared via a DHT. The hash of an epoch's state root *is* its
  address.
- A new player joining shard X: pull the latest snapshot whose root has
  ≥2/3 commit signatures, replay recent inputs from the gossip log to
  reach head, join the mesh.
- Long-term archive: voluntary archive peers store full history. They
  can charge — in any station's currency — for serving old snapshots.
  Archival becomes a market, not infrastructure. Cargo provenance
  auditing, replay highscores, and Layer E verifier runs all consume
  this market.

`saves/pubkey/<base58>.sav` becomes a *local cache* of the player's
shard memberships and signing keys, not a source of truth. The keypair
storage path in [`client/identity.h`](../client/identity.h) keeps its
current semantics — losing your identity file still loses your saves —
but losing the server you happened to be connected to no longer loses
anything.

## Anti-cheat, sybil, fairness

- **State cheating** is free to defend against, because lockstep
  determinism means a peer that lies about state diverges immediately and
  is dropped from the shard at the next commit.
- **Wallhacks** are bounded by signal range. Peers outside your shard
  don't have your shard's state. Within-shard, nothing is hidden — a
  rock under tractor tension is observably moving and everyone can see
  it. This matches the lore: signal range *is* the information boundary.
- **Combat fairness** is free. Tractor-released fragment damage is a
  pure function of `f(state, inputs)`. If the sim says you got hit,
  every peer — yours, theirs, witnesses' — agrees. There is no "lag
  compensation" debate because there is no privileged frame of reference.
- **Sybil** is the genuinely open question. New pubkeys are free (it's
  just an Ed25519 keygen), so the *spawn* must cost something. Candidate
  costs:
  - 60s grace period where the new identity can't produce or trade
  - small proof-of-work or token-burn to dock at any T1+ station for
    the first time
  - outposts maintain spawn-allow lists; player-built spawn rights
    become a real social asset
  None of these are mutually exclusive. The right answer is probably
  layered.
- **Liveness.** If too many shard members go AFK, the shard "freezes" at
  its last commit. Any active peer can petition a neighboring shard to
  absorb the frozen shard's state. This is what dead frontier outposts
  look like in practice.

## What the current ratimics.com server becomes

A **seed peer**. A well-known, well-resourced node that helps new players
bootstrap (gives them a recent snapshot, introduces them to shard
meshes). It has no special authority. Anyone can run another seed. The
current `client/local_server.c` in-process sim — already used for
singleplayer — becomes the default everywhere: every client is its own
server, and the network is just other people doing the same.

The seeded stations (Prospect, Kepler, Helios) keep their world-seed-
derived identities; their cognition runs on whatever peers volunteer or
get paid. ratimics.com is the most likely first volunteer.

## What gets reused from the current codebase

Most of the cryptographic substrate. The current federation design and
the P2P design are not opposites — federation can ship now and gradually
relax into P2P as the determinism work lands.

| Concern | Federation (today) | Pure P2P (this doc) |
| --- | --- | --- |
| Player identity | Ed25519, local file | Same |
| Player-action authority | Signed input, server checks | Signed input, every peer checks |
| Asteroid / fragment identity | Deterministic from world seed | Same |
| Cargo identity | `cargo_unit_t.pub` content hash | Same |
| Cargo provenance | `parent_merkle` + signed receipt chain | Same |
| Station identity | Deterministic Ed25519 keypair per station | Same; key is for *content authoring*, not operator authority |
| Station mechanical behavior | Server code, deterministic | Same code, run by every shard member |
| Station cognition | Server-side, free, unstructured | Bountied LLM jobs, treasury-funded |
| Per-station event log | Signed by operator key | Signed by shard commit certificate for cross-shard events; signed by deterministic station identity for local events |
| Save record key | Player pubkey | Same |
| World persistence | `world.sav` on server, signed chain logs | Content-addressed snapshots in DHT, signed state roots |
| Cross-zone settlement | Cargo receipt chain | Same, witnessed by both shards' commits |
| Verifier tool | `signal_verify` walks one operator's log | Same tool walks a snapshot + epoch commit certificates |

The chain log schema in
[`server/chain_log.h`](../server/chain_log.h) does not need to change.
The handler that produces signatures changes: it stops producing one
operator's signature and starts producing the shard's commit certificate
when the event has cross-shard effect.

## Open problems

These are the corners that would need real design work before any of
this could be implemented, not just hand-waving:

1. **Determinism in C with current physics.** Replacing every FP math
   site in the sim — physics integration, tractor force, smelter beam
   geometry, signal-range curves — with fixed-point is bounded but
   substantial. Cross-platform determinism (native, wasm, ARM, x86)
   needs an explicit test harness that compares state roots across
   builds at known ticks.
2. **Cross-shard handoff atomicity.** A ship crossing a signal boundary
   carrying high-value cargo is the interesting attack surface. The
   handoff transcript needs to commit atomically in both shards' next
   state roots, with a clean rollback path if either side rejects. The
   "ship caught mid-jump between shards" failure mode wants explicit
   design.
3. **Inference verifiability.** Pinning `(model_id, prompt_hash, seed)`
   makes results reproducible *in principle*, but model providers don't
   currently expose deterministic decoding with cryptographically pinned
   weights. A near-term workaround is "winner-takes-all by latency,
   challenger can demand re-execution at provider's cost." A long-term
   answer probably involves attested model-weight hashes or zk inference
   proofs.
4. **Sybil layering.** The cheapest defense against spawn spam is some
   small economic cost per new identity. The exact mix
   (PoW / token-burn / social-vouching / outpost-allowlist) wants game-
   design playtesting, not just protocol design.
5. **NPC haulers and seeded contracts.** These currently run as server
   policy. In P2P they fall under "mechanical" station behavior — every
   peer computes them — but the seed structure for their pubkeys needs
   to be pinned to the station's deterministic identity so two peers
   never disagree about which drone is which.
6. **Episode / cutscene scheduling.** Currently triggered by server
   state. Trigger points need to be deterministic functions of shard
   state so every peer schedules the same cutscene at the same tick.
7. **Bandwidth budget per shard.** ≤16 active players × 120 Hz × input
   size needs to fit a residential uplink. Input batching, delta
   encoding, and adaptive tick rates probably all show up.

## What this design is not

- **Not a Solana port.** Per-station ledgers are explicitly *not* a
  global on-chain token. Wrapping per-station credits into transferable
  tokens for off-game value is the #480 work and is intentionally
  out-of-scope here.
- **Not a replacement for federation.** The federation model in
  [`decentralization.md`](./decentralization.md) is shippable today; this
  design is shippable only after the determinism rewrite. The two can
  coexist — federation peers can publish snapshots that P2P peers
  consume, and P2P peers can hail federated stations as remote shards.
- **Not an L1.** There is no global block, no global token, no global
  validator set. The "blockchain" in Signal-P2P is per-shard signed
  state-root chains, anchored to the world seed and to each other only
  via cargo handoffs.
- **Not a roadmap.** Order of operations, milestone breakdown, and
  team-allocation questions are explicitly out of scope. This document
  is "what would the end state look like," not "how do we get there."

## Reading list

- [`docs/decentralization.md`](./decentralization.md) — the federation
  design this document complements.
- [`docs/cargo-architecture.md`](./cargo-architecture.md) — the three-
  state cargo model that already supplies P2P-friendly provenance.
- [`/CLAUDE.md`](../CLAUDE.md) — repo-level architecture notes, including
  the canonical "Economy: per-station credits" text and the "Stations are
  sovereign currency issuers" framing this design takes as input.
- [`server/chain_log.h`](../server/chain_log.h) — the signed event log,
  reused unchanged in P2P with a different signature semantics.
- [`server/station_authority.h`](../server/station_authority.h) — the
  station keypair derivation, reused unchanged in P2P.
- Issue #479 — off-chain decentralization umbrella.
- Issue #480 — on-chain anchoring follow-on (the cross-currency wrap
  layer this design's "market shard" gestures at).
