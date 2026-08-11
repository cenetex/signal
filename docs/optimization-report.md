# Optimization Report — Server Sim & Client Hot Paths

*Updated 2026-07-04 with a measured CPU profile — see "Measured profile"
below. The measurement reorders the static rankings: fixed-point math and
`maintain_asteroid_field` dominate the real workload; the NPC/station scan
findings are real but secondary at current entity counts.*

*Updated again 2026-07-04 after the measured hot-path pass. Slices 0-4
landed as independent commits: SIM_PROFILE phase timing, O(1)
`chunk_materialized` lookup, compact station scans, conservative station
collision broad phase, and hardware `sqrtf` for `v2_len`.*

*Updated 2026-07-26 after the dense-asteroid scheduler pass: spatial cells
retain every body, gravity and collision share one bounded deterministic
pair plan, and dense native/WASM replay parity is now a fast-suite gate.*

*Updated 2026-07-26 after the #663 backpressure pass: the soak client now
uses the authenticated signed-action path, transfer appends advance a
verified provenance cache under fail-closed invariants, and the unrelaxed
10-minute latency/RSS/backpressure acceptance run passes.*

*Date: 2026-07-03. Scope: static analysis of the 120 Hz authoritative sim
(`server/`), the network broadcast layer (`server/main.c`,
`server/net_protocol.h`), and the client render path (`client/world_draw.c`).
No profiler run yet — the "estimated cost" figures below are worst-case
arithmetic from the loop bounds (`MAX_ASTEROIDS = 2048`, `MAX_STATIONS = 128`,
`MAX_NPC_SHIPS = 100`, `MAX_PLAYERS = 32`, 120 Hz tick). Real load is lower
(active counts, early-outs), but the *shapes* are what matter: several loops
scale as `entities × stations × 120 Hz` and will degrade quadratically as the
world fills in.*

## Measured profile (2026-07-04)

`signal_server` (RelWithDebInfo, Apple Silicon, macOS `sample`, 10 s, 1 ms
interval), running the repo's `world.sav` with its NPC roster and no
connected clients. The sim consumed **1,387 of ~10,000 samples ≈ 14% of one
core ≈ 1.2 ms per 120 Hz tick**. Native desktop absorbs that; in local
(in-process) mode the client runs *two* sim steps per 60 fps frame on the
render thread, and in the WASM build — where the hot leaf below is several
times slower — sim alone plausibly eats most of the 16.6 ms frame budget.
When a frame overruns, `advance_simulation_frame` (`client/main.c:2222`)
runs up to 8 catch-up steps then drops accumulated time, which presents as
stutter/slow-motion: exactly the reported "laggy in local mode."

Breakdown of sim samples:

| Share | Where | What |
|-------|-------|------|
| ~25% | `step_asteroid_gravity` (`sim_physics.c:56`) | Pairwise gravity at 30 Hz. **Over half of this is `fixp_sqrtf` → `__udivmodti4`** — 128-bit soft division inside `v2_len`. |
| ~21% | cargo pod stepping (`step_station_cargo_pod_tractors`, `step_cargo_pods`, `resolve_cargo_pod_circle_collision`, `cargo_pod_find_station_work_module`) | Per-pod station/module scans at 120 Hz. |
| ~13% | `maintain_asteroid_field` (`sim_asteroid.c:1042`) | `chunk_materialized` is a linear scan of all 2,048 asteroid slots **per chunk queried**, × (2·radius+1)² chunks × every viewport (players + every active NPC), at 5 Hz. |
| ~9% | `resolve_asteroid_collisions` + `resolve_asteroid_station_collisions` | 30 Hz collision pass (static Findings 2/5 territory). |
| ~8% | `step_station_ring_dynamics` / `step_station_jostle` + `module_world_pos_ring` + `fixp_sinf/cosf` | Per-module trig at 120 Hz. |
| ~7% | `sim_step_asteroid_dynamics` | Static Finding 1 (station scans per asteroid). |
| ~3% | NPC collision (`npc_resolve_asteroid_collisions`, `station_build_geom`) | Static Finding 2 — real, but small at the current NPC activity level. |

## Hot-path pass result (2026-07-04)

Post-pass `SIM_PROFILE` sample, `signal_server` RelWithDebInfo with
`SIGNAL_SIM_PROFILE=ON`, temp copy of the repo `world.sav` plus `chain/` and
`stations/`, no connected clients, 10-second profile window:

