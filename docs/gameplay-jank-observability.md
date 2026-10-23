# Gameplay jank report

Signal now has an opt-in report for late frames. It is off during normal play.
When enabled, it records bounded counters and small histograms. It does not
record player input, identities, cargo contents, or world saves.

The report includes:

- frame and simulation p50, p95, p99, and maximum time;
- input/network, simulation, interpolation, world, UI, submission, and media
  time;
- local authority simulation and loopback packet work;
- completed, missed, and dropped 120 Hz ticks;
- packet count, bytes, maximum size, and maximum cadence gap;
- correction, velocity jump, and correction jerk for asteroids, cargo pods,
  scaffolds, NPCs, and remote players;
- the main phase responsible for each frame over 16.6 ms;
- save snapshot clone time and background write time in server logs.

## Native fresh and mature save

Run:

```sh
make jank-profile-native
```

Play a fresh world for at least 60 seconds. Then load a mature save and play
for another 60 seconds. A JSON line starting with `[gameplay-jank]` is printed
every 60 seconds. Server saves print `[save-observability]` with the snapshot
tick, collection tick, clone time, and background write time.

## Browser fresh and aged saved world

Run two ten-minute samples:

```sh
make jank-profile-browser
```

The first sample measures a fresh local world. The check then advances the
real authority by 600 game seconds in bounded batches, saves it, and reloads
the page. The second sample measures that aged saved world. Ordinary frame
updates drive both samples. The player stays docked while station work, NPCs,
asteroids, loopback packets, and rendering continue. This is a repeatable
active dockside workload. Use the first-outpost route and a human play session
to assess the wider journey and a developed player economy.

The command selects `tests/gameplay-profile.spec.ts` by file. `npm run smoke`
runs the same check with three-second samples and a 60-game-second warm-up.
For a quick check with a fresh build:

```sh
make jank-profile-browser SIGNAL_JANK_PROFILE_SECONDS=3 SIGNAL_JANK_AGE_SECONDS=60
```

Both settings accept whole seconds from 1 to 3,600. Warm-up uses the real
simulation through a smoke-only export. Each call advances at most 120 ticks
while the player is docked. The check resets timing counters after setup and
after reload. It verifies saved world age, ship identity, seed, station count,
active simulation time, fixed steps, loopback traffic, and observed entity
motion. The separate browser smoke test measures the paused fracture render
scene and the deterministic accelerated-asteroid presentation check.

Each run writes and attaches these JSON files under its `test-results` folder:

- `profile-environment.json`: source revision, WASM hash, OS, CPU model, browser, viewport, and durations;
- `fresh-world.json` and `aged-saved-world.json`: full frame and simulation
  reports, phase costs, packet gaps, entity corrections, simulation speed,
  dropped-tick percentage, and the selected budgets;
- `saved-world-checkpoint.json`: the world before aging, after aging, and after reload.

The check writes each report before its assertions so a failed run retains
its measurements. At boot and around reload, it observes the browser save
completion callback. This preserves the completed generation when another
autosave starts.
The raw `snapshots.max_gap_ms` is the largest gap for any observed message
type. Rare event messages contribute to it alongside regular snapshots.
Compare runs on the same named hardware and browser. The frame histogram
measures frame processing time. Its 0.25 ms buckets saturate
at 63.875 ms; use the exact maximum alongside percentiles for long stalls.
Dropped-tick percentage is dropped ticks divided by completed plus dropped
ticks. `missed` remains available separately in the raw report.

Set measured budgets on a reference machine:

```sh
SIGNAL_JANK_MAX_FRAME_P95_MS=16.6 SIGNAL_JANK_MAX_TICK_LOSS_PERCENT=1 \
  make jank-profile-browser
```

These example values are qualification targets. Choose the reference hardware,
record its baseline, and agree on budgets before using them as release gates.
The short CI check verifies active work and evidence; reference-machine runs
apply the chosen frame and tick-loss limits. The full hour of ordinary input
and sustained flight is a separate release qualification. Pair the dockside
measurements with the existing ordinary-input flight check:

```sh
npx --no-install playwright test tests/browser-smoke.spec.ts --project=chromium \
  --grep "loopback flight corrections apply"
make smoke-latency-suite
```

These checks exercise launched flight and delayed network acknowledgements.
For sustained towing, record the manual ten-minute route with `jankprofile=1`
and include its report beside the dockside samples.

For manual browser play, add `jankprofile=1` to the play URL. Call
`signal_jank_profile_report_json` from the browser module to copy the current
report, or reload the page to start a new window.

## Fixed-step overflow

The client completes at most eight 120 Hz steps in one rendered frame. If a
pause or machine stall leaves more work than that, full leftover ticks are
dropped and counted while the fractional remainder is kept. This prevents a
long catch-up spiral. The report makes the loss explicit under
`fixed_step.accumulator_dropped` and `fixed_step.missed`.

## Accelerated asteroid presentation

Tow and station-pull asteroids still receive authoritative motion packets.
Rendering estimates acceleration from consecutive packet velocities and uses
it for at most 0.25 seconds. Invalid or extreme samples are rejected or
clamped. After 0.25 seconds the asteroid coasts, so a lost packet cannot make
an old force estimate run away. This is presentation-only and never changes
the authoritative simulation.
