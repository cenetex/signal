# Signal: Sector One — Product Requirements Document

**Version:** 1.3
**Date:** 2026-07-03
**Status:** Shipped / Live (Sector One MVP)
**Live:** [signal.ratimics.com/play](https://signal.ratimics.com/play)
**Owner:** Signal Development Team

---

## 1. Executive Summary

**Signal** is a physics-native civilization network disguised as an arcade
space-mining game. Players mine matter, stations sign history, communities
build signal, and the universe expands only when the network earns it. Oh yeah,
and you kill each other with rocks.

The visible product is the game loop: launch, fracture rocks, tow fragments, smelt, haul, build outposts, defend routes, and expand signal. The metaproduct is the machine underneath that turns meaningful physical play into verifiable history: content-addressed cargo, station-signed events, receipt chains, replayable state roots, and eventually portable permaweb settlement artifacts.

Signal is not "space mining," "blockchain Asteroids," or "procedural MMO."
Those are surfaces. The real product is a persistent frontier where matter,
labor, trust, infrastructure, and memory are all simulated.

North star: **every large-scale social structure in the game must be made out
of physical actions the players actually performed.** No abstract guild bases,
no global wallet teleporting value, no land-claim button, and no fake settlement
layer outside the play loop.

Unlike traditional space MMOs with global wallets and dedicated combat weapons, Signal enforces strict physical and economic constraints:
1. **The rock is the core interaction.** The mining laser is strictly a mining tool. There are no lasers, missiles, or turrets. The only weapon is a physical rock fragment gathered under tractor tension and released as a ballistic projectile.
2. **Stations are sovereign.** There is no global currency. Each station issues its own credits, maintains its own ledger, and signs its own append-only chain log. Value moves between zones as physical goods, making haulers the game's FX desks.
3. **Signal is the map.** Coverage determines mining speed, ship control responsiveness, and NPC logistics viability. Expansion literally means extending the signal network.

The game ships asset-light: procedural geometry, code-driven HUDs, and runtime-loaded media (MP3 music, PNG avatars, MPEG-1 episode cutscenes).

### 1.1 Product Stack

Signal should be understood as five product layers sharing one interface:

1. **Physics game:** rocks, thrust, tractor tension, signal, construction, and PvP.
2. **Provenance product:** fragments become named cargo at the smelt/craft boundary, carrying `pub`, `parent_merkle`, origin, and receipt state.
3. **Station-sovereignty product:** stations issue local credits, sign local history, and decide which cargo/contracts they recognize.
4. **Verification product:** `signal_verify`, `signal_chain_assets`, and `signal_replay` let auditors prove what happened after the fact.
5. **Settlement substrate:** per-station history becomes portable checkpoints, permaweb snapshots, federation/P2P manifests, and optional external-chain adapters.

The public hierarchy must stay in that order. Lead with rocks and stations; let proofs, settlement, and tokens emerge from play.

### 1.2 Civilization Ladder

The whole product scales from one primitive:

```text
rock
  -> fragment
  -> ingot
  -> frame
  -> outpost
  -> station
  -> route
  -> sector
  -> civilization
```

A station exists because someone hauled frames. A route exists because haulers
kept it alive. A currency has credibility because the station issuing it has
inventory, labor, history, and a signed record. A gate opens because a whole
region coordinated enough matter, risk, and trust to cross a threshold.

---

## 2. Target Audience & Player Personas

| Persona | Primary Goal | Gameplay Focus |
|---|---|---|
| **The Prospector** | Maximize short-term yield | Fracturing XL asteroids, efficiently towing fragments to Prospect Refinery, upgrading mining laser and cargo hold. |
| **The Hauler** | Arbitrage and contract completion | Monitoring station供需 (supply/demand), fulfilling cross-station delivery contracts, building a reputation for reliable freight. |
| **The Architect** | Network expansion | Planning outpost locations at the signal fringe, ordering scaffold kits, tractor-towing them into place, and defending the construction site. |
| **The Privateer** | Disruption and PvP | Intercepting haulers, tractor-stealing valuable fragments, and using "thrown rocks" to hull-crush rival pilots. |

---

## 3. Core Gameplay Loops

### 3.1 The 5-Minute Loop (Micro)
1. Launch from station dock.
2. Navigate to asteroid belt (guided by signal strength and scan hail).
3. Fire mining beam to fracture an asteroid (XL → L → M → S).
4. Tractor-sweep fragments into the ship's tow tether.
5. Return to station and dock into the smelter beam.

### 3.2 The 30-Minute Loop (Meso)
1. Smelt fragments into ingots (Ferrite, Cuprite, Crystal).
2. Sell ingots to replenish station-specific credits.
3. Purchase a ship upgrade (hold capacity, tractor range, mining tier) or a scaffold kit.
4. Enter Plan Mode (`B`), reserve an outpost slot at the edge of current signal, and tractor-tow the scaffold to the location.
5. Deliver frames to activate the new outpost, extending the network.

### 3.3 The Session Loop (Macro)
1. Establish a profitable supply chain between specialized stations (e.g., Prospect Ferrite → Kepler Frames → Helios Lasers).
2. Fulfill high-tier heritage contracts requiring specific cargo provenance (e.g., "RATi-grade cuprite ingots not originating from Station X").
3. Defend your haul route from privateers using rock-ballistics and superior tractor maneuvering.
4. Die, pay the respawn fee (which can drive your ledger into debt), and rebuild.

### 3.4 The Metaproduct Loop
1. Play produces a physical transformation.
2. The transformation crosses a boundary the system cares about: fracture, smelt, craft, transfer, delivery, construction, or death.
3. The resulting object or milestone receives content identity.
4. A station signs the local event into its append-only history.
5. Receipts and checkpoints make that history portable to other stations, auditors, peers, and eventually permaweb anchors.

The strongest player-facing promise is: **your labor leaves durable traces in the world.**

### 3.5 The Civilization Arc

The player fantasy is not "you are a hero in a vast universe." It is: **you are
a worker in a fragile universe that only becomes vast if everyone builds it.**

The emotional progression:

1. I am surviving.
2. I am earning.
3. I am hauling.
4. I am building.
5. I am trusted.
6. I am part of a route.
7. I am part of a station.
8. I am part of a sector.
9. I am part of the network.
10. We can cross the dark.

### 3.6 Sector One Level Design

Sector One should read as a damaged network, not a neutral starter triangle.
The first map exists to teach that civilization is built from signal, routes,
matter, and local trust.

The starting topology:

1. **Prospect Basin:** safe inner refinery space with reliable signal, ferrite
   work, early mining, and first smelting.
2. **Kepler Yard:** the industrial shelf above Prospect, where frames,
   scaffolds, and corridor repair become the player's first infrastructure
   responsibility.
3. **Broken Helios Corridor:** the old boosted relay lane to Helios. Fully
   repaired, the route should be straightforward; damaged, it creates a
   low-signal gap that makes hauling, patrols, and reconstruction matter.
4. **Blackglass Freeport:** an off-relay pirate station in the gap. It buys
   cargo as-is, creates risk and temptation, and proves that low-signal space
   is a political biome, not just empty distance.
5. **Helios Fringe:** advanced production beyond the broken corridor. Helios
   is reachable, but its isolation gives the player a reason to care about
   restoring signal instead of merely optimizing travel.

The first map objective is therefore not "visit all stations." It is: restore
the Helios corridor through physical construction, verified cargo deliveries,
and defended routes.

---

## 4. Key Systems & Mechanics

### 4.1 Mining & Fracture
- **Beam Mechanics:** Ray-cast intersection. Efficiency scales with signal quality (`0.2x` in void to `1.0x` in core).
- **Fracture Claims:** To prevent deterministic mining exploits, fracturing initiates a 1.5-second client-side "burst nonce" challenge. The server resolves the claim to the best nonce, awarding the `fragment_pub` derivation rights.
- **Tiers:** Mining level gates which asteroid tiers (XL, L, M, S) can be fractured.

### 4.2 Tractor & Physics-Based PvP
- **Towing:** Hold `Space` to tractor fragments or scaffolds. Players can tow up to 10 fragments (with upgrades); NPCs tow one.
- **The Throw:** Tapping `Space` releases the tow. The fragment retains the ship's velocity vector + a release impulse. This is the game's only offensive mechanic. Damage is calculated as `(impact_velocity - threshold) * scale`.
- **Attribution:** Released fragments carry the releasing ship's `session_token` for 6 seconds, enabling kill attribution (`DEATH_CAUSE_THROWN_ROCK`) during the ballistic combat window.

### 4.3 The Three-State Cargo Model
Matter exists in three distinct states, enforcing a strict provenance boundary at the smelter:
1. **Fragment:** Physical `asteroid_t` in space. Identity = `fragment_pub` (content hash). Not in any manifest.
2. **Bulk Float:** Ephemeral station `_inventory_cache` buffer for raw ore. No identity, no chain entry. (Legacy/vestigial in normal play).
3. **Crate:** Named `cargo_unit_t` in a ship/station manifest. Identity = `pub` (content hash) + `parent_merkle` (sorted input roots). Born at the smelt/craft boundary.

### 4.4 Sovereign Station Economy
- **Per-Station Ledgers:** Balances are keyed by `(station_id, player_pubkey)`. Credits earned at Prospect cannot be spent at Helios.
- **Dynamic Pricing:** 
  - *Buy Price:* Scales `1.0x` (empty hopper) → `0.5x` (full hopper).
  - *Sell Price:* Scales `2.0x` (empty stock) → `1.0x` (full stock).
- **Prefix-Class Multipliers:** Named ingots carry a prefix (e.g., `RATi`, `K`). Anonymous = `1.0x`, single-letter = `2.0x`, `RATi` = `50.0x`, Commissioned = `100.0x`.
- **No Money Cap:** A station's credit pool can go arbitrarily negative. The economy is backed by physical goods, not fiat reserves.

### 4.5 Signal Coverage
Signal quality (`0.0` to `1.0`) dictates operational viability:
- **FRONTIER (0.0–0.15):** No NPC support, ~0-15% control responsiveness, boundary push toward coverage.
- **FRINGE (0.15–0.50):** Graduated NPC confidence, moderate control penalty.
- **OPERATIONAL (0.50–0.80):** Normal NPC operation, reasonable mining efficiency.
- **CORE (0.80–1.00):** Full efficiency, no penalties.
- *Visuals:* World geometry fades to grayscale in low signal. Critical UI cues (borders, beams) retain color saturation (min 72%).

### 4.6 Construction & Outposts
1. **Plan:** Undocked, press `B` to reserve a module slot or plant an outpost ghost.
2. **Order:** Dock at a shipyard, purchase an unlocked scaffold kit.
3. **Manufacture:** Station inventory feeds the nascent scaffold until complete.
4. **Tow & Place:** Tractor the loose scaffold through space, press `E` to snap to a ring slot or found the outpost.
5. **Activate:** Placed modules enter a 10-second supply/commissioning timer. New outposts require frame delivery to activate.
- *Constraint:* Outposts can only be placed in signal quality `< 0.80` (FRINGE/FRONTIER) to force network expansion, not stacking.

### 4.7 World Primitives

The long-term game is built from five primitives:

1. **Matter:** rocks, fragments, ingots, frames, modules, crystals, scaffolds.
2. **Signal:** the civilizational boundary where control, communication, NPC support, visibility, and safety exist.
3. **Stations:** sovereign local institutions that issue credits, maintain ledgers, sign history, price goods, and express personality.
4. **Receipts:** portable trust attached to custody, origin, and transformation.
5. **Memory:** permanent consequence: destroyed rocks stay destroyed, chain logs remember events, and player labor becomes historical texture.

---

## 5. AI & NPCs

NPCs are moving toward a neural worker model: workers exchange contract gossip
and market memory when they dock, score job offers from local pressure, then
resolve any chosen work against exact station authority. "Miner," "hauler," and
"tow" are assignment outcomes, not permanent identities.

Current worker assignment families:

- **Mining:** Exit station -> navigate to belt -> fracture -> tow fragment ->
  return to smelter.
- **Hauling:** Hear demand/supply/route pressure -> pick up cargo at source ->
  deliver to destination -> present receipt chain.
- **Delivery proof:** Carry already-bound shipment proof through the delivery
  ledger, clear debt at the origin, and emit receipt-backed route memory.
- **Scaffold tow:** Acquire scaffold work from shipyard/construction pressure
  and tow it to the planned slot or outpost.
- **Scout/fracture:** Respond to fracture demand, route danger, or stuck-worker
  pressure.
- **Repair:** Bias toward repair work when repair-kit supply and damaged-worker
  signals make it worthwhile.
- **Frontier Director:** A virtual logistics system that ranks expansion
  candidates, auto-plans outposts, and manages "virtual pilot" budgets to drive
  network growth without consuming physical network slots.

Holographic memory is advisory here: it can make a kind of work feel familiar
to a worker, but contracts, manifests, receipt chains, station ledgers, and
chain logs decide what is actually true.

Stations should become semi-autonomous institutions with memory, policy, and
voice. Station AI should not invent drama; it should interpret the drama already
produced by shortages, route failures, trusted haulers, privateer attacks,
dark-sector discoveries, debt crises, outpost abandonment, and gate progress.

---

## 6. Multiplayer & Networking

### 6.1 Current Architecture
- **Authoritative Relay:** Single headless server (`signal_server`) running the 120Hz simulation. Clients connect via WebSocket.
- **Client Prediction:** Tick-addressed movement prediction with a 512-frame replay buffer. Server snapshots include `input_ack` and `server_tick` for drift reconciliation.

### 6.2 Target Architecture (P2P Browser Mesh)
- **Goal:** Decentralize the simulation authority to browser peers.
- **Current determinism ratchet:** `signal_replay` now hashes exact IEEE-754 bits, runs native↔WASM replay gates, and includes long-horizon probes for settlement-critical scenarios.
- **Remaining blocker:** #588 still owns the durable answer for cross-platform authority. The shipped path is strict IEEE-754 replay with native/WASM comparison; a full `q32.32` sim-state migration is not built. The project needs an explicit decision to either graduate the strict-float ratchet with a wider platform/scenario matrix or resume fixed-point migration for settlement-critical state.
- **Mesh dependency order:** deterministic replay → canonical settlement events → client state-root comparison → Arweave/permaweb bootstrap → WebRTC quorum mesh.

---

## 7. Decentralization & Trust

Signal implements a robust off-chain identity and audit stack:
1. **Deterministic Identities:** Asteroids (`rock_pub`), fragments (`fragment_pub`), and cargo (`cargo_unit.pub`) are content-addressed.
2. **Station Authority:** Each station has an Ed25519 keypair derived from an operator secret. Private keys are never serialized to disk.
3. **Player Identity:** Clients generate persistent Ed25519 keypairs. State-changing actions require Ed25519 signatures (`NET_MSG_SIGNED_ACTION`).
4. **Chain Logs:** Every state mutation emits a 184-byte signed, hash-chained event (`CHAIN_EVT_*`). Standalone `signal_verify` tool validates integrity.
5. **Cargo Receipts (Layer D):** Portable, station-signed transfer chains accompany cargo across zone boundaries, verified by the destination authority.
6. **Handoff Tickets:** Signed envelopes binding ship state + cargo roots for seamless cross-zone authority transfer.

RATi is treated as a cross-world identity and provenance namespace, not a normal in-world wallet. A RATi-bearing vessel is the local embodiment of a persistent bearer identity: worlds can reset or fork, but the identity primitive and its recognized provenance survive as portable history.

Arweave/permaweb anchoring is the core long-term persistence direction for settlement artifacts. Solana-style state-root commitments, burn-to-mint programs, and bounty payouts remain valid external adapters, but they do not define the core product contract.

---

## 8. Roadmap & Future Vision

### 8.1 Groomed Backlog

Priority is ordered by metaproduct leverage: first make object history unavoidable, then make settlement canonical, then make that history portable.

1. **Metaproduct alignment:** Keep this PRD, [ENG.md](ENG.md), [docs/metaproduct.md](docs/metaproduct.md), and [docs/decentralization-synthesis.md](docs/decentralization-synthesis.md) aligned around "physical play produces verifiable history."
2. **#588 determinism acceptance:** Decide whether the strict native↔WASM replay ratchet is the accepted substrate after broader platform/scenario coverage, or whether full `q32.32` remains mandatory before P2P work can proceed.
3. **#340 / #339 manifest authority:** Make buy, sell, deliver, and production paths move concrete `cargo_unit_t` rows by default, then retire finished-goods float authority.
4. **Lineage view:** Make rock -> fragment -> ingot -> frame -> outpost/gate contribution inspectable as the killer demo.
5. **#587 typed provenance contracts:** Add explicit target pubkeys and fracture/death fulfillment so contracts can price witnessed events, not only aggregate commodities.
6. **#354 / #355 / #356 settlement bridge:** Emit canonical settlement events for validated game actions, construction milestones, and signal-channel continuity.
7. **Player-facing legibility:** Surface cargo lineage, local ledger history, and station-authored provenance in the docked UI so the player can see what the substrate remembers.
8. **Institution tools:** Add shared contracts, escrowed cargo, station-endorsed bounties, route health dashboards, and public construction manifests.
9. **#294 unified ship/controller model:** Retire parallel NPC cargo paths that can drift from player manifest semantics.
10. **#590 / #591 / #589 permaweb + mesh:** Bootstrap clients from Arweave snapshots/logs, then add WebRTC state-root comparison and quorum behavior.
11. **#496 RATi vessel identity:** Bind cross-world RATi identity to substrate-born vessels once manifest transfers and settlement events are canonical.
12. **#285 streaming entity pool:** Lift hard caps after the core economic/provenance path is settled. Cap lifting is additionally gated on the sim hot-path fixes in [docs/optimization-report.md](docs/optimization-report.md) — several 120 Hz loops scale as `entities × stations`, so raising `MAX_STATIONS`/`MAX_ASTEROIDS` without them degrades tick budget quadratically.

### 8.2 Post-Sector One Vision (Sector X)
- **Dark-Sector Battery Runs:** High-risk zones beyond institutional reality: no easy comms, no instant market quote, no NPC safety net, no station guarantee, no complete telemetry, and no automatic trust.
- **Megastructures & Jump Gates:** Shared infrastructure projects requiring massive frame production, rare dark-sector crystal runs, multi-station contracts, verified provenance, defended routes, and public contribution history.
- **Permaweb Settlement:** Station histories, checkpoint roots, and discovery manifests become durable Arweave artifacts that clients can verify and replay from.
- **External-Chain Adapters:** Solana smart contracts can wrap state roots, assets, and RATi Foundation bounty payouts, but remain adapters over Signal's native per-station history.
- **Neural Scorers:** Replace deterministic baseline station policy with trained neural contract scorers, auditable via `/training/v1/station-policy-trace`.

---

## 9. Document Index

| Document | Purpose |
|---|---|
| [ENG.md](ENG.md) | Engineering design, architecture, and implementation details |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Human-facing architecture reference |
| [CLAUDE.md](CLAUDE.md) | AI-oriented context, build commands, working style |
| [CONSTRUCTION_PLAN.md](CONSTRUCTION_PLAN.md) | Construction loop design and implementation status |
| [REMEDIATION_PLAN.md](REMEDIATION_PLAN.md) | Active improvement plan, ranked by impact/effort |
| [docs/metaproduct.md](docs/metaproduct.md) | Canonical product-stack and backlog framing |
| [docs/cargo-architecture.md](docs/cargo-architecture.md) | Canonical three-state cargo model |
| [docs/decentralization.md](docs/decentralization.md) | Federation architecture, identity stack, chain log |
| [docs/decentralization-synthesis.md](docs/decentralization-synthesis.md) | Federation/P2P synthesis and authority model |
| [docs/optimization-report.md](docs/optimization-report.md) | Sim/render hot-path analysis and ranked fix plan |
| [docs/operator-onboarding.md](docs/operator-onboarding.md) | Station operator guide |
