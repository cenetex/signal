# Signal: Sector One — Engineering Design Document

**Version:** 1.2
**Date:** 2026-07-03
**Status:** Shipped / Live (Sector One)
**Live:** [signal.ratimics.com/play](https://signal.ratimics.com/play)

---

## 1. Technology Stack

| Layer | Technology |
|---|---|
| Language | C11 (ISO C11, no extensions beyond compiler builtins) |
| Graphics | Sokol (`sokol_app.h`, `sokol_gfx.h`, `sokol_gl.h`, `sokol_debugtext.h`, `sokol_shape.h`, `sokol_audio.h`, `sokol_log.h`) |
| Backend | Metal (macOS native), OpenGL (Linux/Windows native), WebGL 2 (Emscripten/WASM) |
| Networking | Mongoose (embedded WebSocket server), custom binary wire protocol |
| Crypto | TweetNaCl (Ed25519 sign/verify), SHA-256 (`shared/sha256.h`) |
| Audio Decode | minimp3 (MP3 music), pl_mpeg (MPEG-1 video episodes) |
| Image Decode | stb_image (PNG station avatars) |
| Testing | Custom C test framework (`tests/c/test_harness.h`), Playwright (browser smoke) |
| Static Analysis | cppcheck, clang-tidy, CRAP scoring, banned-API checker |
| Build | CMake, Ninja (optional), Make (wrapper) |
| CI | GitHub Actions (release, Valgrind); Emscripten/Windows/sanitizers in remediation |

---

## 2. Build System

### 2.1 Targets

One CMake project file ([CMakeLists.txt](CMakeLists.txt)), multiple targets:

| Target | Binary | Description |
|---|---|---|
| `signal` | `build/signal` | Native desktop client (Sokol + Metal/GL), singleplayer via in-process `local_server.c` |
| `signal_server` | `build/signal_server` | Headless authoritative relay (Mongoose WebSocket) |
| `signal_test` | `build/signal_test` | 340+ C test cases across 40+ files |
| `signal_verify` | `build/signal_verify` | Standalone chain-log validator |
| `signal_chain_assets` | `build/signal_chain_assets` | Chain-log asset inventory exporter |
| `flight_trace` | `build/flight_trace` | Offline neural training trace generator |
| `signal_replay` | `build/signal_replay` | Deterministic counterfactual replay harness |

### 2.2 Build Commands

```sh
make build              # Native desktop client (Metal/GL)
make build-web          # Emscripten WASM client
make build-server       # Headless relay
make build-test         # Test binary (always rebuilds from source)
make test               # Fast tests, sharded across cores (~3-5s)
make test-soak          # Soak/long-horizon tests (~10-15s)
make test-all           # Fast + soak
make test-serial        # Single-process, no sharding (debug flaky tests)
make test-san           # ASan+UBSan
make test-tsan          # ThreadSanitizer
make smoke              # Playwright browser smoke
make smoke-latency-suite # Full latency proxy + smoke
make replay-repeatability # Native deterministic replay matrix
make replay-cross-build   # Debug/native cross-build replay matrix
make replay-native-wasm   # Native vs WASM replay hash gate
make replay-native-wasm-long # Long-horizon native vs WASM replay probes
make chain-assets CHAIN_ASSETS_LINEAGE=<cargo_pub> # Human cargo lineage tree
```

Build type defaults to `RelWithDebInfo`; `BUILD_TYPE` overrideable. Test build uses `-O2 -g` even in Debug mode (cuts suite from ~180s to ~56s).

---

## 3. Source Layout

```
signal/
├── client/                  # Rendering, HUD, input, audio, networking, episodes
│   ├── main.c               # Entry point, main loop (Sokol app)
│   ├── render.c/h           # Procedural drawing primitives (circles, lines, rects, textures)
│   ├── world_draw.c/h       # World rendering (asteroids, stations, NPCs, beams, signal)
│   ├── hud.c                # HUD layout, debug overlay, onboarding
│   ├── station_ui.c         # Docked station UI (trade, shipyard, contracts, hail)
│   ├── input.c/h            # Keyboard/mouse/touch input → input_intent_t
│   ├── net.c/h              # WebSocket client, binary protocol encode/decode
│   ├── net_sync.c/h         # Client prediction, replay buffer, dead reckoning
│   ├── audio.c/h            # Procedural synth, music MP3 decode, SFX mixing
│   ├── music.c/h            # MP3 track loader and playback
│   ├── avatar.c/h           # Station PNG avatar textures
│   ├── episode.c/h          # MPEG-1 episode cutscene playback
│   ├── identity.c/h         # Player Ed25519 keypair generation/persistence
│   ├── local_server.c/h     # Embedded server for singleplayer
│   ├── npc.c/h              # NPC draw helpers
│   ├── onboarding.c/h       # Onboarding overlay UI
│   ├── contract_objective.c/h # Contract objective HUD markers
│   ├── mining_client.c/h    # Client-side mining prediction
│   ├── neural_singleplayer.c/h # In-process neural brain for offline play
│   ├── palette.h            # Color palettes
│   ├── station_palette.h    # Station ring/module-specific colors
│   ├── station_voice.h      # Station personality voice definitions
│   └── camera_model.h       # Camera transform, zoom, pan
│
├── server/                  # Authoritative sim — no rendering dependencies
│   ├── main.c               # Entry point, Mongoose HTTP/WS server, API routes
│   ├── game_sim.c/h         # World simulation (7,245 lines — largest file)
│   ├── sim_ai.c/h           # NPC state machines, frontier director (3,120 lines)
│   ├── sim_asteroid.c/h     # Asteroid spawn, fracture, fragment lifecycle
│   ├── sim_autopilot.c/h    # Player autopilot (mine→tow→dock→sell loop)
│   ├── sim_catalog.c/h      # Station identity catalog (MOTD, chatter, hail)
│   ├── sim_construction.c/h # Module placement, activation, outpost founding
│   ├── sim_flight.c/h       # Ship movement, thrust, drag, collision
│   ├── sim_mining.c/h       # Mining beam targeting, damage, fracture
│   ├── sim_nav.c/h          # A* pathfinding, nav graph, station entry/exit
│   ├── sim_physics.c/h      # Fragment drag, asteroid drift, spatial hash
│   ├── sim_production.c/h   # Station smelting, fabrication, scaffold manufacture
│   ├── sim_save.c           # World/player save serialization + migration
│   ├── sim_ship.c/h         # Ship state lifecycle (dock/launch/repair/upgrade)
│   ├── chain_log.c/h        # Per-station signed append-only event log
│   ├── chain_log_verify.c   # Offline chain verification walker
│   ├── cargo_receipt_issue.c/h # CHAIN_EVT_TRANSFER emission and receipt chains
│   ├── gossip.c/h           # Contract gossip protocol (dock handshake)
│   ├── handoff_flow.c/h     # Cross-zone ship-state handoff ticket protocol
│   ├── highscore.c/h        # Top-N leaderboard persistence
│   ├── signal_brain.c/h     # Neural flight controller, holographic VSA memory
│   ├── signal_contract_brain.c/h # Neural contract scoring and policy
│   ├── station_authority.c/h # Deterministic station keypair derivation
│   └── mongoose.c/h         # Embedded HTTP/WebSocket server (vendor-adapted)
│
├── shared/                  # Wire protocol, types, economy — both client + server
│   ├── types.h              # All world structs: asteroid_t, ship_t, station_t,
│   │                        #   cargo_unit_t, manifest_t, npc_ship_t, contract_t,
│   │                        #   server_player_t, input_intent_t, etc. (~1,350 lines)
│   ├── protocol.h           # Binary wire protocol: 30+ message types, record sizes,
│   │                        #   compile-time overlap asserts
│   ├── economy.c/h          # Price scaling, refill, demand, station buy/sell
│   ├── economy_const.h      # Tuning constants: smelt rates, repair costs,
│   │                        #   upgrade steps, prefix-class price multipliers
│   ├── commodity.c/h        # Commodity volume/density, station inventory accessors
│   ├── signal_model.h       # Signal quality bands, mining/control/NPC modifiers
│   ├── module_schema.h      # Module definitions: kind, I/O, cost, rings, prerequisites
│   ├── module_schema.c      # Schema table implementation
│   ├── station_geom.h       # Station collision/render geometry
│   ├── station_util.c/h     # Station query helpers: distance, module lookup, slots
│   ├── station_policy.h     # Station trade/construction/finance policy cards
│   ├── manifest.c/h         # Crate API: push/remove/find, hash_ingot/hash_product
│   ├── cargo_receipt.c/h    # Cargo receipt chain helpers
│   ├── cargo_lineage.h      # Lineage prefix-class resolution
│   ├── ship.c/h             # Ship state helpers, manifest bootstrap
│   ├── asteroid.c/h         # Asteroid identity: rock_pub, fragment_pub derivation
│   ├── belt.c/h             # Deterministic belt noise generation (3D value noise)
│   ├── mining.h             # Mining grade, crystal stage, tier definitions
│   ├── laser.c/h            # Mining beam geometry (ray-circle intersection)
│   ├── tractor.c/h          # Tractor beam physics (spring acceleration toward ship)
│   ├── contract_fit.h       # Contract cargo-fit evaluation
│   ├── settlement_engine.c/h # Cross-station settlement state + Merkle root
│   ├── handoff_ticket.c/h   # Portable zone-handoff ticket types + wire format
│   ├── trade_paging.c/h     # Docked-trade pagination
│   ├── chunk.h              # Belt chunk coordinate math
│   ├── compact.h            # Compact integer encoding
│   ├── math_util.h          # vec2, clamp, lerp, angle normalization
│   ├── fixpoint.c/h         # q32.32 fixed-point arithmetic (for #588 P2P determinism)
│   ├── safe_types.h         # Bounds-checked array wrappers
│   ├── rng.c/h              # SplitMix64 PRNG
│   ├── sha256.h             # SHA-256 hash (public domain implementation)
│   ├── signal_crypto.h      # Ed25519 surface over TweetNaCl
│   ├── base58.h             # Base58 encode/decode
│   ├── base64.h             # Base64 encode/decode
│   ├── pubkey_proof.c/h     # Pubkey proof-of-possession verification
│   └── holographic_nn.c/h   # 1024-dim hyperdimensional vector associative memory
│
├── tests/                   # Test suite
│   ├── c/                   # 40+ C test files (test_*.c)
│   │   ├── test_main.c      # Test runner, sharding, fixtures
│   │   ├── test_harness.c/h # Test assertion macros, world helpers
│   │   ├── test_world_sim.c # Broad integration tests (3,627 lines)
│   │   ├── test_construction.c # Construction cycle tests (2,855 lines)
│   │   ├── test_economy.c   # Price scaling, station buy/sell
│   │   ├── test_commodity.c # Commodity volume, density, accessors
│   │   ├── test_ship.c      # Ship state lifecycle
│   │   ├── test_asteroid.c  # Spawn, fracture, fragment identity
│   │   ├── test_mining.c    # Beam targeting, damage, fracture
│   │   ├── test_crypto.c    # Ed25519 sign/verify
│   │   ├── test_chain.c     # Chain log emit/verify
│   │   ├── test_chain_log.c # Chain log walker
│   │   ├── test_gossip.c    # Contract gossip protocol
│   │   ├── test_settlement_engine.c # Settlement state + Merkle roots
│   │   ├── test_manifest.c  # Crate push/remove/hash lifecycle
│   │   ├── test_station_authority.c # Keypair derivation
│   │   ├── test_signal_verify.c # CLI verifier integration
│   │   ├── test_navigation.c  # A* pathfinding
│   │   ├── test_autopilot.c   # Autopilot scenarios (soak)
│   │   ├── test_identity.c    # Player identity lifecycle
│   │   ├── test_save.c        # Save serialization + migration
│   │   └── ... (23 more)
│   └── browser-smoke.spec.ts # Playwright browser smoke + latency assertions
│
├── tools/                   # Standalone CLI tools
│   ├── signal_verify.c      # Chain-log validator (CLI)
│   ├── signal_chain_assets.c # Asset inventory exporter (JSON/CSV)
│   ├── signal_replay.c      # Counterfactual replay harness
│   ├── flight_trace.c       # Neural training trace generator
│   ├── gen_chain_fixture.c  # Deterministic chain-log fixture generator
│   ├── holo_coverage.c      # Holographic memory coverage scanner
│   ├── belt_noise.py        # Belt structure analysis tool
│   ├── dev_http.py          # Dev HTTP server
│   └── packnft/             # Solana NFT packing tool (separate CMake project)
│
├── vendor/                  # Third-party libraries (single-header where possible)
│   ├── sokol/               # Sokol graphics/audio/input (MIT)
│   ├── tweetnacl/           # TweetNaCl (public domain)
│   ├── minimp3.h            # MP3 decoder (CC0)
│   ├── pl_mpeg.h            # MPEG-1 video decoder (MIT)
│   ├── stb_image.h          # PNG/JPEG image loader (MIT/Unlicense)
│   ├── fastfilter/          # Binary fuse filter (Apache 2.0)
│   ├── cenetex/             # Merkle tree helpers
│   └── mongoose.c/h         # HTTP/WebSocket server (GPLv2, adapted)
│
├── web/                     # Static web assets
│   ├── index.html           # Landing page
│   ├── play.html            # Canonical browser game wrapper
│   ├── signal-touch-controls.js # On-screen touch controls
│   └── ...
│
├── scripts/                 # Build/CI/dev scripts
│   ├── sync-assets.sh       # CDN asset sync
│   ├── ws-latency-proxy.mjs # WebSocket latency injection proxy
│   ├── neural-gap-ab.py     # Neural vs heuristic A/B gap harness
│   ├── protocol-check.py    # Wire protocol validator
│   ├── check_banned_apis.py # C safety policy enforcer
│   ├── crap.py              # CRAP score calculator
│   ├── smoke-latency-suite.mjs # Full latency smoke integration
│   └── deploy-arweave.mjs   # Arweave permaweb deploy
│
├── assets/                  # Runtime media (not in git; synced via manifest.txt)
│   ├── music/               # MP3 music tracks
│   ├── avatars/             # Station PNG portraits
│   ├── episodes/            # MPEG-1 episode cutscenes
│   └── motd/                # Station MOTD JSON
│
├── stations/                # Per-station identity catalog (.cat files)
├── chain/                   # Per-station signed chain logs (.log files)
├── saves/                   # Player saves: pubkey/<base58>.sav, legacy/<token>.sav
│
├── CMakeLists.txt           # Single CMake project defining all targets
├── Makefile                 # Convenience wrapper (build/test/ci targets)
├── package.json             # Node deps: Playwright, Arweave SDK
├── playwright.config.ts     # Playwright browser smoke config
├── wrangler.toml            # (unused — infra removed per REMEDIATION_PLAN)
├── .clang-tidy              # Clang-tidy config
├── cppcheck.suppressions    # cppcheck suppression file
├── ARCHITECTURE.md          # Human-facing architecture reference
├── CLAUDE.md                # AI-oriented context
├── CONSTRUCTION_PLAN.md     # Construction loop design
├── REMEDIATION_PLAN.md      # Active improvement plan
└── README.md                # Player-facing overview
```

---

## 4. Metaproduct Architecture

The engineering contract follows the product stack in [docs/metaproduct.md](docs/metaproduct.md):

```
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

The engineering north star is that every large-scale social structure must be
made out of physical actions players actually performed. The sim should not try
to settle every twitch of ship motion. It should settle transformations the
economy can reason about: fracture claims, smelts, crafts, transfers,
deliveries, construction milestones, station hail/work publication,
signal-channel continuity, deaths, gate contributions, route maintenance, and
future RATi vessel-birth events.

### 4.1 Layer Map

| Layer | Engineering surface | Status |
|---|---|---|
| Physics game | `server/sim_*.c`, `shared/types.h`, `client/world_draw.c` | Shipped, authoritative relay |
| Provenance product | `shared/manifest.*`, `shared/asteroid.*`, receipt chains | Shipped core; manifest authority still being tightened |
| Station sovereignty | `station_t.ledger[]`, `server/station_authority.*`, `server/chain_log.*` | Shipped |
| Verification product | `tools/signal_verify.c`, `tools/signal_chain_assets.c`, `tools/signal_replay.c` | Shipped and actively expanding; CLI cargo lineage tree exists |
| Settlement substrate | `shared/settlement_engine.*`, handoff tickets, checkpoint roots | Draft/shipped pieces; canonical event bridge still open |
| Permaweb/P2P persistence | Arweave deploy tooling, future client reads/anchor service/WebRTC mesh | Backlog, gated by determinism and settlement events |
| External-chain adapters | `tools/packnft/`, Solana bridge docs | Adapter path, not core authority |
| Civilization memory | lineage views, route health, contribution ledgers, gate manifests | Backlog; product-defining surface |

### 4.2 Backlog Dependency Graph

The active backlog is ordered by dependency, not by excitement:

1. **#588 determinism acceptance:** full `q32.32` migration or explicit promotion of the strict native↔WASM replay ratchet with broader platform coverage.
2. **#340 / #339 manifest authority:** make trade, delivery, and production move concrete `cargo_unit_t` rows by default and retire finished-goods float authority.
3. **Lineage view:** CLI cargo lineage exists; next expose rock -> fragment -> ingot -> frame -> outpost/gate contribution as a first-class UI query over manifests, receipts, and chain logs.
4. **#587 typed provenance contracts:** contract targets become explicit object/event pubkeys, including fracture/death fulfillment.
5. **#354 / #355 / #356 settlement bridge:** game-sim validation emits canonical settlement events and signal-channel roots.
6. **Player-facing legibility:** cargo lineage and station history become first-class UI surfaces.
7. **Institution tooling:** shared contracts, escrowed cargo, station-endorsed bounties, route health dashboards, and public construction manifests.
8. **#294 unified ship/controller model:** NPC and player cargo semantics converge on the same `ship_t`/`character_t` substrate.
9. **#590 / #591 / #589 permaweb/P2P:** client Arweave reads, peer anchoring, and WebRTC quorum behavior.
10. **#496 RATi vessel identity:** RATi-bearing vessels become substrate-born identities after manifest and settlement semantics are canonical.
11. **#285 streaming entity pool:** cap lifting and `game_sim.c` extraction once economic/provenance invariants are stable.

---

## 5. Simulation Tick Pipeline

The simulation runs at a fixed 120 Hz (`SIM_DT = 1.0 / 120.0`). Every tick:

```
1. Input Collection
   ├── server_player_queue_movement_input() — buffer player inputs
   ├── NPC brain step — heuristic or neural flight decisions
   └── Autopilot step — mining→tow→dock→sell loop

2. Signal Lookup (grid is event-driven, not per-tick)
   ├── rebuild_signal_chain() + signal_grid_t recomputation run only on
   │   topology changes (world init/load, station activation/construction)
   ├── Per-tick consumers read the cached grid via signal_strength_at()
   └── Boundary push forces applied to out-of-signal ships

3. Physics Step
   ├── Ship movement: thrust, drag, turn, boost (signal-scaled)
   ├── Asteroid drift: belt-relative velocity, drag
   ├── Fragment tow physics: spring acceleration toward tractor ship
   ├── Fragment release/throw physics: inherited velocity
   ├── Scaffold tow physics: same spring model
   ├── Cargo pod drift
   ├── Station ring rotation dynamics: spoke torque + drag
   └── Station jostling: soft pairwise repulsion

4. Collision Resolution
   ├── Ship-vs-asteroid: collision_damage_for(impact, threshold_mult)
   ├── Ship-vs-station: crush damage
   ├── Ship-vs-ship ramming: threshold_mult=0.7 for deliberate hits
   ├── Fragment-vs-station hopper beam: smelt initiation
   └── Spatial hash grid: O(1) neighbor lookups, rebuilt inside the 30 Hz
       gravity/collision gate (asteroid–asteroid only; ship–asteroid
       collision does not use it yet — see docs/optimization-report.md)

5. Mining Step
   ├── sim_mining_beam_step() — range/cone/tier validation
   ├── Signal-scaled damage application
   ├── fracture_asteroid() — children spawn, destroyed-rock ledger updated
   ├── Tractor collection of loose fragments
   └── Fracture-claim window expiration/resolution

6. Production Step
   ├── Station smelting: fragment→ingot via furnace (REFINERY_BASE_SMELT_RATE=2.0/s)
   ├── Station fabrication: ingots→frames/lasers/tractors (STATION_PRODUCTION_RATE=1.0/s)
   ├── Shipyard scaffold manufacturing: station inventory feeds nascent scaffolds
   ├── Shipyard repair-kit fab: 1 frame+1 laser+1 tractor → 100 kits / 30 seconds
   └── Module supply delivery: station inventory → awaiting-supply modules

7. AI Step
   ├── NPC miner state machine: exit station, find rock, mine, tow, return
   ├── NPC hauler state machine: pick up goods, route, deliver, return
   ├── NPC tow state machine: scaffold pickup, delivery, placement
   ├── Frontier director: auto-plan outposts, virtual pilot budgets
   ├── Contract generation from station demand/policy
   └── NPC respawn timer: drip-feed replacement of dead NPCs

8. Event & Chain Log
   ├── Event queue emission: per-player SIM_EVENT_* for UI popups
   ├── CHAIN_EVT_* emission for each state mutation
   ├── Cargo receipt issuance for transfers
   └── Settlement engine update

9. Snapshot Broadcast
   ├── WORLD_ASTEROIDS — delta-compressed per-player relevance
   ├── WORLD_NPCS — full snapshot
   ├── WORLD_STATIONS — identity + economic snapshot
   ├── WORLD_PLAYERS — pose + beam + input_ack/server_tick
   ├── WORLD_SCAFFOLDS — active scaffold pool
   └── Payload caching: hash-suppressed re-broadcast (force on action result)
```

### 5.1 Performance Characteristics

A static hot-path analysis of the tick pipeline lives in
[docs/optimization-report.md](docs/optimization-report.md) (2026-07-03). The
short version:

**Already right (do not regress):** the asteroid spatial grid (800-unit
cells), the 30 Hz gate on N-body gravity/collision, the 256² cached signal
grid, tiered netcode rates (120/20/10/4 Hz) with per-player viewport culling
and byte budgets, and client-side per-frame render list caching.

**Known scaling walls:** the dominant costs are not physics math but loop
*shapes* of the form `entities × MAX_STATIONS × 120 Hz`:

1. `sim_step_asteroid_dynamics` scans all 128 station slots twice per active
   asteroid per tick (despawn margin + station vortex).
2. NPC collision (`npc_resolve_station_collisions`) calls
   `station_build_geom` per NPC per colliding station with no broad-phase
   distance check, and NPC/player–asteroid collision scans all 2,048 asteroid
   slots instead of the spatial grid.
3. `signal_strength_at` runs a hidden full-station scan (off-relay dock
   beacons) before every cached-grid lookup, defeating its O(1) contract.
4. The per-tick manifest reconciliation pass linearly scans every station
   manifest for every finished commodity at 120 Hz.
5. Autosave serializes the world with synchronous `fwrite` on the sim
   thread every 30 s (periodic hitch).

The report ranks seven fixes (compact active-station array, cached beacon
list, broad-phase guards, per-tick shared station-geometry cache, spatial-grid
reuse, manifest count cache, async save writer). These are prerequisites for
lifting entity caps under #285 — the walls above steepen quadratically as
station and asteroid counts grow. `world_sim_step` has no per-phase timing
instrumentation yet; adding a `SIM_PROFILE`-gated phase histogram is the
first step before landing any of the fixes.

### 5.2 Singleplayer vs Multiplayer

| Mode | Sim Location | Input Path |
|---|---|---|
| **Singleplayer** | In-process `local_server.c` | Client feeds input directly to sim, same tick |
| **Multiplayer** | Remote `server/main.c` | Client sends input via WebSocket, server applies with `server_tick` |

The simulation code (`world_sim_step()`) is identical in both modes. Singleplayer calls it directly from the client main loop; multiplayer calls it on the server, which broadcasts snapshots back.

---

## 6. Entity Data Model

### 6.1 Entity Pool Architecture

All world entities live in fixed-cap arrays on `world_t` (see `shared/types.h`):

```c
typedef struct {
    station_t           stations[MAX_STATIONS];         // 128
    asteroid_t          asteroids[MAX_ASTEROIDS];        // 2,048
    fracture_claim_state_t fracture_claims[MAX_ASTEROIDS];
    destroyed_rock_s    destroyed_rocks[MAX_DESTROYED_ROCKS]; // sorted, bsearch
    npc_ship_t          npc_ships[MAX_NPC_SHIPS];        // 100
    ship_t              ships[MAX_SHIPS];                // 100 (unified pool)
    character_t         characters[MAX_PLAYERS + MAX_NPC_SHIPS]; // controller pool
    scaffold_t          scaffolds[MAX_SCAFFOLDS];         // 16
    cargo_pod_t         cargo_pods[MAX_CARGO_PODS];       // 64
    server_player_t     players[MAX_PLAYERS];             // 32
    contract_t          contracts[MAX_CONTRACTS];          // 24
    delivery_shipment_t delivery_shipments[MAX_DELIVERY_SHIPMENTS]; // 24
    // ...
} world_t;
```

Capacity constraints are pinned by the v1 wire protocol (entity ids are `uint8` per type, asteroids are `uint16` since #285 Phase 3). Lifting any cap requires a wire protocol revision (tracked as #285).

### 6.2 Asteroid Lifecycle

```
BELT NOISE (deterministic, per-seed)
  → FIRST CONTACT MATERIALIZATION
    → rock_pub = SHA256(belt_seed || chunk_x || chunk_y || slot)
      → asteroid_t in world.asteroids[]
        → FRACTURE
          → destroyed_rocks[] ← rock_pub retired permanently
          → child fragments spawned
            → fragment_pub = SHA256(parent_rock_pub || fracture_seed || child_idx)
              → child asteroids in world.asteroids[]
                → TOW → SMELT → cargo_unit_t with parent_merkle = fragment_pub
                → RELEASE → CHAIN_EVT_FRAGMENT_RELEASE
                → DESTROY → gone (no chain entry; material lost)
                → CLEANUP → auto-despawn after age 30s + distance 4000u
```

The destroyed-rock ledger is identity-keyed (32-byte rock_pub), sorted for O(log n) bsearch. Slice 2+ targets a four-tier model: in-memory Binary Fuse filter → signed chain-log → Merkle Mountain Range/checkpoint root → permaweb artifact or external-chain adapter proof.

### 6.3 Cargo Lifecycle

See [docs/cargo-architecture.md](docs/cargo-architecture.md) for the canonical three-state model.

```
FRAGMENT (asteroid_t, in space)
  → fragment_pub identity, physics
  → towed as ship.towed_fragments[] or npc.towed_fragment

BULK FLOAT (station._inventory_cache[ORE])
  → anonymous ephemeral buffer
  → legacy path; no longer consumed by production

CRATE (cargo_unit_t, in manifest)
  → pub + parent_merkle identity
  → lives in ship.manifest / station.manifest
  → created at smelt (hash_ingot) or craft (hash_product) boundaries
  → quantity = 1 for live production; >1 reserved for future anonymous batches
```

### 6.4 Station Ring + Module Model

```
station_t
├── pos, jostle_vel, radius, dock_radius, signal_range
├── scaffold, planned, scaffold_progress
├── _inventory_cache[COMMODITY_COUNT]  (raw ore floats + manifest cache)
├── ledger[STATION_LEDGER_MAX] — per-player-pubkey credit balances
├── modules[MAX_MODULES_PER_STATION=16]
│   └── station_module_t { type, ring, slot, scaffold, commodity, build_progress }
├── plans[8] — reserved placement slots
├── pending_scaffolds[4] — shipyard queue
├── manifest — cargo_unit_t array (cap 256)
├── contracts[24]
├── signal_channel — broadcast log ring buffer (100 entries)
├── station_pubkey, station_secret (last field, _Static_assert-guarded)
├── chain_last_hash, chain_event_count
├── known_contracts[10] — gossip pool
└── arms[4] { speed, offset, rotation, omega }
```

Stations have 3 outer rings (3/6/9 slots) plus an inner core slot. Total module capacity: 18 outer + 1 core = 19 slots (current cap is 16; core slot reserved for dock/relay). Ring rotation dynamics couple through spoke-graph torque: each producer→hopper pair adds equal-and-opposite angular spring torque, with viscous drag for settling.

---

## 7. Binary Wire Protocol

### 7.1 Protocol Discovery

On connect, the server sends `NET_MSG_PROTOCOL_INFO` (0x41) with:
- Protocol version (uint16)
- Capability bitmask (uint32)
- Stream catalog: per-stream message type, class, header/record sizes, max records, cadence

Capabilities (`shared/protocol.h`):
```
SIGNAL_PROTOCOL_CAP_PROTOCOL_INFO    = 1 << 0
SIGNAL_PROTOCOL_CAP_STATION_DIAG     = 1 << 1
SIGNAL_PROTOCOL_CAP_MANIFEST_STREAMS = 1 << 2
SIGNAL_PROTOCOL_CAP_LATENCY_METRICS  = 1 << 3
SIGNAL_PROTOCOL_CAP_RECEIPT_CHAINS   = 1 << 4
SIGNAL_PROTOCOL_CAP_INSPECT_SNAPSHOT = 1 << 5
SIGNAL_PROTOCOL_CAP_HANDOFF_TICKETS  = 1 << 6
SIGNAL_PROTOCOL_CAP_DELIVERY_SHIPMENTS = 1 << 7
```

### 7.2 Input Packet

```
NET_MSG_INPUT (0x04): 18 bytes
  [type:1][flags:1][action:1][buy_grade:1][seq:u16][target:u16]
  [action_id:u16][input_tick:u32][_pad:2]
```

Movement flags: `THRUST | LEFT | RIGHT | FIRE | BRAKE | TRACTOR | BOOST | REVERSE`.
Actions: one-shot state changes (dock, launch, buy, sell, hail, etc.) — idempotent via `action_id`.
Input tick: client's predicted sim tick for application. Server accepts future-dated movement within `[2, 12]` ticks ahead.

### 7.3 Signed Actions

For persistent state changes (buy, sell, deliver, place outpost, claim contract), the client sends `NET_MSG_SIGNED_ACTION` (0x33):

```
[type:1][nonce:u64][action_type:1][payload_len:u16][payload:N][signature:64]
```

The server validates: pubkey registered, nonce > last_signed_nonce, Ed25519 signature over `(nonce || action_type || payload_len || payload)`. Movement inputs remain unsigned to avoid per-frame signing overhead.

### 7.4 World State Snapshots

| Message | Record Size | Max Records | Compression |
|---|---|---|---|
| `WORLD_ASTEROIDS` (0x10) | 35 bytes | 2,048 | Per-player relevance + delta |
| `WORLD_NPCS` (0x11) | 29 bytes | 100 | Full snapshot |
| `WORLD_PLAYERS` (0x18) | 77 bytes | 32 | Full snapshot |
| `WORLD_STATIONS` (0x12) | 41 bytes | 128 | Hash-suppressed, force on change |
| `WORLD_SCAFFOLDS` (0x24) | 28 bytes | 16 | Full snapshot |
| `STATION_IDENTITY` (0x17) | ~1,400 bytes | 128 | On-join, on-change |
| `STATION_DIAG` (0x40) | 19 bytes | 128 | Per-tick live telemetry |

### 7.5 Client Prediction & Reconciliation

```
Client side:
  input → push to replay_buffer (512 frames, ring)
       → send INPUT packet with target_tick
       → apply prediction locally

Server side:
  receive INPUT → queue in player.movement_queue[]
  world_sim_step() → consume from movement_queue at matching tick
                  → broadcast WORLD_PLAYERS with input_ack + server_tick

Client reconciliation:
  receive WORLD_PLAYERS → server_state = authoritative
  replay_buffer.walk(inputs after input_ack)
    → re-apply against server_state
    → dead_reckon remote entities forward to local_tick
```

Dead reckoning for remote asteroids and NPCs uses bounded extrapolation windows. Correction modes include snap, lerp, and velocity-blend; the client chooses based on position error magnitude.

---

## 8. Signal Grid Implementation

### 8.1 Cached Grid

```c
#define SIGNAL_GRID_DIM  256
#define SIGNAL_CELL_SIZE 200.0f

typedef struct {
    float *strength;           // SIGNAL_GRID_DIM² floats
    float offset_x, offset_y;  // world offset to center grid
    bool  valid;               // false = needs rebuild
} signal_grid_t;
```

The grid covers a 51,200×51,200 unit area centered on the origin (expandable). Each cell stores the max signal strength from any connected station covering that cell. Lookups are O(1): clamp world position to grid bounds, index into flat array.

### 8.2 Signal Chain Rebuild

```
rebuild_signal_chain(world_t *w):
  1. Reset all stations to signal_connected = false
  2. BFS from root stations (seeded stations 0-3):
     For each connected station:
       - Mark signal_connected = true
       - For each neighboring station within (combined signal_range - margin):
         - If neighbor not visited, enqueue (BFS)
  3. Rebuild signal_cache from all connected stations:
     For each grid cell:
       strength = max(station's contribution at cell center)
     where each station's contribution decays with distance:
       signal_at_distance = max(0.0, 1.0 - distance/signal_range)
  4. Mark signal_cache.valid = true
```

Rebuild is event-driven — it runs at world init/load and on station topology changes (activation, construction, signal-chain edits), not per tick. Connected stations form a spanning tree from roots. Isolated stations (no path to a root) don't contribute to the grid. Note: `signal_strength_at` currently also scans stations for off-relay dock beacons on every call — see [docs/optimization-report.md](docs/optimization-report.md) Finding 3.

---

## 9. Economy Model

### 9.1 Per-Station Ledgers

```c
station_t.ledger[STATION_LEDGER_MAX] = {
    { player_pubkey[32], balance: float, lifetime_supply: float },
    // 64-entry table; full ledgers reclaim only empty/inert rows
}
```

Credits are `(station_id, player_pubkey) → balance`. Stations are sovereign currency issuers — `station_credit_pool()` = `-Σ(ledger balances)` and can go arbitrarily negative.

### 9.2 Dynamic Pricing

```c
station_buy_price(station, commodity):
    fill = station_inventory_amount(station, commodity) / hopper_capacity
    return base_price * (1.0 - 0.5 * fill)  // 1.0x empty → 0.5x full

station_sell_price(station, commodity):
    fill = station_inventory_amount(station, commodity) / max_product_stock
    return base_price * (2.0 - 1.0 * fill)  // 2.0x empty → 1.0x full stock
```

Per-unit pricing for named cargo applies `prefix_class_price_multiplier(unit.prefix_class)`.

### 9.3 Supply/Demand

- Raw ore smelt gives player 65% of the fragment's value (35% station cut). The station gets the ingot.
- Finished goods are sold at `base_price * station_sell_price()`.
- Hauler reserve (6.0 ingots) prevents NPCs from draining station stock to zero — leaves margin for player purchases.

---

## 10. Decentralization Stack

### 10.1 Identity Derivation

**Seeded stations (Prospect, Kepler, Helios):**
```
seed = SHA256("signal-station-v1" || operator_secret || world_seed_u32 || station_index_u32)
keypair = Ed25519_keypair_from_seed(seed)
```

**Outposts:**
```
seed = SHA256("signal-outpost-v1" || operator_secret || founder_pub[32] || station_name[16] || planted_tick_u64)
keypair = Ed25519_keypair_from_seed(seed)
```

Derivation is deterministic: any server with the operator secret can rederive all station keypairs. Private keys are never serialized — `station_secret` is the last field and guarded by `_Static_assert(offsetof(station_t, station_secret) == sizeof(station_t) - 64)`.

### 10.2 Chain Log Format

```
Per-station file: chain/<base58(station_pubkey)>.log

Each entry:
  [header: 184 bytes]
    epoch:     u32   (sim tick)
    event_id:  u32   (monotonic per segment)
    type:      u32   (chain_event_type_t)
    prev_hash: u8[32] (SHA-256 of previous header, 0 = genesis)
    authority: u8[32] (station_pubkey)
    payload_hash: u8[32] (SHA-256 of payload)
    signature: u8[64] (Ed25519 over header bytes 0..119)
  [payload_len: u16]
  [payload: payload_len bytes]
```

Verification: walk entry by entry, verifying signature, prev_hash linkage, and payload_hash. New segments (`event_id == 1, prev_hash == 0`) are accepted. Missing/broken linkage = FAILED.

### 10.3 Cargo Receipt Chains (Layer D)

When a cargo unit crosses a zone boundary, the source station issues a signed transfer receipt:

```c
cargo_receipt_t {
    cargo_pub[32],           // what moved
    from_pubkey[32],         // source holder
    to_pubkey[32],           // destination holder
    station_pubkey[32],      // authority witnessing the transfer
    event_id, epoch,         // link to source chain-log event
    prev_receipt_hash[32],   // chain on the receipt side
    signature[64]            // station's signature
}
```

The destination verifies: signature valid, authority known, head receipt names the player's pubkey, chain links are intact. Handoff tickets extend this to full ship-state transfer with manifest + receipt root binding.

---

## 11. Neural Worker Architecture

### 11.1 Assignment Outcomes And Legacy Role Labels

The code still has legacy role labels such as `NPC_ROLE_MINER`,
`NPC_ROLE_HAULER`, and `NPC_ROLE_TOW` because they are useful for compatibility,
presentation, and capability hints. The target architecture is broader:
workers produce `npc_job_offer_t` candidates from exact contracts, structured
market memory, route evidence, HNN resonance, and local capability, then execute
the best safe offer.

Current job offer families:

```
NPC_JOB_MINE:
  find ore/fracture pressure -> navigate to belt -> fracture/tow fragment
  -> return to smelter

NPC_JOB_HAUL:
  score exact contracts plus demand/supply/route memory -> pick source
  -> load cargo -> deliver to destination -> present receipt chain

NPC_JOB_DELIVER_PROOF:
  bind NPC-owned shipment cargo -> deliver to destination -> return proof
  -> clear origin debt and emit route/receipt memory

NPC_JOB_TOW:
  read scaffold/build pressure -> acquire scaffold -> tow to plan
  -> place or advance construction

NPC_JOB_SCOUT:
  respond to fracture, route danger, or stuck-worker pressure

NPC_JOB_REPAIR:
  respond to repair-kit supply and damaged-worker pressure
```

The invariant is that memory only chooses what to try. Completion still resolves
through authoritative station state: contracts, ledgers, manifests, receipt
chains, and chain logs.

### 11.2 Frontier Director

The frontier director (`step_frontier_director()`) is a virtual logistics system:
- Tracks a configurable pool of virtual pilots (up to 1,000,000).
- Ranks expansion candidates by signal fringe proximity, resource access, and distance to existing stations.
- Places planned outpost ghosts and orders virtual scaffold kits.
- Virtual scaffolds "manufacture" instantly (skip physics) and are "delivered" by virtual tow drones.
- Drives NPC expansion without consuming physical ship slots or network bandwidth.

### 11.3 Neural Flight

- **Checkpoint scorer:** `signal_brain_drive()` loads a `signal-flight-live-v2` trained model, runs inference per tick, and outputs thrust/turn decisions.
- **Holographic VSA:** `signal_brain_drive_npc()` uses 1024-dim hyperdimensional vectors. Each tick's (state vector) is associated with (action vector) via circular convolution binding. Recall = unbind + threshold + consensus sum.
- **Training traces:** `flight_trace` generates CSV traces with (pos, vel, angle, target, signal_quality → thrust, turn) for supervised learning.

---

## 12. Render Pipeline

### 12.1 World Rendering

```
world_draw() per frame:
  1. Clear (background starfield: 120 stars, fixed camera depth)
  2. Draw asteroid belt (chunk-frustum culling, LOD by distance)
     - Shape: N-gon outline + optional fill, tier-based vertex count
     - Color: commodity-tinted, hp-ratio dimmed
  3. Draw stations (back-to-front sort by camera depth)
     - Ring outlines (tinted by signal saturation)
     - Module outlines (type-specific shapes)
     - Furnace glow (last_smelt_commodity → color)
     - Dock approach corridor (dashed lines)
     - Hail/scan range circle (when active)
  4. Draw ships (players + NPCs)
     - Hull shape by class, callsign label, thruster flame
     - Tractor beam (when active)
     - Mining beam + hit indicator
  5. Draw fragments and scaffolds
  6. Draw signal boundary overlay (when in fringe/frontier)
     - Colored ring at signal band transitions
     - Directional arrow toward nearest coverage
  7. Draw HUD overlay
     - Ship stats (hull, speed, signal band)
     - Station proximity indicator
     - Towed fragment/scaffold indicators
     - Contract objective markers
     - Scan tags (short-lived nearby object labels)
```

### 12.2 Signal-Driven Saturation

The renderer accepts a per-pixel saturation query:
```c
void render_set_saturation_sampler(render_saturation_sample_fn fn, void *user);
```

Each draw call multiplies its color saturation by the sample at its world position. The sampler reads from `signal_cache`. Critical cues (signal borders, beams, navigation marks) use `signal_visual_cue_saturation()` which clamps minimum saturation to 0.72. The player's own ship uses `signal_visual_player_saturation()` with minimum 0.92.

### 12.3 Docked Station UI

```
Tab-cycled panels:
  SHIP      — hull HP, repair (R), upgrade mining (M), upgrade hold (C),
              upgrade tractor (T), scaffold kit, autopilot toggle
  TRADE     — station buy rows (1-5), player sell rows (1-5), page (F),
              sell all (S), deliver (D), commodity short labels
  CONTRACTS — up to 3 active contracts, track (1-3), deliver (S),
              contract objective markers in world
  YARD      — scaffold kit order (1-9), locked-by-tech-tree indicators,
              module type names, order fee display
  HAIL      — station MOTD, local ledger balance, station work,
              chatter lines, RATi-grade delivery hail
```

---

## 13. Persistence Format

### 13.1 World Save (`world.sav`)

Binary format, little-endian. Versioned (currently v62; minimum accepted v49).
Sections:

```
[HEADER] version:u32, world_seq:u32, belt_seed:u32, time:f32, tick:u32
[STATIONS] count, then per-station: pos, modules, inventory, ledger, manifest, etc.
[ASTEROIDS] count, then per-asteroid: pos, vel, hp, ore, rock_pub, fragment_pub, etc.
[NPCS] count, then per-npc_ship_t: ship state, hull, session_token, known_contracts
[SCAFFOLDS] count
[CARGO_PODS] count
[CONTRACTS] count
[DELIVERY_SHIPMENTS] count
[DESTROYED_ROCKS] count, then sorted rock_pub[32] + destroyed_at_ms tuples
[SIGNAL_CHANNEL] messages ring buffer
[PUBKEY_REGISTRY] count, then (pubkey, session_token) pairs
```

Migration paths: `world_load()` applies version-gated fixups (e.g., v50→v51
furnace tagging, v52→v53 receipt chain seeding). Recent layout changes:
v54 adds `world_seq`; v55 persists staged-crystal fracture-child sidecars;
v56/v57 add contract provenance and forbidden-origin gates; v58 expands station
session slots to `MAX_STATIONS`; v59 persists delivery-shipment debt/proof
state; v60 persists active fracture-child throw ownership; v61 persists
contract `target_pub`; v62 expands each station's player ledger from 16 to
`STATION_LEDGER_MAX` entries while older saves still read the 16-entry table.

### 13.2 Player Save

```
[HEADER] version:u32
[SHIP] ship_t (pos, vel, angle, hull, cargo, manifest, upgrades, stats, receipts)
[IDENTITY] pubkey[32], last_signed_nonce:u64
[UNLOCKED_MODULES] bitmask:u32
```

Keyed by `saves/pubkey/<base58(pubkey)>.sav` (canonical) or `saves/legacy/<token_hex>.sav` (auto-migrated at startup). Legacy saves can be claimed via signed `"claim-legacy-save-v1" || <token_hex>` challenge; verified attempts append `legacy_claims.log` so operators can audit first-claim-wins imports.

---

## 14. Testing Architecture

### 14.1 C Test Framework

Custom lightweight harness (`tests/c/test_harness.h`):
```c
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { if (!(cond)) { test_fail(__FILE__, __LINE__, #cond); return; } } while(0)
#define ASSERT_FLOAT_EQ(a, b, eps) ...
#define ASSERT_VEC2_EQ(a, b, eps) ...

// Sharding support:
int main(int argc, char **argv) {
    test_runner_init(argc, argv);  // parse --shard=N/T, --quiet, --no-soak, --soak-only
    // ... register tests ...
    test_runner_run();
}
```

Tests declare `RUN_FAST` or `RUN_SOAK` macros for filtering. Sharding splits the test list across N parallel processes. Aggregate reporting merges per-shard logs.

### 14.2 Key Test Files

| File | Lines | Focus |
|---|---|---|
| `test_world_sim.c` | 3,627 | Broad integration: full world lifecycle, multi-player, contracts, death, highscore |
| `test_construction.c` | 2,855 | Construction cycle: plan, order, tow, place, supply, activate, outpost founding |
| `test_economy.c` | ~800 | Dynamic pricing, prefix-class multipliers, station buy/sell, credit pool |
| `test_chain.c` | ~700 | Chain log emit/verify, signature validation, segments, migration |
| `test_settlement_engine.c` | ~600 | Settlement state, Merkle roots, event application |
| `test_manifest.c` | ~500 | Crate lifecycle, push/remove/find, hash_ingot/hash_product, receipt chains |
| `test_navigation.c` | ~400 | A* pathfinding, signal-connected routing, obstacle avoidance |
| `test_autopilot.c` | ~400 | Autopilot mining→dock→sell cycle (soak) |

### 14.3 Browser Smoke

Playwright test (`tests/browser-smoke.spec.ts`):
- Build WASM client, serve via local HTTP.
- Launch Chromium, navigate to `play.html`.
- Wait for canvas render.
- Simulate keyboard input (WASD flight, M mining, E dock, H hail).
- Read wasm-side telemetry for correctness.
- Latency smoke: inject delay via `ws-latency-proxy.mjs`, verify ping/ack/gap metrics.

### 14.4 CI Pipeline (Current)

| Workflow | Trigger | What |
|---|---|---|
| `release.yml` | Push to main, tag `v*` | Native build + test + Arweave deploy |
| `valgrind.yml` | Manual / schedule | Valgrind memcheck on full test suite |

Remediation targets: keep native/WASM replay gates on the blocking path, add Linux x86 and Windows coverage for determinism-critical targets, add ASan+UBSan and clang-tidy to CI, and add fuzzing harnesses for protocol decode, save load, and chain-log parsing.

---

## 15. Key Constants & Tuning

| Constant | Value | Notes |
|---|---|---|
| `SIM_DT` | 1/120 s | Fixed-step simulation tick |
| `SIM_MAX_EVENTS` | 64 | Per-tick event buffer |
| `MINING_RANGE` | 170 u | Beam range from ship muzzle |
| `SHIP_BRAKE` | 180 u/s² | Braking acceleration |
| `FRAGMENT_TRACTOR_ACCEL` | 380 u/s² | Tractor pull strength |
| `FRAGMENT_MAX_SPEED` | 210 u/s | Tow speed cap |
| `SHIP_COLLISION_DAMAGE_THRESHOLD` | 115 u/s | Impact velocity before damage |
| `SHIP_COLLISION_DAMAGE_SCALE` | 0.12 | Damage per excess impact unit |
| `REFINERY_BASE_SMELT_RATE` | 2.0 /s | Fragments → ingots |
| `STATION_PRODUCTION_RATE` | 1.0 /s | Ingots → finished goods |
| `REFINERY_INGOTS_PER_FRAGMENT` | 10.0 | One fragment yields 10 ingots |
| `STATION_REPAIR_COST_PER_HULL` | 5.0 cr/HP | Repair pricing |
| `REPAIR_KIT_FAB_PERIOD` | 30 s | Shipyard kit batch time |
| `REPAIR_KIT_PER_BATCH` | 100 kits | Kits per batch |
| `SCAFFOLD_MATERIAL_NEEDED` | 60.0 frames | Outpost construction |
| `OUTPOST_MIN_DISTANCE` | 1500 u | Inter-station spacing |
| `OUTPOST_SIGNAL_RANGE` | 6000 u | Outpost coverage |
| `OUTPOST_MAX_SIGNAL` | 0.80 | Can't place in core coverage |
| `WORLD_RADIUS` | 50000 u | Soft boundary |
| `BELT_SCALE` | 15000 | Noise period divisor |
| `FIELD_ASTEROID_TARGET` | 220 | Active belt rocks |
| `A* NAV_MAX_NODES` | 96 | Pathfinding graph cap |
| `SPATIAL_CELL_SIZE` | 800 u | Spatial hash granularity |
| `SIGNAL_GRID_DIM` | 256 | Signal cache resolution |
| `SIGNAL_CELL_SIZE` | 200 u | Signal cache granularity |

---

## 16. C Safety Policy

From [docs/c_safety_policy.md](docs/c_safety_policy.md):

| Banned | Replacement |
|---|---|
| `sprintf`, `vsprintf` | `snprintf`, `vsnprintf` |
| `strcpy`, `strcat` | `strncpy` + explicit null-terminate, or `snprintf` |
| `gets` | `fgets` |
| `scanf` family on untrusted input | Custom parsers with bounds checks |
| `alloca` / VLAs of unbounded size | Fixed-size stack arrays or heap |
| `realloc` of untrusted size | Bounded alloc + copy |

Enforced by `scripts/check_banned_apis.py` in `make banned-apis`.

---

## 17. Docs Cross-Reference

| Document | Path | Role |
|---|---|---|
| PRD | [PRD.md](PRD.md) | Product requirements, gameplay design |
| ENG | [ENG.md](ENG.md) | This document — engineering design |
| Architecture | [ARCHITECTURE.md](ARCHITECTURE.md) | Human-facing architecture overview |
| AI Context | [CLAUDE.md](CLAUDE.md) | AI-oriented build commands, codebase navigation |
| Construction Plan | [CONSTRUCTION_PLAN.md](CONSTRUCTION_PLAN.md) | Construction loop design + status |
| Remediation Plan | [REMEDIATION_PLAN.md](REMEDIATION_PLAN.md) | Active improvement priorities |
| Metaproduct | [docs/metaproduct.md](docs/metaproduct.md) | Product stack, settlement framing, and groomed backlog |
| Cargo Architecture | [docs/cargo-architecture.md](docs/cargo-architecture.md) | Three-state cargo model (fragment/float/crate) |
| Decentralization | [docs/decentralization.md](docs/decentralization.md) | Federation architecture, identity stack, chain log |
| Decentralization Synthesis | [docs/decentralization-synthesis.md](docs/decentralization-synthesis.md) | Bridge between federation and P2P |
| Furnace Design | [docs/furnace-design.md](docs/furnace-design.md) | Furnace/smelting subsystem |
| Operator Onboarding | [docs/operator-onboarding.md](docs/operator-onboarding.md) | Station operator guide |
| Anime Integration | [docs/anime-integration-plan.md](docs/anime-integration-plan.md) | Episode playback architecture |
| Protocol Telemetry | [docs/protocol-telemetry.md](docs/protocol-telemetry.md) | Wire protocol stream reference |
| Replay Harness | [docs/replay-harness.md](docs/replay-harness.md) | Deterministic replay tool docs |
| Optimization Report | [docs/optimization-report.md](docs/optimization-report.md) | Sim/render hot-path analysis and ranked fix plan |
| C Safety Policy | [docs/c_safety_policy.md](docs/c_safety_policy.md) | C safety rules and banned APIs |
| Settlement Events | [docs/settlement-event-model.md](docs/settlement-event-model.md) | Settlement engine event model |
| Solana Bridge | [docs/signal-solana-bridge.md](docs/signal-solana-bridge.md) | On-chain bridge design |
| Station Tokens | [docs/station-tokens.md](docs/station-tokens.md) | Station token economics |
| Txn Design | [docs/txn-design.md](docs/txn-design.md) | Transaction/action design |
| Yield Split | [docs/yield-split-design.md](docs/yield-split-design.md) | Mining yield split design |
| PackNFT | [docs/packnft-architecture.md](docs/packnft-architecture.md) | NFT packing tool architecture |
