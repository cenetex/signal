# Signal Remediation Plan

Derived from a full-codebase review on 2026-05-29. Items are ordered by
impact-to-effort ratio, not strictly by severity.

---

## 1. Docs cleanup (in progress)

**Problem:** 40+ files in `docs/` with no organization. Half are design-exploration
SVGs and whitepapers that don't reflect the shipped game. `memory/` has three
stub files that duplicate content from other docs. No human-facing architecture
overview exists.

**Fix:**
- Remove all SVG design mockups, old screenshots, `design/`, and `belt_samples/`
- Remove speculative docs: `p2p-design.md`, `p2p-mesh-gap-analysis.md`,
  `sector-x-whitepaper.md`, `anime-framework.md`
- Collapse `memory/` into root-level `ARCHITECTURE.md`
- Keep only docs that describe shipped systems

---

## 2. `game_sim.c` split (blocked on #285)

**Problem:** 7,236 lines. The spatial hash, signal chain rebuild, dock/launch
logic, commodity transfers, cargo pod management, scaffold delivery, and mining
target finding share a single file. The file itself carries a banner warning
against mechanical splitting because the entity-pool refactor (#285) would
invalidate any split done against the current `MAX_STATIONS=8`-style assumptions.

**Fix:** Once #285 Phase 3 lands (streaming entity pool), extract bounded
subsystems into their own files: signal grid, docking, trading, scaffold
lifecycle, cargo pods.

---

## 3. `sim_ai.c` frontier director extraction

**Problem:** The frontier director — which auto-plans outposts, manages virtual
logistics budgets, and tracks scaffold work — lives inside the NPC AI file
alongside miner/hauler state machines and steering. At 3,083 lines, the file is
hard to navigate, and the frontier subsystem has no dedicated tests.

**Fix:** Extract `sim_frontier.c` from `sim_ai.c`. Give it its own header and
test file. This can happen independently of #285.

---

## 4. `hud.c` decomposition

**Problem:** 3,396 lines mixing layout primitives, relay debug JS interop,
balance helpers, hail/scan display, contract-fit rendering, inspect anim, and
onboarding overlay. Not structurally blocking, but UI iteration is slower than
it should be.

**Fix:** Pull hail/scan panel, station UI chrome, and onboarding overlay into
separate client files.

---

## 5. Test suite balance

**Problem:** `test_world_sim.c` (3,627 lines) and `test_construction.c` (2,855
lines) are 26% of all test code. These are broad integration tests — valuable
but slow and hard to debug. Subsystems like the frontier director, scaffold
state transitions, signal grid rebuild, and trade paging have no unit-level
coverage.

**Fix:** Add focused unit tests for:
- Frontier director planning and virtual logistics
- Scaffold state machine transitions
- Signal grid rebuild correctness
- Trade paging pagination and edge cases

---

## 6. Fuzzing harnesses

**Problem:** The game parses binary wire protocol packets and save files in C
with no fuzz coverage. A single malformed `NET_MSG_WORLD_ASTEROIDS` or corrupt
`world.sav` could crash the server.

**Fix:** Add libFuzzer harnesses targeting:
- `net.c` deserialization of world snapshots
- `sim_save.c` world-load and player-save paths
- `chain_log.c` log parsing

Wire these into CI as a periodic job.

---

## 7. Memory allocator strategy

**Problem:** 570 `malloc`/`free` calls across production code with no arena or
pool allocator. Every scaffold, cargo pod, manifest row, and spatial grid bucket
hits the general allocator at 120 Hz. On the server this adds GC pressure; on
the client it adds frame-time jitter.

**Fix:** Introduce a per-frame bump allocator or fixed-size object pools for
hot-path allocations (spatial grid cells, manifest rows, sim events). The entity
pool caps make this a good fit.

---

## 8. CI breadth

**Problem:** Two workflow files (release, Valgrind) but no Windows
build, no Emscripten build check, no clang-tidy, no sanitizer CI run. The
Makefile has `test-san` and `test-tsan` targets that nothing calls automatically.

**Fix:** Add CI jobs for:
- Windows native build
- Emscripten build
- ASan+UBSan test run
- clang-tidy on PRs

---

## 9. A* nav graph ceiling

**Problem:** The A* pathfinding graph caps at 96 nodes (`NAV_MAX_NODES`) with
no fallback. As outposts multiply, pathfinding silently breaks when the graph
overflows.

**Fix:** Add a fallback (direct-line navigation) when graph insertion fails, or
bump the cap with a logged warning. The cap can stay for now but the failure
mode should be visible.

---

## 10. Docs that stay

| File | Why |
|------|-----|
| `cargo-architecture.md` | Authoritative three-state cargo model |
| `c_safety_policy.md` | Actively enforced C safety rules |
| `protocol-telemetry.md` | Wire protocol stream reference |
| `replay-harness.md` | Shipped tool documentation |
| `operator-onboarding.md` | Station operator guide |
| `anime-integration-plan.md` | Shipped episode playback architecture |
| `decentralization.md` | Federation architecture reference |
| `decentralization-synthesis.md` | Bridge between federation and P2P designs |
| `cloudwatch-dashboard-signal-relay.json` | Production config |
# Signal Remediation Plan

Derived from a full-codebase review on 2026-05-29 and re-scoped on 2026-05-30
after the dedicated server was removed to force the P2P/browser-peer
architecture. Items are ordered by impact-to-effort ratio, not strictly by
severity.

---

## 0. Fixed-point sim rewrite (priority:now — #588)

**Problem:** ~567 `float` occurrences across 10 sim files produce subtly
different results between native and WASM builds. Without cross-platform
determinism, browser peers running `world_sim_step` diverge, and quorum
signatures on chain-log events are meaningless. This is the single blocker for
the entire P2P stack.

**Fix:** Replace all `float` in sim state structs and step functions with q32.32
fixed-point (32-bit integer, 32-bit fractional in `int64_t`). Provide
add/sub/mul/div/sqrt/sin/cos/atan2/exp in integer arithmetic. Render path stays
float. All 340+ tests must pass identically on native and WASM.

---

## 1. `game_sim.c` split (blocked on #285)

**Problem:** 7,236 lines. The spatial hash, signal chain rebuild, dock/launch
logic, commodity transfers, cargo pod management, scaffold delivery, and mining
target finding share a single file. The file itself carries a banner warning
against mechanical splitting because the entity-pool refactor (#285) would
invalidate any split done against the current `MAX_STATIONS=8`-style assumptions.

**Fix:** Once #285 Phase 3 lands (streaming entity pool), extract bounded
subsystems into their own files: signal grid, docking, trading, scaffold
lifecycle, cargo pods.

---

## 2. `sim_ai.c` frontier director extraction

**Problem:** The frontier director — which auto-plans outposts, manages virtual
logistics budgets, and tracks scaffold work — lives inside the NPC AI file
alongside miner/hauler state machines and steering. At 3,083 lines, the file is
hard to navigate, and the frontier subsystem has no dedicated tests.

**Fix:** Extract `sim_frontier.c` from `sim_ai.c`. Give it its own header and
test file. This can happen independently of #285.

---

## 3. `hud.c` decomposition

**Problem:** 3,396 lines mixing layout primitives, relay debug JS interop,
balance helpers, hail/scan display, contract-fit rendering, inspect anim, and
onboarding overlay. Not structurally blocking, but UI iteration is slower than
it should be.

**Fix:** Pull hail/scan panel, station UI chrome, and onboarding overlay into
separate client files.

---

## 4. Test suite balance

**Problem:** `test_world_sim.c` (3,627 lines) and `test_construction.c` (2,855
lines) are 26% of all test code. These are broad integration tests — valuable
but slow and hard to debug. Subsystems like the frontier director, scaffold
state transitions, signal grid rebuild, and trade paging have no unit-level
coverage.

**Fix:** Add focused unit tests for:
- Frontier director planning and virtual logistics
- Scaffold state machine transitions
- Signal grid rebuild correctness
- Trade paging pagination and edge cases

---

## 5. Fuzzing harnesses

**Problem:** The game parses binary wire protocol packets and save files in C
with no fuzz coverage. A single malformed `NET_MSG_WORLD_ASTEROIDS` or corrupt
`world.sav` could crash the server.

**Fix:** Add libFuzzer harnesses targeting:
- `net.c` deserialization of world snapshots
- `sim_save.c` world-load and player-save paths
- `chain_log.c` log parsing

Wire these into CI as a periodic job.

---

## 6. Memory allocator strategy

**Problem:** 570 `malloc`/`free` calls across production code with no arena or
pool allocator. Every scaffold, cargo pod, manifest row, and spatial grid bucket
hits the general allocator at 120 Hz.

**Fix:** Introduce a per-frame bump allocator or fixed-size object pools for
hot-path allocations (spatial grid cells, manifest rows, sim events). The entity
pool caps make this a good fit.

---

## 7. CI breadth

**Problem:** Two workflow files (release, Valgrind) but no Windows build, no
Emscripten build check, no clang-tidy, no sanitizer CI run. The Makefile has
`test-san` and `test-tsan` targets that nothing calls automatically.

**Fix:** Add CI jobs for Windows native build, Emscripten build, ASan+UBSan test
run, and clang-tidy on PRs.

---

## 8. A* nav graph ceiling

**Problem:** The A* pathfinding graph caps at 96 nodes (`NAV_MAX_NODES`) with
no fallback. As outposts multiply, pathfinding silently breaks when the graph
overflows.

**Fix:** Add a fallback (direct-line navigation) when graph insertion fails, or
bump the cap with a logged warning.

---

## P2P architecture docs (retained)

The following docs were previously flagged for removal but are now the target
architecture and stay:

| File | Why |
|------|-----|
| `docs/p2p-design.md` | Target architecture for browser-peer mesh |
| `docs/p2p-mesh-gap-analysis.md` | Gap analysis for the P2P transition |
| `docs/decentralization-synthesis.md` | Bridge between federation and P2P designs |

## Docs that stay (unchanged)

| File | Why |
|------|-----|
| `cargo-architecture.md` | Authoritative three-state cargo model |
| `c_safety_policy.md` | Actively enforced C safety rules |
| `protocol-telemetry.md` | Wire protocol stream reference |
| `replay-harness.md` | Shipped tool documentation |
| `operator-onboarding.md` | Station operator guide (federation fallback) |
| `anime-integration-plan.md` | Shipped episode playback architecture |
| `decentralization.md` | Federation architecture reference |

## Docs to remove

- All SVG design mockups, old screenshots, `design/`, `belt_samples/`
- `sector-x-whitepaper.md` (post-MVP vision, not current target)
- `anime-framework.md` (milestone-video scope doc, superseded by anime-integration-plan)
- `cloudwatch-dashboard-signal-relay.json` (AWS CloudWatch, infra removed)
- `memory/` directory (stub files that duplicate content from other docs)

