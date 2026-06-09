# Signal: Sector One — Product Requirements Document

**Version:** 1.1
**Date:** 2026-06-08
**Status:** Shipped / Live (Sector One MVP)
**Live:** [signal.ratimics.com/play](https://signal.ratimics.com/play)
**Owner:** Signal Development Team

---

## 1. Executive Summary

**Signal** is a multiplayer space mining and construction sandbox about frontier economies, signal coverage, and the slow work of building an outpost network at the edge of charted space. Oh yeah, and you kill each other with rocks.

Unlike traditional space MMOs with global wallets and dedicated combat weapons, Signal enforces strict physical and economic constraints:
1. **The rock is the core interaction.** The mining laser is strictly a mining tool. There are no lasers, missiles, or turrets. The only weapon is a physical rock fragment gathered under tractor tension and released as a ballistic projectile.
2. **Stations are sovereign.** There is no global currency. Each station issues its own credits, maintains its own ledger, and signs its own append-only chain log. Value moves between zones as physical goods, making haulers the game's FX desks.
3. **Signal is the map.** Coverage determines mining speed, ship control responsiveness, and NPC logistics viability. Expansion literally means extending the signal network.

The game ships asset-light: procedural geometry, code-driven HUDs, and runtime-loaded media (MP3 music, PNG avatars, MPEG-1 episode cutscenes).

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

---

## 4. Key Systems & Mechanics

### 4.1 Mining & Fracture
- **Beam Mechanics:** Ray-cast intersection. Efficiency scales with signal quality (`0.2x` in void to `1.0x` in core).
- **Fracture Claims:** To prevent deterministic mining exploits, fracturing initiates a 1.5-second client-side "burst nonce" challenge. The server resolves the claim to the best nonce, awarding the `fragment_pub` derivation rights.
- **Tiers:** Mining level gates which asteroid tiers (XL, L, M, S) can be fractured.

### 4.2 Tractor & Physics-Based PvP
- **Towing:** Hold `Space` to tractor fragments or scaffolds. Players can tow up to 10 fragments (with upgrades); NPCs tow one.
- **The Throw:** Tapping `Space` releases the tow. The fragment retains the ship's velocity vector + a release impulse. This is the game's only offensive mechanic. Damage is calculated as `(impact_velocity - threshold) * scale`.
- **Attribution:** Released fragments carry the releasing ship's `session_token` for 30 seconds, enabling kill attribution (`DEATH_CAUSE_THROWN_ROCK`).

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

---

## 5. AI & NPCs

NPCs operate via state machines, augmented by experimental neural/holographic brains:
- **Miners:** Exit station → navigate to belt → fracture → tow fragment → return to smelter.
- **Haulers:** Scan known contracts → pick up cargo at source → navigate to destination → present receipt chain → return.
- **Tow Drones:** Autonomous scaffold delivery from shipyards to planned outposts.
- **Frontier Director:** A virtual logistics system that ranks expansion candidates, auto-plans outposts, and manages "virtual pilot" budgets to drive network growth without consuming physical network slots.

---

## 6. Multiplayer & Networking

### 6.1 Current Architecture
- **Authoritative Relay:** Single headless server (`signal_server`) running the 120Hz simulation. Clients connect via WebSocket.
- **Client Prediction:** Tick-addressed movement prediction with a 512-frame replay buffer. Server snapshots include `input_ack` and `server_tick` for drift reconciliation.

### 6.2 Target Architecture (P2P Browser Mesh)
- **Goal:** Decentralize the simulation authority to browser peers.
- **Blocker:** Floating-point non-determinism between native (x86/ARM) and WASM builds. 
- **Solution:** Complete rewrite of sim state and step functions to `q32.32` fixed-point arithmetic (Tracked as #588).

---

## 7. Decentralization & Trust

Signal implements a robust off-chain identity and audit stack:
1. **Deterministic Identities:** Asteroids (`rock_pub`), fragments (`fragment_pub`), and cargo (`cargo_unit.pub`) are content-addressed.
2. **Station Authority:** Each station has an Ed25519 keypair derived from an operator secret. Private keys are never serialized to disk.
3. **Player Identity:** Clients generate persistent Ed25519 keypairs. State-changing actions require Ed25519 signatures (`NET_MSG_SIGNED_ACTION`).
4. **Chain Logs:** Every state mutation emits a 184-byte signed, hash-chained event (`CHAIN_EVT_*`). Standalone `signal_verify` tool validates integrity.
5. **Cargo Receipts (Layer D):** Portable, station-signed transfer chains accompany cargo across zone boundaries, verified by the destination authority.
6. **Handoff Tickets:** Signed envelopes binding ship state + cargo roots for seamless cross-zone authority transfer.

*(Note: On-chain state-root anchoring and bounty payouts are deferred to Post-Sector One / Issue #480).*

---

## 8. Roadmap & Future Vision

### 8.1 Immediate Priorities (Sector One Hardening)
1. **#588 Fixed-Point Sim Rewrite:** Unblock P2P determinism.
2. **#285 Streaming Entity Pool:** Lift wire protocol caps (MAX_ASTEROIDS 2048 → unbounded, MAX_STATIONS 128 → unbounded).
3. **CI Expansion:** Add Windows, Emscripten, ASan/UBSan, and clang-tidy to GitHub Actions.
4. **Fuzzing:** Add libFuzzer harnesses for wire deserialization and save loading.

### 8.2 Post-Sector One Vision (Sector X)
- **Dark-Sector Battery Runs:** High-risk, high-yield zones beyond the mapped network.
- **Megastructures & Jump Gates:** Cosmic events that reassign chunk coordinates, requiring MMR (Merkle Mountain Range) provenance proofs.
- **On-Chain Bridging:** Solana smart contracts for state-root commitments, asset wrapping, and RATi Foundation bounty payouts.
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
| [docs/cargo-architecture.md](docs/cargo-architecture.md) | Canonical three-state cargo model |
| [docs/decentralization.md](docs/decentralization.md) | Federation architecture, identity stack, chain log |
| [docs/operator-onboarding.md](docs/operator-on and the rest of the docs | ... |

