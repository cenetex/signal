# Signal — Architecture

This is the human-facing architecture reference. For AI-oriented context, see
[CLAUDE.md](/CLAUDE.md).

## Build Targets

One CMake project, three targets:

| Target | Command | Purpose |
|--------|---------|---------|
| `signal` | `make build` | Native desktop client (Sokol + Metal/GL). Runs full sim in-process via `client/local_server.c` for singleplayer. |
| `signal_server` | `make build-server` | Headless game server (Mongoose WebSocket). Authoritative sim for multiplayer. |
| `signal_test` | `make test` | 40+ C test files covering economy, physics, mining, crypto, gossip, station authority, and construction. |

Additional tool targets: `signal_verify` (chain-log validator), `signal_chain_assets` (inventory export), `flight_trace` (offline training traces), `signal_replay` (deterministic counterfactual harness).

## Source Layout

```
client/     Rendering, HUD, input, audio, net, episodes, local_server — Sokol-dependent.
server/     Authoritative sim, chain log, AI, station authority, Mongoose — no rendering deps.
shared/     Wire protocol, types, commodity, economy, cargo receipts, crypto — both sides.
tests/      C test suite (+ Playwright browser smoke in tests/browser-smoke.spec.ts).
tools/      Standalone CLI tools (chain verifier, replay harness, flight traces).
vendor/     Sokol, TweetNaCl, minimp3, pl_mpeg, fastfilter.
web/        Static HTML/JS for the browser build (play.html, touch controls).
```

## Sim Tick Pipeline

The simulation runs at a fixed 120 Hz (`SIM_DT = 1.0/120.0`). Every tick:

1. **Input collection** — player input packets and NPC policy decisions
2. **Physics step** — ship movement, asteroid drift, fragment drag, collision resolution
3. **Mining step** — beam targeting, asteroid fracture, fragment spawning
4. **Production step** — station smelting, fabrication, scaffold manufacturing
5. **AI step** — neural worker job assignment, autopilot, frontier director planning
6. **Signal step** — signal grid rebuild, boundary push, band classification
7. **Event emission** — chain-log events for state mutations (smelt, trade, construction, death)
8. **Snapshot broadcast** — world snapshots to connected clients (multiplayer only)

In **singleplayer**, steps 1-8 all run on the client. In **multiplayer**, steps 2-8 run on the server and step 1 collects input from both local and remote players.

## Entity Lifecycle

### Asteroids
Born from belt noise at world init or respawned in-field. Carry `rock_pub` (deterministic content hash from `belt_seed + index`) and `fragment_pub` (hash binding parent + fracture seed). Fracturing produces child fragments that inherit parent provenance.

### Cargo (three states — see [cargo-architecture.md](docs/cargo-architecture.md))
1. **Fragment** — physical `asteroid_t` in space, towed via tractor, identified by `fragment_pub`. Not in any manifest.
2. **Bulk float** — ephemeral station buffer after smelting. No identity, no chain entry. Legacy path.
3. **Crate** — named `cargo_unit_t` with `pub` + `parent_merkle`. Lives in ship or station manifests. Full receipt-chain provenance.

### Stations
Ring structures with module slots. Three seeded stations (Prospect, Kepler, Helios) plus player-built outposts. Each has an Ed25519 identity, per-station ledger, signed chain log, and authored hail/chatter text synced from an LLM avatar.

### Scaffolds
Manufactured at shipyards, towed through space, snapped onto ring slots. Lifecycle: nascent → loose → towing → snapping → supply phase → commissioning timer → active module.

## Network Sync Model

Multiplayer uses a tick-addressed client-prediction model (not pure server authority):

1. Client sends input with a predicted target tick
2. Server applies input during the matching `world_sim_step()`
3. Client records predictions in a 512-frame replay buffer (`input_replay_frame_t`)
4. Server snapshots arrive with `input_ack` and `server_tick` fields
5. Client replays buffered inputs against each snapshot, reconciling any drift

