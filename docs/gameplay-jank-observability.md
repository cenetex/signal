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

## Browser fresh and mature scenario

Run:

```sh
make jank-profile-browser
```

The browser gate runs the same packet-driven single-player client in a fresh
world and a dense mature-world fixture for 60 seconds each. It checks that
the report has frame percentiles, phase attribution, packet cadence, fixed
steps, all entity classes, and the deterministic accelerated-asteroid gate.

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
