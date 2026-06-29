# Signal Central Intelligence Vision

**Status:** direction / implementation triage after removing the GGUF/Wllama
hail path.

## Thesis

Signal should have one intelligence spine, not a pile of unrelated brains.

The rule is simple:

> C owns world truth and legal choices. Neural backends rank, phrase, or bias
> bounded choices. Station chains sign durable truth. Ships transport memory.

This keeps Signal's best property intact: the game remains a small C11/Sokol
simulation that builds to native and WebAssembly, while intelligence becomes a
portable subsystem instead of a platform dependency.

## Backend Split

### NSRL

Use `~/Documents/nsrl` for low-frequency language and station voice:

- station hail copy
- ship radio chirps
- operator posts
- chain-signed station announcements
- bounded hail-choice selection

NSRL should not free-write world facts. The C simulation builds a grounded
context window from station, route, cargo, memory, and reputation state. NSRL
returns either a compact choice assignment or a short line whose claims fit the
given facts.

Best first target: the existing ship-radio corpus. Its protocol is already the
right shape: C enumerates legal lines; the model chooses or lightly phrases
inside that envelope.

### CRLPLRIMES

Use `~/develop/crlplrimes` for numeric rankers:

- flight/reflex scoring
- tactical mining choices
- worker assignment
- contract/action ranking
- route/source selection
- station-chain frontier policy

The generated `client/integration/work/signal/*` static C bundles are the
right artifact shape for Signal: fixed feature contracts, no heap, no parser,
no temp files, no runtime dependency. The server still has older `.nnckpt`
loaders, so the next bridge is making `signal_intelligence` route both
checkpoint-era backends and generated static bundles behind one API.

## Current Code Shape

Signal already has the right decomposition:

- `server/signal_intelligence.*` is now the central C-facing facade.
- `server/signal_brain.*` owns flight and holographic pilot behavior.
- `server/signal_contract_brain.*` ranks contract/action candidates.
- `server/signal_npc_worker_brain.*` ranks worker options.
- `server/sim_ai.c` enumerates legal jobs, writes worker shadow traces, and
  keeps neural activation gated by mode and margin.
- `shared/npc_radio.*` already builds bounded hail/radio choices.
- `shared/types.h` already stores station hail copy and NPC job diagnostics.
- `/health` exposes backend names, inference counts, virtual pilots, and chain
  health.

The biggest architectural gap is not another model. It is a stable model
contract:

```text
world facts -> candidate features -> model score/choice -> sim applies legal action
```

Every neural head should fit that contract.

## Player-Facing Intelligence

The game cannot keep adding invisible cognition. Good AI choices look like bugs
when the player cannot see the reason.

The `[I]` surface should become an intelligence console with four compact
views. Avoid a giant debug dashboard; make it feel like a station/ship
instrument.

### Overview

- active backend names
- loaded model heads
- inference and decision counts
- closest station authority
- neural/teacher/shadow mode
- recent disagreement or activation rate

### Chain Command

- closest station
- station chain health
- controlled physical ships
- frontier virtual pilot count
- current station goals
- supply pressure and route pressure

### Neural Ships

- worker callsign
- home station
- role assignment
- current job
- destination
- top score / margin
- strongest why: contract, heard memory, proof, route, or hologram match
- last decision age

### Training / Trace

Do not pretend the game is training live unless it is. Show one of:

- loaded training artifact metadata and loss samples, when available
- live shadow metrics: heuristic vs neural disagreement, margins, activation
  rate, teacher fallback count, completion/failure outcomes
- station-scoped trace summaries from `SIGNAL_NPC_WORKER_TRACE`

This makes "training" visible without lying about runtime behavior.

## Scale Model

Yes, station chains can eventually command hundreds or thousands of ships, but
not by running a full per-frame neural pilot for every ship.

Use hierarchy:

```text
station chain commander
  -> station/route policy rankers
  -> squad or route controllers
  -> active nearby ships
  -> virtual pilots / aggregate logistics
```

The current `SIGNAL_FRONTIER_VIRTUAL_PILOTS` path proves the shape: large
strategic populations should appear as aggregate pressure first, then
materialize into physical ships only when they become relevant to the player,
a station, a route, or a proof event.

Target behavior:

- physical ships are high-fidelity actors near players and active stations
- far traffic runs as aggregate route pressure
- station chains issue order batches, not joystick commands
- neural rankers choose among legal station or route options
- C executes and verifies every mutation

