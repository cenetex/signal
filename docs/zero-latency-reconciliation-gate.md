# Zero-latency reconciliation gate

The gate observes the real local networking boundary without using browser
frame timing:

1. a client input is encoded with a deterministic target tick;
2. client prediction advances exactly one `SIM_DT`;
3. the in-process authority consumes the queued input in `world_sim_step`;
4. the authority serializes its ordinary player/world snapshots;
5. the client decodes the packets and compares prediction with authority
   before reconciliation mutates the predicted pose.

The bounded fixture covers thrust, turns, boost, mining, asteroid and NPC
motion packets, a held tow, release, dock, launch, and death/respawn. Every
step is scheduled by integer tick and input sequence; wall time, keyboard
holds, network timers, and Playwright sleeps are not part of the scenario.

## Classification

Bit-exact `x`, `y`, `vx`, `vy`, and `angle` comparisons are classified as:

- `bootstrap`;
- `input-frontier` for an unknown/different tick or input sequence, an
  unacknowledged command, a replay gap, or a different applied movement
  intent;
- `semantic-discontinuity` only inside the explicit tick of an observed
  launch, dock, tow attach/release, predicted action, or the bounded
  death/respawn event window;
- `transport-recovery` only on the explicit rebase tick;
- `numeric-drift` when pose bits differ at the same known tick and input
  frontier without one of the bounded causes above.

Each causal window has a closed first/last tick. There is no persistent
frontier-taint flag: a scheduling mismatch at one tick cannot mask a later
numeric divergence. Likewise, being docked is not itself semantic; only the
observed dock transition tick is.

The normal fixture must record zero numeric drift. The fault probe predicts
one matching tick, flips the least-significant bit of client
`player.ship.pos.x`, and then takes the ordinary authoritative snapshot. It
must produce exactly one numeric-drift sample.

## Failure artifact

The first numeric divergence retains:

- authoritative and prediction ticks;
- predicted and authoritative input sequences;
- input, semantic, and transport-recovery cause masks;
- entity and first divergent field;
- exact IEEE-754 bits for the full predicted and authoritative poses;
- the canonical `signal.authoritative_state.v4` root.

`signal_zero_latency_gate_report_json()` exposes the deterministic scenario
coverage and embeds the first-drift artifact. The first failure is also
written as one compact `[net-drift]` line.

## Focused checks

```sh
./build/signal_test --filter=reconciliation_diagnostics
npx playwright test tests/browser-smoke.spec.ts --project=chromium \
  --grep "fixed-tick loopback gate"
```

The gate deliberately keeps the current reconciliation thresholds. It does
not migrate a numeric domain or claim cross-runner soak coverage; the
versioned state-root replay gate remains the cross-runner prerequisite.

## Adverse towable presentation slice

The browser smoke also runs one deliberately bounded #617 scenario through
the real cargo-pod roster/interpolation and atomic tow-snapshot apply paths:
one live player ship and one cargo-pod slot. A pure diagnostic scheduler
replays the same payload timeline at 50, 125, and 250 ms base latency with
fixed bounded jitter and reordering, every-seventh-packet loss, and
every-fifth-packet duplication. Loss means that one complete replacement
tow snapshot instance is absent; every later tow snapshot is independently
complete and can recover the relation. This scheduler is test-only and is
not production transport code.

The authored 60 Hz pass thresholds are constants fixed before a run:

- correction distance: at most 24 world units;
- authoritative velocity discontinuity: at most 18 world units/second;
- longest visible snapshot gap: at most 0.18 seconds (starvation observation
  begins after 0.12 seconds);
- presentation jerk: at most 250,000 world units/second cubed;
- screen-space jerk: at most 500,000 pixels/second cubed at the fixture's
  fixed 2 pixels/world-unit scale.

The timeline covers attach, release, a 50 ms reattach, target retirement,
a replacement generation only after an accepted empty revision, attach of
that replacement, relevance exit/re-entry while every repeated tow snapshot
is withheld until final release, and final release. The re-entry
regression therefore verifies that a known accepted relation is re-projected
by the inactive-to-active roster transition rather than by a later duplicate
tow packet. Metrics read a copied presentation pose; they
never feed smoothing state into authority. Every delivery and render step
also rejects a target or source compatibility projection that exists without
the accepted current relation and a relevant target.

This is not the full #617 matrix. It does not cover player/NPC/station-module
sources crossed with fragments, terrain asteroids, scaffolds, and cargo pods;
OS/WebSocket traffic shaping; save/reconnect; or every loss-order
combination. Non-ship roster streams and the station-module roster do not
currently expose generation identity. Consequently, same-slot replacement
before a newer tow revision is an explicit open gap: this gate only tests
replacement after the empty revision and does not claim stale-generation
protection for station sources or non-ship targets.

Canonical tow serialization currently collects and insertion-sorts links for
each recipient snapshot. A follow-up should cache the canonical order once
per tow revision instead of paying worst-case
`O(players × links²)` work each world tick.

Focused checks:

```sh
./build/signal_test --filter=tow_presentation
npx playwright test tests/browser-smoke.spec.ts --project=chromium \
  --grep "deterministic adverse delivery"
```