Dead reckoning extrapolates remote asteroids and NPCs between snapshots with bounded windows. Action IDs are idempotent — retried actions don't duplicate.

The binary wire protocol is documented exhaustively in [shared/protocol.h](shared/protocol.h) with byte layouts for all 20+ message types.

## Key Subsystems

### Signal Model ([shared/signal_model.h](shared/signal_model.h))
Unified signal quality axis (0.0–1.0) gating mining efficiency, ship control, NPC confidence, and visual saturation. Four bands: FRONTIER, FRINGE, OPERATIONAL, CORE. Boundary push forces ships back toward coverage.

### Economy ([shared/economy.h](shared/economy.h), [shared/economy_const.h](shared/economy_const.h))
Per-station sovereign currencies — no global wallet. Credits are `(station_id, player_pubkey) → balance`. Prices are dynamic (supply/demand scaling). Value moves between stations as goods, not currency.

The production backbone is:
fragments -> ingots -> frames / laser modules / tractor modules -> ships,
scaffolds, and repair kits. Smelting is furnace-tagged by output commodity.
Frames use ferrite ingots, laser modules use cuprite ingots plus frames, and
tractor modules use crystal ingots plus frames. Shipyards consume those
finished parts for hull commissions, station-module scaffolds, and the repair
kit sink.

Mining progression has both size and material gates. The starter L1 laser can
fracture M rocks. Each laser upgrade raises the largest fracture size one
step; cuprite requires L2, and crystal requires L3.

### Chain Log ([server/chain_log.h](server/chain_log.h))
Per-station signed append-only event log. Every state mutation (smelt, trade, construction, death) emits a 184-byte header + payload signed by the station's Ed25519 key. `signal_verify` validates the full chain.

### Station Authority ([server/station_authority.h](server/station_authority.h))
Deterministic keypair derivation from operator secret + world seed (seeded stations) or founder pubkey + name + planted tick (outposts). Private keys are never written to disk.

### Frontier Director ([server/sim_ai.c](server/sim_ai.c))
Virtual logistics system that auto-plans outposts, manages scaffold work budgets, and scales with virtual pilot count. Drives NPC expansion without consuming player or ship slots.

### Gossip And Holographic Memory ([server/gossip.h](server/gossip.h), [shared/holographic_nn.h](shared/holographic_nn.h))
Stations and ships exchange bounded situated knowledge through dock contact.
Exact contracts remain authoritative in `contract_t`; gossip carries portable
`contract_summary_t` snapshots and decaying `market_memory_t` pressure through
`knowledge_view_t`. Holographic pilot memory can also bundle station/pilot
experience so neural workers learn through the same physical routes that move
cargo.

## Further Reading

| Doc | Covers |
|-----|--------|
| [CLAUDE.md](CLAUDE.md) | AI-oriented build commands, save layout, working style |
| [docs/cargo-architecture.md](docs/cargo-architecture.md) | Three-state cargo model (fragment, bulk, crate) |
| [docs/holographic-gossip-network.md](docs/holographic-gossip-network.md) | Decaying market gossip and neural worker coordination |
| [docs/holographic-gossip-gap-analysis.md](docs/holographic-gossip-gap-analysis.md) | Gap analysis from current docs/code to the gossip-network vision |
| [docs/decentralization.md](docs/decentralization.md) | Federation architecture and trust model |
| [docs/decentralization-synthesis.md](docs/decentralization-synthesis.md) | Bridge between federation and P2P designs |
| [docs/operator-onboarding.md](docs/operator-onboarding.md) | Station operator guide |
| [docs/protocol-telemetry.md](docs/protocol-telemetry.md) | Wire protocol stream reference |
| [docs/replay-harness.md](docs/replay-harness.md) | Deterministic replay tool |
| [docs/c_safety_policy.md](docs/c_safety_policy.md) | C safety rules and banned APIs |
| [docs/anime-integration-plan.md](docs/anime-integration-plan.md) | Episode playback architecture |
| [REMEDIATION_PLAN.md](REMEDIATION_PLAN.md) | Active improvement plan |