## Multiplayer Model

Multiplayer makes the authority rule stricter:

> Models may run on clients for prediction or presentation, but only the server
> or station-operator path authors multiplayer truth.

Signal's protocol already has the right split:

- live world streams for players, NPCs, stations, cargo pods, and scaffolds
- relevance-filtered NPC and cargo-pod snapshots
- per-player private streams for ship state, manifests, delivery ledgers,
  inspect snapshots, and known-contract masks
- static station identity for station-authored text, services, policy, and
  public keys
- signed station chains for durable authority

The central intelligence layer should respect that split.

### Server-Authoritative Decisions

These must run on the server, or be validated and re-authored by the server:

- NPC worker assignment
- station/route/frontier planning
- contract/action choice that mutates station state
- station-authored hail copy that clients see as official
- any radio line that becomes a signed operator post or station-chain event
- any tactical choice that moves, pays, transfers, builds, or damages

Clients can render the result, but the mutation comes from authoritative C
state and signed station events.

### Client Prediction And Cosmetic Inference

These can run on the client because they do not create truth:

- local flight prediction for the player's own inputs
- UI previews and ranker explanations
- local-only radio flavor that is clearly not station-authored
- `[I]` console summaries over already-synchronized facts
- optional model-assisted line selection from legal candidates already supplied
  by the server

If a client-side model picks a chirp, the server must either have supplied the
full legal candidate set or treat the result as cosmetic only. A client model
must never be allowed to invent a station fact, hidden route, balance, proof,
or contract.

### Per-Player Visibility

Multiplayer intelligence has to be scoped to what that player can know.

Existing examples:

- `NET_MSG_PLAYER_KNOWN_CONTRACTS` hides work the player's ship has not heard
  about through gossip.
- `NET_MSG_INSPECT_SNAPSHOT` is per-player and scan-scoped, so proof/detail
  can be shown only when the player actively inspects a target.
- `NET_MSG_WORLD_NPCS` and cargo pods are relevance-filtered.
- station hail response is per-recipient and carries that player's balance at
  the responding station.

The `[I]` console should follow the same rule. In multiplayer, it should show
"your ship's intelligence picture," not an omniscient server dashboard, unless
the player is using an authenticated operator/debug endpoint.

### Station-Authored Radio

Official station voice should flow like current operator content:

```text
station state + chain/history context
  -> NSRL/operator model proposes text
  -> server validates bounds
  -> station command writes text
  -> CHAIN_EVT_OPERATOR_POST signs it
  -> station identity rebroadcasts it
```

This makes station copy multiplayer-safe. Everyone sees the same official
hail because the station authored it once. If a station forks its story, the
chain layer is where that fork becomes visible.

Local hail chatter can be lighter:

```text
player presses H
  -> server chooses visible station/NPC context for that player
  -> server returns candidate assignments or compact chirp records
  -> client displays them with clarity based on confidence/freshness
```

For WebAssembly builds, NSRL can still be compiled into the client for offline
single-player. In multiplayer, the official station path remains server-side or
operator-side.

### Scaling Multiplayer Ships

The physical network should not broadcast thousands of full NPCs.

Use layers:

- full NPC records only for relevant nearby physical ships
- aggregate station/route pressure for far traffic
- station-chain summaries for commanded virtual fleets
- per-player inspect expansion when the player targets a ship, route, or proof
- operator/debug APIs for whole-server fleet views

This keeps the client bandwidth shaped like the player's local signal picture,
not the server's total population.

### Multiplayer Acceptance Rules

- two clients watching the same official station hail see the same signed text
- two clients with different gossip histories may see different known contracts
  and different `[I]` intelligence summaries
- no client-side model output can create money, cargo, proof, damage, or chain
  history
- any generated text that becomes public station identity is validated,
  station-authored, and chain-signed
- active neural worker choices remain replayable across native and WebAssembly
  server-side simulation gates

## High-Leverage Gaps

### 1. Generated Static Brain Bridge

Wire the CRLPLRIMES generated static C bundles into `signal_intelligence`.

Why it matters: this removes the remaining distinction between "old checkpoint
brain" and "real Signal neural engine." It also gives native and WebAssembly
the same dependency-free model path.

Acceptance:

- `signal_intelligence_backend_name()` reports generated heads when linked
- flight can use `signal_client_flight` without writing a temp checkpoint
- strategic worker v2 has an explicit feature-contract bridge from current
  worker candidates
