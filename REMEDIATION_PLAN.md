# Signal Playable Core Remediation Plan

Re-groomed on 2026-08-08 around one outcome:

> A player can spend one hour in Signal with smooth rock motion, trustworthy
> economics, a complete progression path, and enough physical danger and
> feedback to produce a story.

The live milestone is GitHub issue #690. This plan supersedes the earlier
substrate-first ordering. Signal still turns physical play into verifiable
history, but the history is valuable only when the underlying game action is
complete, legible, and fun.

## Working rule

`priority:now` is an executable queue. Measure before broad optimization. Take
narrow architecture slices only when they complete a player-visible outcome;
do not substitute whole-model cleanup for fixing the observed game.

## Now

### 1. Economy integrity (#686)

Reproduce the reported station payment loop and establish one invariant across
every payout path: one physical/logical source identity can fund a given
station action at most once.

The gate covers repeated input, failed custody or handoff, duplicated packets,
reconnect, save/load, and crash recovery. Cargo consumption/custody, contract
progress, durable evidence, ledger credit, and player statistics must stage
before one commit boundary.

### 2. Asteroid motion (#685)

Remove the visible 10 Hz correction rhythm in local mode. Default loopback
world snapshots arrive at 10 Hz while the client predicts loose rocks with
ambient drag and towed rocks with constant velocity. That does not model player
or station tractor acceleration, collision response, fracture impulses, or
multi-body contact.

Compare default cadence with the per-tick diagnostic mode, measure correction
distance/velocity discontinuity/screen-space jerk, and preserve the same
authoritative simulation used by multiplayer.

### 3. Gameplay observability (#687)

Record CPU frame phases, simulation phases, loopback encode/decode,
interpolation, world/UI rendering, submission, fixed-step count, snapshot
cadence, persistence markers, entity correction, and accumulator loss. Produce
p50/p95/p99/max reports for native/browser, fresh-world/mature-save scenarios.

No fixed-step debt may be silently discarded.

### 4. Atomic towing (#617)

Make attach, tow, release, station tractor fields, and remote-body presentation
reliable under loopback, latency, jitter, loss, duplication, reordering,
relevance transitions, reconnect, and save/load.

Only the narrow authority-boundary work required for this outcome is active;
the broad #308 cleanup remains parked.

### 5. Reachable construction (#674)

Give legitimately purchased or towed frame pods a receipt-preserving normal
player handoff that can satisfy an outpost core. Unknown or tampered cargo must
remain fail-closed, and the end-to-end path must not need synthetic receipts.

## Next

### 6. Physics feel (#684)

After #687 separates performance hitching from physics response, review ship
thrust/braking/rotation, tractor stiffness/damping/tension, collision energy,
throw/release, multi-body settling, camera behavior, and controller response.

### 7. Persistence hitch and recovery (#666)

Publish crash-consistent generations from a bounded writer. Durable I/O must
not stall the 120 Hz simulation loop, and recovery must not duplicate or lose
cargo, credits, ships, manifests, or chain heads.

### 8. Interaction clarity (#619)

Prioritize the immediate target, tow state/tension, danger, docking, payout,
and rejection reason. Move audit, hash, build, and network detail behind
deliberate inspection surfaces.

### 9. First-hour progression (#688)

Gate the normal fresh-player path:

```text
launch -> fracture -> tow -> smelt -> trade -> upgrade -> frames -> outpost
```

Record milestone times, dead ends, unexplained waits, and maximum no-progress
intervals. The scenario must survive save/load and reconnect without injected
state.

### 10. Rock combat and rewards (#689)

Make thrown-rock aim, tension, near misses, impacts, damage, ownership credit,
recovery, contracts, and upgrades readable and strategically meaningful. No
conventional weapons are introduced; every escalation remains physical rock
play.

## Milestone acceptance

- One source/action cannot create duplicate station credit.
- Local towed, colliding, fractured, and station-pulled rocks are visually
  continuous at render cadence.
- Ten-minute native and browser runs expose frame percentiles and report no
  silent accumulator loss.
- Attach/release is atomic and presentation remains smooth under adverse
  network conditions.
- A fresh player reaches the first outpost through normal actions.
- Save/load preserves cargo and credits without replaying an economic action.
- Immediate action and danger are legible at a glance.
- Rock handling, conflict, and upgrades create meaningful decisions.

## Accepted foundations

- #588 is closed: certified Linux x64, macOS ARM, and WASM authorities use the
  strict IEEE, deterministic-transcendental, versioned state-root, replay-gate
  profile. Fixed point is evidence-triggered, not a default rewrite.
- #339 and #340 are closed: manifest-backed finished-good authority and the
  transition cleanup are complete.

## Gated backlog

These remain valuable but do not displace Playable Core:

- player-facing lineage and #587 typed provenance contracts, except where a
  narrow slice explains an active trade/construction/combat decision;
- #354/#355/#356 settlement-event expansion;
- institution tools, escrow, public manifests, and route dashboards;
- #294 controller convergence and broad #308 authority cleanup;
- #590/#591/#589 permaweb, peer anchoring, and WebRTC quorum behavior;
- #496 RATi vessel identity and external-chain adapters;
- #343/#603 hull/station block convergence;
- #285 entity-cap lifting and scale-only render optimization such as #667;
- broader fuzzing, extraction, allocator, and maintenance work not tied to an
  observed Playable Core failure.
