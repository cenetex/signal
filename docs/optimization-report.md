# Optimization Report — Server Sim & Client Hot Paths

*Updated 2026-07-04 with a measured CPU profile — see "Measured profile"
below. The measurement reorders the static rankings: fixed-point math and
`maintain_asteroid_field` dominate the real workload; the NPC/station scan
findings are real but secondary at current entity counts.*

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

### M1. `v2_len` routes through software 128-bit division — **measured P0**

`shared/math_util.h:66` defines `v2_len` as `fixp_sqrtf(v2_len_sq(...))`.
`fixp_sqrtf` (`shared/fixpoint.c:109`) converts float → q32.32, takes an
integer-sqrt initial guess, then runs **up to 8 Newton iterations, each
performing a `fixp_div` = 128÷64-bit division** (`__udivmodti4` in
compiler-rt — software, even on ARM64; far worse in WASM where it's emulated
i64 math). Every vector length in the entire sim pays this. The single
hottest leaf in the profile.

Options, in order of preference:

1. **Use hardware `sqrtf`.** IEEE 754 *requires* correctly-rounded square
   root, so `sqrtf` is bit-exact across x86/ARM/WASM — unlike `sinf`/
   `cosf`/`atan2f`, which genuinely need the fixp versions. If the
   native↔WASM replay gates pass with `sqrtf` in `v2_len`, this is a
   one-line change that deletes the entire cost. (Emscripten compiles
   `sqrtf` to the `f32.sqrt` WASM instruction, also correctly rounded.)
   Verify with `make replay-native-wasm` and the long-horizon probes
   before accepting.
2. If (1) is rejected for determinism-policy reasons: rewrite
   `fixp_sqrt_fixp` as a pure integer sqrt over the full 96-bit value
   (shift-and-subtract, no division), and/or cut Newton iterations — the
   integer initial guess is already accurate enough that 8 iterations with
   a 128-bit divide each is heavily overprovisioned.
3. Independently: the gravity pair loop already has `dist_sq`; hoisting a
   single `1/dist` and strength-reducing removes redundant work but does
   not fix the underlying primitive.

### M2. `chunk_materialized` linear scan — **measured P1**

`sim_asteroid.c:990` scans every asteroid slot to answer "is chunk (cx,cy)
materialized," and `maintain_asteroid_field` asks it for every candidate
chunk around every viewport — including all ~100 NPC positions. Fix: build
a small hash set of materialized `(cx,cy)` once per call (one pass over
asteroids), then O(1) lookups; or maintain a persistent per-chunk count
updated in `materialize_asteroid`/`clear_asteroid_slot`.

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

- **Asteroid spatial grid** (`game_sim.c:506`, `game_sim.h:99`): 800-unit
  cells, used by asteroid–asteroid gravity and collision.
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
- **Spatial cell overflow is silent** (`game_sim.c:501`,
  `SPATIAL_MAX_PER_CELL = 16`): in a dense field (cells are 800×800), the
  17th+ asteroid in a cell is dropped from *all* neighbor queries — it
  stops attracting, colliding, and being collidable. That is a correctness
  cliff as much as a perf note; consider bumping the cap or logging
  overflow in debug builds.
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
| 0a | `sqrtf` in `v2_len` (gated on replay-gate pass), or division-free `fixp_sqrt` | small | Deletes the hottest measured leaf (~15%+ of sim; far more in WASM) |
| 0b | Hash set / per-chunk count for `chunk_materialized` | small | Removes ~13% of measured sim time |
| 1 | Compact active-station array, used by asteroid dynamics, vortex, industrial pull, despawn, cargo pod helpers | small | Removes the dominant `asteroids × 128-slot` traffic |
| 2 | Off-relay beacon list + early-out in `signal_strength_at` | small | Restores the O(1) promise of the signal cache |
| 3 | Broad-phase distance guard before every `station_build_geom` in collision paths | small | Cuts geometry builds to near-zero when far from stations |
| 4 | Rebuild spatial grid per tick; use it for NPC + player asteroid collision | medium | ~25M slot checks/sec → thousands |
| 5 | Per-tick shared `station_geom_t` cache (heap) | medium | One geom build per station per tick, all consumers |
| 6 | Manifest count cache or 2 Hz gate on reconciliation | small | Deletes a constant per-tick scan |
| 7 | Async autosave writer thread | medium | Removes 30 s hitch |

Steps 1–3 are independent, low-risk, and probably recover the bulk of the
headroom. Steps 4–5 touch collision behavior and deserve the replay soak
gate (`make test` + no-omniscience replay) before shipping.

## Measure before/after

There is no per-phase timing in `world_sim_step` today. Before starting,
add a cheap instrumentation pass — accumulate per-phase `mach_absolute_time`
/ `clock_gettime` deltas across the ~15 step functions in
`world_sim_step` (`game_sim.c:11230`) and dump a histogram every N seconds
under a `SIM_PROFILE` env var or build flag. That turns this report's
worst-case arithmetic into real numbers, catches regressions, and will
rank steps 1–5 by actual (not estimated) payoff on the live workload. The
`perf/` directory is the natural home for captured runs.
