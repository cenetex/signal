# Signal — Engineering Deep-Dive Report

**Date:** 2026-06-09 · **Updated:** 2026-06-13 · **Branch:** main @ `31197b1` for original audit; remediation notes through the 2026-06-13 replay/build-flag, planned-outpost, station-jostle, player-ram, and NPC-ram slices · **Scope:** full repo audit (sim, economy, client, protocol, persistence, infra, decentralization stack)

---

## 1. Executive Summary

Signal is a multiplayer space-station mining game written in C11, live at signal.ratimics.com/play. The repo is ~10 weeks old (first commit 2026-03-29) with a single primary author and a very high commit cadence (peaks of 30–50 commits/day). In that time it has shipped: a deterministic-leaning 120 Hz authoritative simulation, a client-predicted WebSocket multiplayer protocol with an in-process singleplayer mode, a sovereign per-station economy with cryptographic cargo provenance (Ed25519-signed chain logs and receipt chains), a custom ~340-case C test suite with sharding, and an unusual deploy pipeline (WASM client on Arweave, fronted by a Cloudflare Worker, game server in Docker).

**Overall assessment:** the architecture is unusually coherent for its age — the client/server/shared split is clean, invariants are documented and enforced with `_Static_assert`s and regression tests, and the "designed vs shipped" boundary is honestly tracked in docs. The original 2026-06-09 audit found four main structural risks: (a) three monolith files (`game_sim.c` 7.3k, `main.c` server 4.9k, `world_draw.c` 4.0k lines); (b) floating-point simulation state blocking P2P determinism (#588); (c) persistence/auth gaps; and (d) CI that could deploy without tests. As of the 2026-06-13 update, the persistence/auth and CI gaps called out here have been substantially remediated; #588 remains the largest architectural migration, now with an incremental deterministic-math ratchet instead of an untouched library.

### 1.1 Remediation Update — 2026-06-13

The original risk register drove a concentrated remediation pass. Landed fixes:

- `2118f9f` / `b2b0da1`: per-player saves now use atomic write patterns and reject corrupt PLY7 saves before mutating live player state.
- `670c645` / `423cbe1`: Arweave deploys are gated by test jobs, sanitizer coverage, browser smoke, banned-API checks, and a deterministic-libm ratchet.
- `4427e39`: session tokens use cryptographic randomness rather than world-seeded derivation.
- `38d3c3d`: station-ledger capacity failures are protected rather than silently cliffing.
- `54569d6`: save-format docs were refreshed to the current save version.
- `2338c80`: station receipt chains verify on insert.
- `5248298`: destroyed-rock tombstone capacity was raised.
- `2963fce`: legacy-save claims are audited.
- `417c520`, `f7c1695`, `423cbe1`, `617f8b0`, `9c9a236`, plus the 2026-06-13 replay slices: deterministic math has been integrated into shared vector helpers plus tractor, laser, ship physics, asteroid physics, the shared flight controller, and the replay harness, with `scripts/check_deterministic_libm.py` preventing raw-transcendental regressions across all `server/`, `shared/`, and deterministic replay code. The replay matrix now includes free flight, provenance buy/sell, pod tow/sell, mining→fracture, asteroid collision→death, planned-outpost scaffold materialization, station-jostle, player-player ram, and NPC-NPC ram coverage.

Net effect: several items that were urgent on 2026-06-09 are now closed or materially reduced. Remaining high-leverage work is narrower: keep expanding cross-build replay scenarios over physics/economy surfaces, then address float currency/ledger balances and the larger float-state migration.

---

## 2. Project Shape and History

| Metric | Value |
|---|---|
| First commit | 2026-03-29 ("Initial playable prototype") |
| Total first-party C (excl. vendor/node_modules) | ~70–80k lines |
| Largest first-party files | `server/game_sim.c` 7,251 · `server/main.c` 4,905 · `client/world_draw.c` 3,979 · `client/hud.c` 3,396 · `server/sim_ai.c` 3,120 |
| Vendored | mongoose (29k), stb_image, pl_mpeg, minimp3, tweetnacl |
| Test files | 42 files, 69 registries, ~300–340 cases |
| Save format | world.sav **v61** (min accepted v49); player save "PLY7" v7 |
| Wire protocol | v1, 40+ message types, 8-bit capability mask |

Recent commit history shows three active threads: the Arweave/Irys deploy pipeline stabilization (a dozen fix-up commits in early June), neural NPC brains (checkpoint embedded in WASM, `?neural` flag), and the on-chain track (burn-to-mint program, `/mine.html` RATi mining page, station avatar keypairs).

Note: `CLAUDE.md` says save v53 and `ENG.md` references v53 in places; the code is at **v61** (`sim_save.c:85`). v54–v61 added contract provenance flags, origin bans, fracture-child sidecar metadata, delivery shipment ledgers, and contract target pubkeys. Docs lag code by roughly 8 save versions.

---

## 3. Architecture Overview

Three-way split, enforced by CMake source lists:

- **`server/`** — authoritative sim. ~21 sim translation units (`game_sim.c` plus `sim_{physics,ai,asteroid,mining,production,construction,nav,autopilot,flight,ship,anchor,catalog,save}.c`), plus chain log, station authority, gossip, receipt issuance, and the mongoose WebSocket frontend (`main.c`).
- **`client/`** — sokol-based render/HUD/input/audio plus net transport (`net.c`), reconciliation (`net_sync.c`), and an in-process server for singleplayer (`local_server.c`).
- **`shared/`** — wire protocol (`protocol.h`), core types (`types.h`, 1,356 lines), economy constants, module schema, manifest/receipt/settlement code, fixed-point math, crypto.

**Singleplayer = multiplayer with the server in-process.** `local_server.c` (~200 lines) calls the same `world_sim_step()` and memcpy-mirrors the world into the client each frame (`local_server.c:47-77`). The interpolation interval switches from `SIM_DT` (singleplayer) to 100 ms (multiplayer packet cadence) at `client/main.c:235-243`. This is the single best architectural decision in the codebase: every sim feature is automatically multiplayer-correct.

### 3.1 The 120 Hz tick

`world_sim_step(world_t*, float dt)` at `game_sim.c:6301`, `SIM_DT = 1/120`. Per-tick order: ring dynamics → station jostle → asteroid dynamics → cargo pods → field maintenance → **gravity/collisions at 30 Hz** (accumulator-gated, O(N²) within a 800-unit spatial hash, 3×3 neighborhood, max 16 entries/cell) → fracture claims → smelting → refinery/station production → module flow/activation → frontier director → scaffolds → contracts → deliveries → NPC ships → players → ship-ship ramming.

**Entity pools (all static, capacities pinned by the v1 wire protocol):**

| Pool | Cap | Wire index |
|---|---|---|
| Asteroids | 2,048 | u16 |
| Stations | 128 | u8 |
| NPC ships | 100 | u8 |
| Players | 32 | u8 |
| Scaffolds | 16 | u8 |
| Cargo pods | 64 | u8 |

Lifting any cap requires protocol v2 (#285, "streaming entity pool"), which is also the declared precondition for splitting `game_sim.c` (banner comment at `game_sim.c:8-21`: "DO NOT MECHANICALLY SPLIT THIS FILE").

### 3.2 Determinism status

- Deterministic-by-construction: single `uint32_t` world RNG (`shared/rng.h`), belt seed anchoring procedural rocks, fracture seeds computed from quantized inputs (`sim_asteroid.c:565`), rock identity as `SHA256("rock-v1" || belt_seed || chunk || slot)`.
- `shared/fixpoint.c` provides deterministic `sqrt/sin/cos/atan2/exp/pow` replacements and the build sets `-ffp-contract=off -fno-fast-math`. As of 2026-06-13, deterministic helpers are integrated through the authoritative `server/` and `shared/` sim surfaces covered by `make deterministic-libm`, and the `signal_replay` harness is included in the ratchet because it emits the cross-build evidence. The default replay checks cover thirteen scenarios, including mining→fracture, fragment identity hash inputs, collision→death recovery, planned-outpost scaffold materialization with station construction/contract state hashed, station-pair jostle with transient jostle velocity hashed, player-player ram collision with all connected player bodies hashed, and NPC-NPC ram collision with paired authoritative NPC `ship_t` bodies hashed. **Sim state is still float throughout**, so #588 is not complete, but the project now has an incremental migration path with guardrails instead of a dormant fixed-point library.

---

## 4. Simulation Systems

### 4.1 Physics and combat

Asteroid gravity: `F = m₁m₂/d² × 14.0`, clamped, at 30 Hz over the spatial hash. Industrial pull draws rocks toward stations scaled by intake-module count (range 600–1500u, 3× multiplier for S-tier fragments feeding hoppers). "Signal pressure" pushes rocks down the signal gradient toward the frontier so cores don't strip-mine the fringe.

Combat is exactly what CLAUDE.md promises — the only weapon is a tractored fragment released at speed. Ramming damage: `max(0, impact − 115·0.7) × 0.12` (`game_sim.h:69-72`). Tractor: 380 u/s² acceleration, 210 u/s fragment speed cap, 220u nearby range. There is no weapon code path anywhere else; the design rule is structurally enforced.

### 4.2 Mining and fracture claims

Beam range 170u; tier gate by laser level (XL→L→M→S, plus XXL). Fracture spawns 1–4 children with deterministic seeds and outward drift. The interesting mechanic is the **fracture claim window**: on fracture the server opens a deadline; the client brute-forces nonces (cap 50–200 graded by distance from origin) over `(fracture_seed, player_pubkey, nonce)` to derive an Ed25519 keypair whose base58 prefix determines the ore *grade* (RATi/M/H/T/S/F/K prefix classes — `mining_client.h:64`). The server re-derives and arbitrates. This is effectively a proof-of-work mini-lottery that makes rare rocks cryptographically scarce, and it feeds directly into prefix-class price multipliers (up to 50× for RATI, 100× for COMMISSIONED — `economy_const.h:31-41`).

Destroyed terrain rocks go into a permanent tombstone ledger. The original audit flagged the former `destroyed_rocks[256]` overflow behavior as an integrity risk; `5248298` raised the tombstone cap and reduced the immediate cliff, though long-term protocol-v2/side-file work is still the right end state.

### 4.3 NPC AI — three brains

1. **Heuristic state machines** (default): miner (find rock → mine → return) and hauler (negotiate contract from gossip pool → load → travel → unload) in `sim_ai.c`. NPCs spawn at 3 s intervals with session tokens prefixed "NPC", and run on forced-debit credit (no starter balance).
2. **Neural flight** (`signal_brain.c`): a 4-layer ReLU network over 48 features producing 9 discrete WASD actions, loaded from `.nnckpt` checkpoints — embedded in the WASM build and toggleable via `?neural`. Training data comes from `tools/flight_trace.c` and the deterministic replay harness.
3. **Holographic memory** (`shared/holographic_nn.c`): a Vector Symbolic Architecture — 1024-dim hypervectors, circular-convolution bind/unbind, bundled (state→action) pairs. Docked NPCs exchange experience with stations (`gossip_hnn_exchange`, `sim_ai.c:2737`), i.e., collective pilot memory diffuses through the dock network the same way contracts do.

A "frontier director" (inside `sim_ai.c`) plans outposts and virtual logistics; REMEDIATION_PLAN item 2 wants it extracted to `sim_frontier.c`.

### 4.4 Construction

Scaffold FSM: NASCENT → LOOSE → TOWING → SNAPPING → PLACED → (supply) → commissioned. Stations are 3 rings (3/6/9 slots at radii 180/340/520) plus core. Producers must pair with a HOPPER at the canonical adjacent slot (cross-ring pair rule). Outpost founding requires: inside signal but below 0.80 unboosted strength, ≥1500u from existing stations, 500 credits, free station slot. Module tech tree is schema-driven (`module_schema.c:15-146`): RELAY is root; HOPPER→FURNACE→FRAME_PRESS→SHIPYARD; LASER_FAB/TRACTOR_FAB hang off FURNACE.

### 4.5 Signal

256×256 cached grid at 200u cells (±25,600u coverage) with bilinear interpolation and an O(N stations) raw fallback. Strength = Σ strength/(d²+ε) over connected stations; connectivity is a BFS from root stations through relays. Four bands (FRONTIER <0.15, FRINGE, OPERATIONAL, CORE >0.80) gate mining efficiency, control responsiveness, NPC confidence, and autopilot availability. A 100-message signal channel (station broadcast log, ~44 KB ring) persists to the on-disk hash chain.

---

## 5. Economy and Cryptographic Provenance

### 5.1 Sovereign ledgers

`station_t.ledger[16]` — sixteen `ledger_entry_t` per station, keyed by Ed25519 pubkey (legacy session tokens map via pseudo-pubkey). Balance is `float`. `station_credit_pool() = -Σ balances` and may go arbitrarily negative by design (currency in circulation, not a stored resource). Pricing: buy `base × (1 - 0.5·fill)`, sell `base × (1 + deficit²)` (`commodity.c:177,193`), times prefix-class multipliers. The original audit flagged silent rejection at the 17th trader as a scaling cliff; `38d3c3d` added protection around capacity behavior, but a larger dynamic ledger remains the long-term fix.

### 5.2 Three cargo states

Exactly as `docs/cargo-architecture.md` specifies:

1. **Fragment** — physical `asteroid_t`, identity = `fragment_pub[32]`, towed as u16 indices, never a manifest row.
2. **Bulk float** — vestigial raw-ore hopper float (`_inventory_cache`); the old hopper smelt path is retired and regression-counters (`hopper_smelt_events`, `game_sim.c:525`) verify it stays dead.
3. **Crate** — `cargo_unit_t`, exactly 80 bytes (`_Static_assert`, `manifest.h:9`), with `pub[32]` content hash and `parent_merkle[32]`: fragment_pub for smelts, sorted-input merkle root for crafts, zero for legacy migrations. Ship manifest cap 32, station cap 256.

Finished-goods invariant `floor(inventory[c]) == manifest_count_by_commodity(c)` is asserted in debug builds (`manifest.c:14-22`).

### 5.3 Receipt chains and chain logs (the "off-chain blockchain")

- **Chain log (Layer C):** per-station append-only log at `chain/<base58 pubkey>.log`. 184-byte event headers, Ed25519-signed over a 120-byte span, linked by `prev_hash = SHA256(prev header)`, with segment-boundary resets for world wipes. Event types cover SMELT/CRAFT/TRANSFER/TRADE/LEDGER/ROCK_DESTROY/TOW/RELEASE/DEATH/OPERATOR_POST. `chain_log_verify_with_pubkey()` (`chain_log_verify.c:106`) checks authority, linkage, monotonicity, payload hash, and signature; failure at boot blocks further appends (`chain_log.c:302`). The live `chain/` dir holds 188+ logs, the main station's at 576 KB (~3,100 events).
- **Receipt chains (Layer D):** 208-byte `cargo_receipt_t` per transfer, max 16 links per crate, origin-anchored to the SMELT/CRAFT chain event hash, verified on presentation (`cargo_receipt.c:85-119`). Invariant: receipt count == manifest count.
- **Settlement engine (Layer E groundwork):** `shared/settlement_engine.c` is a forward-apply state machine producing a Merkle state root over manifests/ledgers/fragment owners/credit notes — built and tested (`test_cross_station_settlement.c`, 968 lines) but explicitly draft; cross-zone handoff (`handoff_flow.c`) is a skeleton.

### 5.4 Gossip (no station radio)

The `stations-don't-talk` rule is enforced structurally: contracts spread only via `gossip_dock_handshake()` (bidirectional merge of bounded `known_contracts` pools, FIFO eviction). One sanctioned violation: `gossip_bootstrap_world_stations()` floods all pools once at world load to avoid cold-start deadlock (`gossip.c:80`).

---

## 6. Networking and Protocol

- **Binary little-endian protocol, v1, 40+ message types (0x01–0x47).** Fixed record sizes (player 77 B, asteroid 35 B) with `compact_u16` varints for counts. `NET_MSG_PROTOCOL_INFO` (0x41) self-describes every stream's cadence/record size/caps, also exposed as HTTP `/api/protocol` — external tooling discovers wire layout instead of hardcoding it. 8-bit capability bitmask in lieu of version negotiation.
- **Cadences:** player state 20 Hz, world/asteroids 10 Hz, autosave 30 s. Relevance filtering at ~3000u view radius keeps asteroid/NPC broadcast O(N·viewers).
- **Prediction:** client replays up to 512 recorded input frames (`net_replay`, `client.h:446`) against server acks; input lead derived from RTT, clamped 12 ticks; render-offset smoothing eases corrections over 180–340 ms. One-shot actions ride a separate 8-deep ack'd action queue with 6 s resend.
- **Auth handshake:** session token (8 B) → `NET_MSG_REGISTER_PUBKEY` (33 B) → `NET_MSG_PROVE_PUBKEY` (Ed25519 over `"prove-pubkey-v1" || pubkey || token`) → save loaded from `saves/pubkey/`. Pubkey collision with a live session evicts the old session and transfers ledger entries (`main.c:1078-1119`). State-changing actions go over `NET_MSG_SIGNED_ACTION` with a monotonic nonce; movement stays unsigned for throughput.
- **Limits:** 4 connections/IP, 5 s session-handshake timeout, 30 s reconnect grace, 20 req/s token bucket on `/api/*`.

---

## 7. Client

- **Frame loop:** fixed-step accumulator, max 8 sim steps/frame, frame dt clamped to 100 ms for tab-resume (`main.c:1898-1911`). Single-threaded.
- **Rendering:** pure immediate-mode `sokol_gl` + `sokol_debugtext` — no shaders, no materials. Procedural geometry with screen-ratio LOD (6 segments for tiny circles), simple AABB frustum culling, palette-driven colors per module/commodity/grade. Draw order puts station rings above ships deliberately.
- **Camera:** deadzone free-flight, station edge-latch when docked, death cinematic, scan-target framing, boost zoom — all in `main.c:1420-1593`.
- **Identity (Layer A):** Ed25519 keypair generated client-side, stored as raw 64 bytes in platform-appropriate dirs (localStorage on web, Application Support on macOS, XDG on Linux, LOCALAPPDATA on Windows) — `identity.c:44-102`.
- **Media:** 10 unlockable MPEG-1 episode cutscenes via `pl_mpeg` streamed from S3; 24 gameplay + 4 death MP3 tracks via `minimp3` with a signal-band-driven shuffle; all mixed into one sokol_audio callback.
- **Onboarding:** in-world visual objectives (move/fracture/tractor/hail/boost), persisted in localStorage — consistent with the "text is subtitles only" design rule.
- **Telemetry:** 50+ `EMSCRIPTEN_KEEPALIVE` exports for net-motion metrics, consumed by the Playwright smoke suite.

---

## 8. Persistence

- **world.sav:** magic "SIGN", **v61**, min v49, CRC32 trailers, written atomically (.tmp → rename, `sim_save.c:1369`). Thirteen documented migration steps v49→v61 with per-field gating; idempotent live migrations (e.g., auto-tagging furnace hoppers at v50 load).
- **Player saves:** `saves/pubkey/<base58>.sav` ("PLY7" format, carries manifest + receipt chains + last signed nonce); `saves/legacy/<token_hex>.sav` fallback. Legacy claim: sign `"claim-legacy-save-v1" || token_hex`, server renames into pubkey dir, first-claim-wins. Since `2118f9f` and `b2b0da1`, player saves use atomic writes and CRC/staged-load hardening so corrupt files are rejected before live player state is replaced.
- **Chain-log/save reconciliation:** the save stores `chain_last_hash`/`chain_event_count`; if the on-disk log verifies and is ahead of the save (crash after append), the verified tail is adopted at load.

**Remaining gaps:** no `fsync` anywhere (rename atomicity only); legacy-save claiming is now audited but still fundamentally proves key possession rather than original ownership. Full historical ownership remains a chain-log/provenance problem, not just a file-rename problem.

---

## 9. Decentralization / On-Chain Stack

The layered plan (docs/decentralization.md) maps cleanly to reality:

| Layer | What | Status |
|---|---|---|
| A | Player Ed25519 identity + signed actions | **Shipped** |
| B | Station authority (deterministic keypairs: `SHA256("signal-station-v1" \|\| operator_secret \|\| world_seed \|\| idx)`) | **Shipped** |
| C | Signed append-only chain logs | **Shipped** |
| D | Cargo receipt chains | **Shipped (off-chain)** |
| E | `signal_verify` auditor + asset exporter | **Shipped** |
| F | Cross-operator handoff tickets | Partial (skeleton) |
| #480 | On-chain state-root anchoring, wrapped assets, fork resistance | Designed only |
| #588 | Fixed-point sim → P2P determinism | In progress: deterministic helper integration + ratchet landed; float state remains |

Notable: `programs/burn-to-mint/onchain-c/src/rati_burn_to_mint.c` (1,277 lines) is a **real Solana native program in C** — 10 instructions, fixed-ratio + bonding-curve pricing, checked arithmetic, PDA derivation, golden test vectors, and a versioned spec. It is complete but unintegrated: there is no `server/solana_bridge.c`, no wallet-link message, no furnace UI flow. Station tokens (RUBY/KYRO/RATi) and yield-split NFTs are design docs only. The honest gap-tracking between docs and code is itself a strength — every aspirational doc names the issue that blocks it.

Operator onboarding (federation) is documented to production depth: hardware sizing (single core, 512 MB, ~50 MB/day chain growth), key custody, chain verification on every boot, and the trust model (sim < chain log < federation).

---

## 10. Build, Test, CI, Deploy

### Build
CMake, Ninja-preferred; 8 targets (server, client, WASM client, test binary, 4 tools). `-Wall -Wextra -Wpedantic -Werror`, `-fstack-protector-strong`, strict IEEE FP flags, `_FORTIFY_SOURCE=2`; MSVC `/W4 /WX`. tweetnacl compiled with sanitizers selectively disabled (reference-impl UB). GIT_HASH baked into `/health` and client mismatch detection. Eleven build trees on disk (asan/ubsan/tsan/valgrind/coverage/web/...).

### Tests
Custom harness (`tests/c/test_harness.h`): 42 files / 69 registries / ~340 cases, modulo-sharding (`--shard=K/N`, default min(8, nproc)), `--filter`, quiet mode, soak gating (`RUN_SOAK` ≈ 75% of wall-clock, skipped by default; `-O2 -g` test builds cut the suite 180 s → 56 s). Cleanup attributes for fixtures. Seven bug-regression batches institutionalize "every fixed bug gets a test" (c_safety_policy.md). Banned-API scanner (`gets/strcpy/sprintf/atoi/rand/...`), cppcheck with vendor suppressions, CRAP scoring script.

### CI (3 workflows)
- `deploy-arweave.yml` — push-to-main: blocking fast C suite, ASan/UBSan suite, browser smoke, banned-API scanner, deterministic-libm ratchet, then Emscripten build → Irys upload to Arweave (content-hash cached) → manifest to Cloudflare KV.
- `release.yml` — on release: native binaries for Linux/macOS-arm64/Windows + server tarball.
- `valgrind.yml` — nightly 8-shard memcheck (definite leaks only, non-soak only).

### Deploy
WASM + HTML live permanently on Arweave; `workers/arweave-proxy.js` on `signal.ratimics.com/*` resolves paths through a KV-cached manifest with multi-gateway fallback. Game server is a Docker container (port 9091 WS + 8080 static dev server) with a clever local-dev loop: a post-commit hook rebuilds WASM + server (incremental Alpine builder) and redeploys to localhost in seconds, then runs the fast test suite in the background; a pre-push hook consults the cached verdict.

### Pipeline gaps
- The original "nothing blocks a bad deploy" finding is closed: deploy now depends on fast C tests, sanitizers, browser smoke, banned APIs, and deterministic-libm checks.
- TSan, cppcheck, and clang-tidy are still not blocking deploy.
- KV manifest update still has no rollback; deploys are all-or-nothing.
- The June commit log (≈12 consecutive deploy-fix commits) remains useful evidence for why the new gate matters.

---

## 11. Risk Register

Ordered by (impact × likelihood), with the cheapest mitigation named.

1. **#588 float→fixed rewrite** — still the largest decentralization blocker. The raw-transcendental surface is now ratcheted across `server/`, `shared/`, and deterministic replay code, but sim state remains float. Keep broadening replay scenarios and migrate high-value state/accounting fields to fixed or integer representation where cross-world consensus needs exact agreement.
2. **Float currency** — ledger balances are `float`; large balances lose integer precision past 2^24. `749ec0e` hardened ledger float handling, but the right substrate for station credits is fixed/integer accounting.
3. **Monolith risk concentration** — `game_sim.c`, server `main.c`, `hud.c`/`world_draw.c`, and parts of `sim_ai.c` are still large change-conflict and review-risk centers. The #285-gating rationale is sound for `game_sim.c`, but `main.c`'s handler table and `sim_ai.c`'s frontier director are extractable now.
4. **Legacy-save ownership semantics** — claim auditing now exists, but possession-of-keypair still is not the same as original ownership. Full resolution needs chain-log-backed claim history.
5. **Destroyed-rock tombstone scaling** — immediate overflow pressure is reduced by `5248298`, but the long-term permanent-ledger property still wants side-file/protocol-v2 storage.
6. **CI static analysis depth** — deploy is now gated by tests/sanitizers/smoke and project scanners, but cppcheck/clang-tidy/TSan remain non-blocking.
7. **KV deploy rollback** — Cloudflare KV manifest updates remain all-or-nothing with no rollback path.
8. **Full `fsync` durability** — player saves are atomic at the rename level, but neither player nor world save paths fsync file + directory.
9. **Dynamic station ledger capacity** — capacity behavior is protected, but high-concurrency stations still want a larger/dynamic ledger design.
10. **On-chain bridge integration** — the Solana burn-to-mint program is real but still unintegrated with server/wallet/UI flows.

---

## 12. What's Working Well

Worth stating explicitly, because most of it is non-obvious discipline:

- **One sim, two transports.** Singleplayer embedding the real server eliminates the entire class of "works offline, breaks online" bugs.
- **Design rules enforced structurally, not by convention.** No weapons but rocks (no other damage path exists); no station radio (gossip is the only contract transport); cargo three-state model (fragments physically cannot become crates without a smelt event).
- **Wire-format honesty.** `_Static_assert`s on every serialized struct, protocol self-description endpoint, explicit per-version save migration with 13 documented steps.
- **Provenance-first economy.** Receipt chains, signed chain logs, and the fracture-claim PoW lottery mean the eventual on-chain bridge wraps *already-verified* state rather than retrofitting trust.
- **Docs that admit what isn't built.** Every aspirational doc names its blocking issue; REMEDIATION_PLAN.md is a candid ranked debt list. CLAUDE.md/ENG.md just need a save-version refresh (v53 → v61).
- **The test culture.** Custom harness with sharding/soak gating, regression batches per bug, banned-API enforcement, deterministic replay harness for counterfactual testing, and a commit-to-localhost-in-seconds loop.

---

*Methodology: six parallel exploration passes (sim core, economy/provenance, client, protocol/persistence, build/CI/deploy, docs/on-chain) plus git-history analysis, reconciled against source. File:line references verified against main @ `31197b1`; 2026-06-13 remediation notes reconciled through the deterministic replay NPC-ram slice.*