```text
[sim-profile] ticks=1196 sim_sec=10.001 measured_ms=188.283
[sim-profile] phase=book        share=  0.04 total=0.078ms avg=0.000ms max=0.013ms samples=1197
[sim-profile] phase=stations    share=  0.93 total=1.748ms avg=0.001ms max=0.005ms samples=1197
[sim-profile] phase=asteroids   share=  6.12 total=11.531ms avg=0.010ms max=0.854ms samples=1197
[sim-profile] phase=cargo       share= 34.97 total=65.848ms avg=0.055ms max=0.092ms samples=1197
[sim-profile] phase=gravity     share= 29.72 total=55.950ms avg=0.047ms max=0.345ms samples=1197
[sim-profile] phase=production  share=  5.08 total=9.556ms avg=0.008ms max=0.022ms samples=1197
[sim-profile] phase=manifest    share=  0.16 total=0.295ms avg=0.000ms max=0.001ms samples=1197
[sim-profile] phase=objects     share=  3.71 total=6.976ms avg=0.006ms max=0.021ms samples=1197
[sim-profile] phase=npcs        share= 14.36 total=27.039ms avg=0.023ms max=0.142ms samples=1197
[sim-profile] phase=players     share=  0.02 total=0.029ms avg=0.000ms max=0.001ms samples=1197
[sim-profile] phase=collisions  share=  0.42 total=0.799ms avg=0.001ms max=0.002ms samples=1197
[sim-profile] phase=sync        share=  4.48 total=8.434ms avg=0.007ms max=0.028ms samples=1197
```

That is **188.3 ms / 1196 ticks = 0.157 ms/tick**, comfortably below the
0.6 ms/tick target and roughly an 87% reduction from the original
`sample`-derived 1.2 ms/tick estimate on the same save shape. A second clean
window in the same run reported 182.9 ms / 1196 ticks = 0.153 ms/tick.

Validation gates:

| Gate | Result |
|------|--------|
| `make deterministic-libm` | pass |
| `make test-fast` | 1137/1137 pass |
| `make replay-repeatability` | pass, 20 fast scenarios |
| `make replay-native-wasm` at pre-Slice-4 parent `47dde71` | pass, 20 fast scenarios |
| `make replay-native-wasm` | pass, 20 fast scenarios |
| `make replay-native-wasm-long` | pass, 4 long scenarios |
| `make signal-no-omniscience-soak` | pass, 5 deterministic scenarios |
| `make build-server` | pass |
| `make build-web` | pass |

Slice 4 inventory: no baked replay state-hash goldens were found under
`tests/` or the replay corpus before switching `v2_len` to hardware
`sqrtf`; current replay gates compare freshly generated native/native or
native/WASM runs. The deterministic-libm ratchet still bans raw libm calls in
deterministic code except the single intentional `sqrtf` inside `v2_len`.

The new ranking is also sharper than the old call-stack sample: cargo and
gravity remain the next real targets, with NPC work now visible at ~14%.
`maintain_asteroid_field` has dropped into the asteroid phase bucket at ~6%.

## WebSocket backpressure and provenance tail latency (2026-07-26)

The #663 soak now follows the production authentication path: an ephemeral
Ed25519 identity registers, completes the v2 session proof, and submits
signed BUY/DELIVER actions while a second client stops reading. Samples
received while the harness takes a phase-boundary health reading are
discarded, so late replies cannot leak into the next latency cohort.

Profiling the initial failing run isolated the tail-latency growth to origin
authority evaluation, not the socket queue or durable append. Evaluation
grew to 79 ms p50 / 104 ms p95 as the station history lengthened; receipt
linking was effectively 0 ms, signing about 2 ms, serialization/crypto about
4–6 ms, and `fsync` 0.35 ms p95. Each successful local transfer had
invalidated the verified origin index, so the next action replayed the whole
history.

The fix advances a verified cache only across the exact transaction that
appends TRANSFER or TRANSFER+TRADE events. It holds the verified file object
open and requires the same cache generation, authority/configuration,
registry fingerprint, station event count and tail, file identity, exact
byte growth, appended event IDs, and final tail hash. Any mismatch discards
the entry and forces full verification; transforms, rotation, and other
generic appends still rebuild. This relies on the server's documented
synchronous single-writer chain-log model. Focused regressions keep
`full_verifications` and `index_builds` at one across one- and two-event
transfers, require a later SMELT to rebuild, and reject a tampered cached
prefix rather than advancing it.