- `/health` exposes model hashes and feature sets

### 2. Bounded NSRL Radio Bridge

Create a C ABI for NSRL that serves radio tasks only.

Why it matters: it replaces the removed GGUF hail path with the architecture
Signal actually wants: tiny, deterministic, fact-bounded language.

Acceptance:

- C builds a compact radio context from live world state
- NSRL returns bounded assignments or one short chirp
- C validates output before applying it
- native and WebAssembly can use the same model artifact strategy
- fallback C radio remains available when no NSRL model is linked

### 3. `[I]` Intelligence Console

Add a first in-game intelligence surface.

Why it matters: centralizing intelligence in code is only half the work. The
player needs to see what the neural economy is doing.

Acceptance:

- undocked `[I]` opens a compact intelligence overlay
- docked station UI gets an `INTEL` tab rather than stealing the shipyard `[I]`
  commission key
- the first version shows backend status, closest station, controlled workers,
  and live ranker counters
- trace/loss graphs can be added after data is present

### 4. Contact Card Stabilization

Turn the current scan/inspect diagnostics into a stable first-read contact
card.

Why it matters: NPCs are the cast in solo play. The sim already knows cargo,
destination, job reason, memory source, proof, and route hints; the first read
still asks the player to assemble the story.

Acceptance:

- scanning a worker leads with callsign, assignment, cargo/destination, and
  strongest motive
- uncertainty is rendered with the existing clarity grammar
- proof/detail remains available in inspect rows

### 5. Active-Worker Determinism Gate

Keep the replay gate as the promotion boundary for active worker intelligence.
The fast set now includes a selected worker/HNN diagnostic fixture, the long set
keeps workers alive long enough to exchange gossip and HNN market memory, and
CI can compare native and WASM replay before merge.

Why it matters: this is the safety harness for the whole direction. The normal
flight/economy replay gates are not enough to prove the active intelligence
economy.

Acceptance:

- native and WASM replay agree over active-worker scenarios
- the fast gate includes selected worker tow diagnostics, HNN-backed rows,
  scaffold pickup counters, and shared job-family outcome telemetry
- long/native and named long native-WASM modes include physical gossip
  transport, worker activity counters, and HNN market exchange before active
  model promotion

### 6. Station Memory Summary

Make stations read like institutions on first dock.

Why it matters: HISTORY exists, but station memory should be felt before it is
browsed.

Acceptance:

- dock header can say `Known for: ...` from route history or local evidence
- dock header can say `You here: ...` from station-local ledger/history
- signed truth, carried gossip, and fuzzy reputation stay visually distinct

### 7. Remote Receipt Anchor Retrieval

Complete the proof-following story when an anchor is known but the local chain
is missing.

Why it matters: worker motives and HNN resonance stay trustworthy only if the
player can follow proof when the sim claims proof exists.

Acceptance:

- selected worker job can expand from job reason to structured memory to
  receipt chain where available
- missing remote links have a clear retrieval/browse path
- main UI stays compact; deep proof lives in focused inspect/history surfaces

### 8. Multiplayer Intelligence Streams

Add only the narrow wire surfaces needed for player-visible intelligence.

Why it matters: single-player can read the whole local world; multiplayer must
not accidentally turn intelligence UI into an omniscient debug panel.

Acceptance:

- `[I]` can render from per-player synced facts and inspect snapshots
- official station intelligence uses station identity, signal channel, or
  signed chain events
- private intelligence summaries are explicitly per-player
- protocol discovery advertises any new stream shape before external tools rely
  on it

## Near-Term Sequence

1. Static CRLPLRIMES brain bridge into `signal_intelligence`.
2. First `[I]` console backed by existing health/counter/worker data, scoped to
   player-visible facts in multiplayer.
3. NSRL bounded radio ABI and validation wrapper.
4. Server/operator path for station-authored NSRL copy.
5. Contact card stabilization using existing inspect labels.
6. Active-worker replay gate before increasing neural authority.

This order makes the architecture real, then makes it visible, then makes it
safe to give the models more responsibility.

## North Star

Signal's intelligence should feel like infrastructure, not magic.

The player should be able to look at a ship, station, route, or radio chirp and
answer:

- who knew this?
- how certain was it?
- what proof exists?
- what decision did it cause?
- what changed in the world because of it?

When that loop is visible, hundreds or thousands of neural ships stop feeling
like background bots and start feeling like a frontier society under station
command.
