# Zero-latency reconciliation gate

The loopback determinism gate observes the real local networking path:

1. browser input is encoded as a tick-addressed network command;
2. the in-process authoritative server consumes that command in
   `world_sim_step`;
3. the server serializes an authoritative player snapshot;
4. the client decodes the snapshot and enters `apply_remote_player_state`;
5. client prediction is compared with authority before reconciliation changes
   the local pose.

Pose comparison is bit-exact across `x`, `y`, `vx`, `vy`, and `angle`. The gate
does not change correction thresholds, replay behavior, or floating-point
tolerances.

## Classification

Only unequal poses are corrections. Each correction is assigned one class:

- **bootstrap** — the first authoritative local state;
- **input-frontier** — server and prediction ticks differ, an input is
  unacknowledged, the authoritative ACK just advanced, or the tick-addressed
  replay intent differs from the input authority actually applied;
- **semantic-discontinuity** — launch, dock, or another explicit local state
  transition, including the death packet followed by its respawn snapshot;
- **transport-recovery** — prediction is deliberately rebased after recovery
  or excessive tick skew;
- **numeric-drift** — pose bits differ after tick and input frontiers are the
  same and no earlier frontier correction remains unresolved.

An exact authoritative sample clears frontier provenance. This prevents a
deliberately deferred scheduling correction from being relabeled as numeric
drift on its next packet.

Death/respawn is also counted as an authoritative semantic event. If client
prediction already reproduced the respawn pose bit-exactly, it remains an
exact pose sample rather than being mislabeled as a correction.

## Failure artifact

The first numeric drift records:

- server and prediction ticks;
- predicted and authoritative input sequences;
- player entity and first divergent motion field;
- full predicted and authoritative poses;
- exact IEEE-754 bit patterns for the first divergent value;
- the authoritative `signal.authoritative_state.v1` root when the local
  authority is available.

Browser smoke reads the artifact through
`get_net_reconcile_first_drift_json`. The gate also writes a compact
`[net-drift]` line to stderr.

## Bounded scenarios

`tests/browser-smoke.spec.ts` covers:

- launch and dock semantic transitions;
- authoritative death and respawn through boost-turn hull drain;
- thrust, turn, boost, and mining controls in a cleared deterministic flight
  corridor;
- real loopback tractor hook, hold, asteroid/cargo tow coupling, and release;
- changed NPC and asteroid motion snapshots during the flight and towing
  scenarios;
- a forced one-bit `player.ship.pos.x` perturbation at an identical tick and
  input frontier.

The perturbation must produce exactly one numeric-drift sample and a
versioned authoritative root. Normal bounded scenarios must produce zero.

The explicit per-tick local diagnostic mode (`?netcadence=0`) invalidates its
previous private-pose baseline before each snapshot. Default singleplayer
keeps the dedicated server's bounded heartbeat/correction cadence.

## Verification

Run the pure classifier tests:

```sh
./build/signal_test --filter=reconciliation_diagnostics
```

Run the real browser loopback scenarios:

```sh
npx playwright test tests/browser-smoke.spec.ts --project=chromium \
  --grep "E docks|loopback flight corrections|hooks, holds, and releases"
```