The final 10-minute acceptance run passed without changing the original
absolute gates (p95/p99 at most 100/250 ms) or comparative gates (slow-reader
increase at most 25/50 ms):

| Signal | Baseline | Slow-reader phase |
|---|---:|---:|
| ACK p95 / p99 | 17 / 21 ms | 18 / 21 ms |
| Server-excluded p99 | 12 ms | 12 ms |
| Ping p99 | 12 ms | 12 ms |
| Authoritative tick rate | 119.895 Hz | 120.003 Hz |

The slow-reader phase's minimum tick interval was 118.57 Hz. All 59 signed
actions were verified (30 buys, 29 deliveries); the outbound queue peaked at
172,033 bytes against its 262,144-byte bound. The stalled connection was
closed for `no_write_progress` after 21.414 seconds. RSS peaked at
47,284,224 bytes with 5,816,320 bytes growth, and the run reported no
failures.

### M1. `v2_len` routed through software 128-bit division — **resolved**

Before this pass, `shared/math_util.h:66` defined `v2_len` as
`fixp_sqrtf(v2_len_sq(...))`. `fixp_sqrtf` (`shared/fixpoint.c:109`)
converts float → q32.32, takes an integer-sqrt initial guess, then runs
**up to 8 Newton iterations, each performing a `fixp_div` = 128÷64-bit
division** (`__udivmodti4` in compiler-rt — software, even on ARM64; far
worse in WASM where it's emulated i64 math). Every vector length in the
entire sim paid this. The single hottest leaf in the original profile.

Implemented: `v2_len` now uses hardware `sqrtf` for finite positive squared
lengths and preserves the old 0.0f result for zero/nonfinite inputs. Native,
WASM, long-horizon replay, and no-omniscience gates all passed. If a future
platform ever rejects this assumption, the fallback remains a division-free
`fixp_sqrt_fixp` rewrite, but the current deterministic evidence supports the
hardware path.

### M2. `chunk_materialized` linear scan — **resolved**

`maintain_asteroid_field` now builds a small open-addressed set of
materialized `(cx,cy)` chunks once per call and uses O(1) lookups in the
materialization loop. The behavior is intentionally equivalent to the old
"at least one active terrain asteroid exists in the chunk" predicate.

### M3. Cargo pod stepping — **measured P1**

~21% of sim time at 120 Hz across the pod tractor/handoff/collision
helpers, with per-pod scans over stations and modules
(`cargo_pod_find_station_work_module`, `cargo_pod_current_module_tractor_valid`).
The active-pod count is small (`MAX_CARGO_PODS` = 64) — the cost is the
station/module scan shape inside each helper, the same pattern as static
Finding 1, plus `resolve_cargo_pod_circle_collision` against station
geometry. Benefits from the same compact active-station array and shared
per-tick geometry cache.

The static findings below stand, but at current entity counts they are the
*next* tier — they become dominant as stations/NPCs scale (the #285 concern),
while M1–M3 are what's slow today.

## What is already good

Credit where due — these are done right and should not be touched:

- **Asteroid spatial grid** (`game_sim.c`, `game_sim.h`): 800-unit cells
  retain up to the full `MAX_ASTEROIDS` pool. Navigation/proximity queries
  see every body; gravity and collision consume the bounded pair plan
  described below.
- **30 Hz gate on N-body work** (`game_sim.c:11242`): gravity + asteroid
  collisions run at 30 Hz, not 120 Hz.
- **Signal strength cache grid** (`game_sim.c:645`): 256² precomputed grid
  with bilinear lookup, rebuilt only on topology change. (One leak in it —
  see Finding 3.)
- **Netcode tiering** (`server/main.c:782-790`): 120 Hz sim / 20 Hz player
  state / 10 Hz world / 4 Hz ship detail, per-player viewport culling and
  byte budgets for asteroid serialization, dirty flags, snapshot scratch
  buffers, WS backpressure deferral.
- **Client render lists** (`world_draw.c:260-282`): per-frame cached
  S-tier/smelting index lists instead of rescanning per draw pass.

### Dense asteroid pair ownership and fairness

`asteroid_pair_plan_build` constructs one immutable, fixed-capacity plan
from the already-built grid at each 30 Hz solve. Both
`step_asteroid_gravity` and `resolve_asteroid_collisions` walk that exact
plan, so a pair has one owner and cannot be omitted by one path or processed
twice by the other.

- Cells are sorted by `(y, x)`. Only the four forward neighbors
  `(1,0), (-1,1), (0,1), (1,1)` own cross-cell pairs; together with the
  same-cell owner, this covers the 3×3 neighborhood exactly once.
- Bodies are ordered by authoritative identity first: `rock_pub`, then
  fracture/fragment identity. The explicit semantic record is the
  tie-breaker. Slot is used only for an exact anonymous-record tie; that
  legacy/test-only fallback is valid only when aligned metadata and live
  external references are also equal/absent, which the regression asserts.
- A same cell of at most 16 bodies is exhaustive. A denser cell processes
  four non-wrapping cyclic-distance bands per epoch and visits every
  unordered pair exactly once within
  `ceil(floor(N/2) / 4)` 30 Hz epochs, for both odd and even `N`.
- A neighboring-cell product of at most 256 is exhaustive. A denser
  cross-cell product processes four deterministic bipartite offsets per
  epoch and visits every pair exactly once within
  `ceil(min(A,B) / 4)` epochs, including unequal cell populations.
- The phase is `world.tick / 4`. Save load reconstructs `world.tick` from
  persisted `world.time`; a boundary regression saves at tick 39, reloads
  in epoch 9, and advances to epoch 10 on tick 40.
- Storage is statically bounded: no pair array and no hot-path allocation.
  The proof bound is 72 candidates per active body (self cost at most
  `8N`, plus at most eight adjacent-cell contributions at
  `8(A+B)`), or 147,456 candidates at the full 2,048-body pool. A
  nine-adjacent-cell/full-pool regression asserts the bound.
- Signal-pressure's boolean proximity probe is intentionally not sampled;
  it scans every retained body in the 3×3 neighborhood because a boolean
  safety/crowding decision cannot fairly rotate omissions.

The focused benchmark times plan construction plus two complete plan walks
(the gravity and collision scheduling overhead);
`make asteroid-physics-bench` rebuilds and runs it. The table below was
recorded on Apple Silicon from the native RelWithDebInfo binary
(`-O2 -g -DNDEBUG`, deterministic floating-point flags), 2026-07-26:

| Bodies in one cell | Candidates/path at 30 Hz | Both paths per solve | Amortized callbacks per 120 Hz tick | Plan + two walks |
|---:|---:|---:|---:|---:|
| 16 | 120 | 240 | 60 | 1.91 µs |
| 32 | 128 | 256 | 64 | 2.13 µs |
| 64 | 256 | 512 | 128 | 2.71 µs |
| 128 | 512 | 1,024 | 256 | 3.99 µs |

At 128 bodies, scheduler overhead is about 0.004 ms per 30 Hz solve
(roughly 0.001 ms amortized per 120 Hz tick), leaving the existing
0.6 ms/tick whole-sim target essentially untouched. These numbers isolate
scheduling overhead; pair-specific distance/math cost remains in the
phase profiler.

The fast replay matrix also contains `dense-asteroids`: 32 overlapping
M/L bodies, 32 ticks, one deterministic candidate. Native and Emscripten
outputs were byte-identical on 2026-07-26 (SHA-256
`bc8de9b5d0e9d1bb42cb5bd7f5229d98062461c27e1b5bec7e4c8d1a2588d515`);
`make replay-native-wasm` runs the same scenario on every fast parity gate.

## Findings, ranked by estimated impact

### 1. `sim_step_asteroid_dynamics`: two full station scans per asteroid, per tick — **P1**

`server/sim_asteroid.c:696`. At 120 Hz, for every active asteroid:

- **Despawn check** (`sim_asteroid.c:750`): `point_within_signal_margin`
  scans all `MAX_STATIONS` slots.
- **Station vortex** (`sim_asteroid.c:771`): a second loop over all
  `MAX_STATIONS` slots with `station_exists` + `v2_dist_sq` each.

Worst case: `2048 asteroids × 256 station touches × 120 Hz ≈ 63M`
station-struct touches/sec. `station_t` is large, so iterating 128 of them
per asteroid is also cache-hostile — most of the cost is memory traffic on
mostly-empty slots.

**Fix (cheap, big win):** once per tick, build a compact array of active
stations — `{pos, signal_range, dock_radius, has_intake}` (a few hundred
bytes) — and run both inner loops over that. With 3–10 active stations this
removes >90% of the traffic. Additionally, the despawn check does not need
120 Hz: stagger it (e.g. asteroid `i` checks on tick `t` when
`(t + i) % 32 == 0`).

### 2. NPC collision has no broad phase and rebuilds station geometry per NPC — **P1**

`server/sim_ai.c:5644` (`npc_resolve_station_collisions`): for each undocked
NPC, every tick, loops all stations and calls `station_build_geom` for every
`station_collides` station **before any distance check**. Geometry build
iterates every module and emits circles/corridors/spokes/docks — it is the
single most expensive helper in the sim, and here it runs
`undocked NPCs × colliding stations × 120 Hz` times, almost always producing
geometry for stations the NPC is nowhere near.

`server/sim_ai.c:5709` (`npc_resolve_asteroid_collisions`): scans all 2048
asteroid slots per NPC per tick — `100 × 2048 × 120 ≈ 24.6M` slot
checks/sec — while a spatial grid for exactly this data already exists
(`w->asteroid_grid`).

**Fixes:**
- Broad phase first: skip a station when
  `dist_sq(npc, st->pos) > (st->dock_radius + margin)²`. One subtract/multiply
  versus a full geometry build. Same guard belongs in
  `resolve_asteroid_station_collisions` (`sim_physics.c:353`) — it currently
  runs every asteroid against every colliding station's full geometry at
  30 Hz.
- Use `w->asteroid_grid` (3×3 neighborhood) for NPC–asteroid collision. The
  grid is currently rebuilt only inside the 30 Hz gravity gate
  (`sim_physics.c:58`); rebuilding it every tick is cheap (one pass over
  asteroids, ~2048 inserts) and lets NPC + player collision share it.
- Per-tick geometry cache: build each colliding station's `station_geom_t`
  at most once per tick, on first request, into a heap-allocated cache
  (`MAX_STATIONS` slots ≈ 320 KB heap — the WASM-stack concern noted at
  `sim_physics.c:347` applies to stack, not heap), invalidated each tick.
  Every consumer — NPC collision, player `resolve_module_collisions`
  (`game_sim.c:4282`), asteroid–station collision, `sim_nav.c:119` — then
  shares one build per station per tick instead of one per caller.

### 3. `signal_strength_at` hides a full station scan in the "O(1)" path — **P1**

`server/game_sim.c:676`. The first line of every call is
`off_relay_dock_beacon_strength_world(w, pos)` (`game_sim.c:601`), which
loops all 128 station slots — and for any off-relay dock station iterates
its modules with `module_world_pos_ring` — *before* the cached grid is even
consulted. The comment says "O(1) signal lookup"; it is O(MAX_STATIONS)
every call.

Call volume is high: per NPC per tick (`sim_ai.c:5487`), per cargo pod, per
player (`game_sim.c:7693,7725,7782`), and **five times per eligible asteroid**
in the 30 Hz signal-pressure gradient sampler (`sim_physics.c:192-199`).
At a few hundred thousand calls/sec, the beacon pre-scan alone is tens of
millions of station touches/sec — for a feature (off-relay dock beacons)
that is empty in the common case.

**Fix:** maintain a tiny cached list of off-relay dock beacon positions
(station has docking, `signal_range == 0`, not planned/scaffold — this only
changes on topology events, exactly when `rebuild_signal_chain` already
runs). `signal_strength_at` then early-outs on `beacon_count == 0` and
otherwise checks a handful of positions. This also automatically fixes the
gradient sampler's 5× amplification.

### 4. Per-tick manifest reconciliation scans every station manifest — **P2**

`server/game_sim.c:11278-11290`. Every tick, for every existing station,
for every finished commodity, `manifest_count_by_commodity`
(`shared/manifest.c:620`) linearly scans the station manifest (cap 256
units). Worst case ≈ `stations × ~10 commodities × 256 units × 120 Hz` ≈
tens of millions of unit reads/sec, to detect a drift condition that can
only change on production/trade/delivery events.

**Fix (either):** keep a per-station `uint16 manifest_count[COMMODITY_COUNT]`
cache updated by the mint/remove helpers (they already exist as the
designated mutation points — `station_finished_mint` etc.), making the
reconciliation O(commodities); or gate the whole pass to every N ticks
(e.g. 2 Hz) — it is a drift detector, not gameplay physics.

### 5. Player collision resolution scans all asteroid slots — **P2**

`server/game_sim.c:4333` (`resolve_world_collisions`): all 2048 asteroid
slots per player per tick. Same fix as Finding 2: query the spatial grid's
3×3 neighborhood around the ship. Lower priority than the NPC version only
because live player counts are smaller than the NPC roster. The station
half of this function benefits from the same broad-phase + shared geometry
cache as Finding 2.

### 6. Autosave runs synchronous file I/O on the sim thread — **P3 (latency, not throughput)**

`server/main.c:795` (`AUTOSAVE_MS 30000`) triggers `world_save` +
per-player saves inside the mongoose poll loop — sequential `fwrite` of the
whole world (`sim_save.c`). Every 30 s the sim stalls for however long the
serialization + disk write takes; on a loaded world that is a visible
multi-tick hitch and a burst of client-side extrapolation.

**Fix:** serialize into a memory buffer on the sim thread (fast), hand the
buffer to a detached writer thread for the `fwrite`/`fsync`/rename. The
save format code doesn't change; only the destination becomes a buffer.

### 7. Smaller items / notes

- **`step_asteroid_gravity` industrial-pull loop** (`sim_physics.c:118`):
  all asteroids × all stations at 30 Hz. The per-station intake counts are
  already precomputed; the loop just needs the same compact
  active-station array as Finding 1.
- **Dense spatial-cell omission is resolved**: the grid retains the whole
  pool and the pair plan gives every same/cross-cell pair a formula-bounded
  revisit window. Grid `overflow_count` now represents allocation failure,
  not an intentional first-16 physics clamp.
- **`rebuild_signal_chain` flood fill** (`game_sim.c:541`) is O(passes ×
  N²) but runs only on topology changes — fine, leave it.
- **Client**: render lists are cached and asteroid serialization is
  viewport-culled server-side, so the draw loops over `MAX_ASTEROIDS`
  (`world_draw.c:270,460,...`) mostly skip inactive slots. `station_build_geom`
  per drawn station per frame (`world_draw.c:1746`) is acceptable at
  frame rate for on-screen stations; if a shared per-tick geom cache lands
  (Finding 2) the client can reuse the same pattern, but it is not urgent.
- **Player–player / NPC–NPC O(n²) collision** (`game_sim.c:11306,11353`):
  32² and 100² pair checks with cheap early-outs at 120 Hz — measurable but
  small; only worth touching after the items above, and the same spatial
  grid would cover it.

## Suggested order of attack

*Revised 2026-07-04 per the measured profile: steps 0a–0b come first — they
are what is actually slow today.*

| Step | Change | Effort | Expected effect |
|------|--------|--------|-----------------|
| done | `sqrtf` in `v2_len` | small | Deleted the hottest measured leaf; replay native/WASM gates passed |
| done | Hash set for `chunk_materialized` | small | Removed the linear asteroid-slot scan per candidate chunk |
| done | Compact station arrays for asteroid/vortex/industrial pull scans | small | Removed the dominant `asteroids × 128-slot` traffic in covered paths |
| done | Broad-phase distance guard before station collision geometry | small | Cuts far-away `station_build_geom` calls with a derived conservative bound |
| done | Dense asteroid pair plan | medium | Removed slot-order omissions while keeping a 72N fixed bound and native/WASM parity |
| 2 | Off-relay beacon list + early-out in `signal_strength_at` | small | Restores the O(1) promise of the signal cache |
| 4 | Rebuild spatial grid per tick; use it for NPC + player asteroid collision | medium | ~25M slot checks/sec → thousands |
| 5 | Per-tick shared `station_geom_t` cache (heap) | medium | One geom build per station per tick, all consumers |
| 6 | Manifest count cache or 2 Hz gate on reconciliation | small | Deletes a constant per-tick scan |
| 7 | Async autosave writer thread | medium | Removes 30 s hitch |

Steps 1–3 are independent, low-risk, and probably recover the bulk of the
headroom. Steps 4–5 touch collision behavior and deserve the replay soak
gate (`make test` + no-omniscience replay) before shipping.

## Measure before/after

Per-phase timing is now available behind `SIGNAL_SIM_PROFILE` at build time
and the `SIM_PROFILE` runtime env var. For native tools:

```sh
SIM_PROFILE=1 make build-server
SIM_PROFILE=1 SIM_PROFILE_WINDOW_SEC=10 ./build/signal_server
```

The same flag is wired through replay and WASM builds; prefix the relevant
make target with `SIM_PROFILE=1`. Keep the default off for normal builds;
enable it when sampling a save or a replay scenario, then paste the
10-second phase dump here.
