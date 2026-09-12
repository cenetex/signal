# Connectome brain (`SERVER_BRAIN_MODE_CONNECTOME`)

A fourth NPC brain mode that flies workers with a *Drosophila* connectome
subgraph instead of a hand-written controller. A deterministic integer
leaky-integrate-and-fire kernel runs a FlyWire circuit per NPC; the wiring is
the program, there is no training, and brain-time is rationed by economic
stake.

- `server/connectome/flybrain.[ch]` — the kernel. Vendored from the upstream
  flybrain project; re-sync there before changing it.
- `server/connectome/flyswarm.[ch]` — budget allocation, stake, deep slots.
- `server/signal_connectome_brain.[ch]` — the signal adapter.

## Enabling it

The mode is off unless `SIGNAL_CONNECTOME_FAST` names a compiled `.cnx`
circuit. With no env set, NPCs fall back to the neural/heuristic brains and
nothing about the sim changes.

```sh
SIGNAL_CONNECTOME_FAST=/path/to/nav.cnx ./build/signal_server
SIGNAL_CONNECTOME_FAST=/path/to/nav.cnx ./build/signal        # singleplayer
```

In the Fly deployment, `fly.toml` sets `SIGNAL_CONNECTOME_FAST=/app/nav.cnx`
and `SIGNAL_CONNECTOME_BUDGET=30`; `server/Dockerfile` copies the committed
blob to that path. The machine only runs while players are connected
(`min_machines_running = 0`), so the flies work during live sessions only.

Both `server/main.c` and `client/local_server.c` call `signal_connectome_init()`
before the world loads, so NPC spawn stamps the mode from the first tick.

### Circuit blobs

`nav.cnx` (~871 KB, 4,564 neurons) is committed at
`assets/connectome/nav.cnx` so the Fly image can enable the brain without a
build-time network fetch; `server/Dockerfile` copies it to `/app/nav.cnx`.
`full.cnx` (~24 MB, 138,584 neurons) stays external — it is only needed for the
optional `SIGNAL_CONNECTOME_DEEP` deliberation slots. Both are compiled from the
multi-GB FlyWire export by the upstream flybrain project's `tools/`. The
`*.cnx` ignore rule still excludes every other circuit; the committed file is
explicitly re-included.

A blob must carry the `ring` and `columnar` populations (injection sites) and
`dn_left` / `dn_right` (the descending readout). `escape_*`, `escape2_*` and
`brake_*` are used by the fear path when present.

## Operating point, and why it matters

**The single most important knob is `SIGNAL_CONNECTOME_TONIC_UV`, and getting
it wrong produces a silent brain rather than an error.**

A neuron driven with a constant current settles at `uv / (1 - exp(-dt/tau))`
with `tau` = 20 ms. At the default `dt = 4000 µs` that is `uv / 0.1813`, so
the tonic must exceed about **1270 µV** just to reach the 7000 µV threshold.
Ignition is a cliff, not a ramp — measured on `nav.cnx`, the circuit is
completely silent below ~1600 µV and sits at ~28–31 Hz from 2200 µV up, with
no graded region in between.

If the tonic is too low, every NPC reads `drive = 0`, fails the arousal gate in
`signal_connectome_flight_cmd`, and has thrust clamped to zero forever. They
look alive and never move. Init prints a `[WARN]` when the measured steering
half-span is zero, which is the symptom of exactly this.

Defaults (`dt = 4000`, tonic 2200, steer 2500, noise 0) are characterised for
`nav.cnx`. **Changing `SIGNAL_CONNECTOME_DT_US` invalidates the tonic**, since
both sides of that ratio move.

## What the connectome actually delivers

Measured on the real `nav.cnx`, not assumed:

| Channel | Status |
|---|---|
| Arousal / thrust gate (`drive`) | **Works.** Stable ~28–31 Hz descending bus. |
| Fear → freeze (`brake` + `escape`) | **Works.** Monotone: brake 0→125 Hz, escape 10→127 Hz under injection, and `forward` collapses to 0 at high fear. |
| Steering axis (`turn`) | **Partial.** Reliable *sign* only. |

The steering caveat is the important one. Suppress-left, symmetric and
suppress-right come out deterministically ordered and separated by about 0.29,
so the axis carries a real directional signal at full drive. Every
*intermediate* steering level is chaotic: removing membrane noise does not fix
it, and neither does averaging over 2000 steps — the response is simply not a
function of the input. This is the extraction artifact the upstream README
reports as "flat, non-monotone" for this subgraph (cutting the nav subgraph out
deletes 58% of incoming synapses, keeping the balance but losing the float), and
it is a property of the connectome, not of this adapter.

So the brain **biases** the reflex controller rather than replacing it:

```
turn = reflex_turn + smoothed_brain_turn * forward_clearance * turn_gain
```

`SIGNAL_CONNECTOME_TURN_GAIN_X100` (default 35) is the ceiling. Clearance still
scales it, so at a rock face the giant-fibre reflex gets the wheel to itself.
A frightened or lovestruck fly genuinely pushes the rudder differently without a
chaotic axis being allowed to fly a loaded hauler into a rock.

Fixing this properly is upstream work: per the flybrain README, column-resolved
EB wedge ordering (`EPG_R1..R8`) is what turns a left/right split into a real
heading integrator.

## Drives

Per NPC, recomputed each tick from world state, all Q16:

- **HUNGER** — empty-handed and undocked; rising tonic arousal.
- **LUST** — towing ore. The cargo is the fly's beloved: tonic gain rises and
  the awake threshold drops until delivery.
- **FEAR** — nearby closing rocks (signal's only weapon; deliberately thrown
  rocks get a wider radius than drift) plus low-hull dread.
- **PAIN** — hull loss since last tick, spikes then decays over ~1 s.

## Combined brain: stations plan, connectomes fly

`SIGNAL_CONNECTOME_STRATEGY=1` makes the **station** the strategist. Each
station samples one *posture* for its own swarm -- forage, prospect, caution,
haul, or regroup -- and broadcasts it. Every connectome fly applies the
posture of its `home_station`, so the flies at one station act as a hive and
different stations diverge.

The posture is delivered through the existing signal field: the modulation is
scaled by `signal_strength_at()` at the fly's position. Near the station the
leash is taut; out past the relay chain it goes slack and the fly falls back
to its own connectome drives. A station holds a posture for
`SIGNAL_CONNECTOME_STRATEGY_PERIOD` ticks (default 300) and re-samples it
with a seeded xorshift keyed on station index and world tick, so the choice
varies across stations while staying bit-exact under replay.

The connectome remains the body: postures bias hunger/lust/fear, which drive
the connectome's arousal gate and steering, but the wiring still flies the
ship, and the giant-fibre reflex still owns the wheel at a rock face.

Connectome flies also become eligible for the strategic worker planner's job
re-assignment (`npc_can_reassign`), so a station can re-task a jobless fly.
That planner uses the `signal-npc-worker-v2` model when a checkpoint is
loaded (`SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT`) and teacher scores
otherwise.

Off by default. With the flag unset, or the adapter not loaded, the sim is
bit-identical to before.

## Brain-time as an economy

One shared read-only connectome, N agent states. Each tick a fixed integer
budget of kernel steps is divided by stake:

- A docked fly has stake 0, sleeps, and its share is rented to working flies.
- Stake rises with ore in tow, cargo, scaffolds, contracts in flight, and
  urgency (lust/fear).
- Flies clearing `SIGNAL_CONNECTOME_PROMOTE_STAKE` compete for a few scarce
  deep-circuit slots (`full.cnx`, ~25× the cost per step). Scarcity is what
  makes a hierarchy emerge rather than be assigned.

The adapter drives the stepping loop itself rather than calling
`fb_swarm_tick`, because **sensory drive must be re-injected for every kernel
step**, not once per sim tick. Injection adds current for a single step; a fly
that earned twenty steps with drive on only the first spends nineteen decaying,
and the circuit is silent before the tick ends. flyswarm still owns allocation
policy — only the stepping moved.

Deep circuits allocate per-agent state for all `MAX_NPC_SHIPS` slots in both
circuits, so enabling `SIGNAL_CONNECTOME_DEEP` with `full.cnx` costs ~136 MB
resident for the handful of slots that can ever use it.

## Determinism

No floating point in the kernel step path: membrane voltages are int32
microvolts, the leak is a Q16 multiply, and `exp()` is an integer series. Stakes
come from replayed world state and allocation is integer arithmetic, so two
machines produce identical simulations — a slow machine falls behind in wall
time rather than diverging. `signal_connectome_checksum()` is the gate.

## Verifying it

Unit tests (`tests/c/test_connectome_brain.c`) cover the kernel and the swarm
economics on a synthetic circuit. They deliberately never enable the adapter:
it is process-global, so switching it on inside the test binary would flip every
other sim test's NPCs into connectome mode.

End-to-end coverage is `tools/connectome_swarm_probe.c`, which runs the real
`world_sim_step` loop headless and reports what the swarm did:

```sh
./build/connectome_swarm_probe 3600                                  # baseline
SIGNAL_CONNECTOME_FAST=/path/to/nav.cnx ./build/connectome_swarm_probe 3600
```

A healthy swarm moves, changes state, and tows rock; a dead brain shows NPCs
pinned in one state with zero distance flown. `SIGNAL_CONNECTOME_DEBUG=<n>`
dumps agent 0's drive, turn and thrust every *n* ticks.

## Environment reference

| Variable | Default | Meaning |
|---|---|---|
| `SIGNAL_CONNECTOME_FAST` | — | nav `.cnx` path; **required to enable** |
| `SIGNAL_CONNECTOME_STRATEGY` | 0 | station-level postures broadcast to their flies (hybrid brain) |
| `SIGNAL_CONNECTOME_STRATEGY_PERIOD` | 300 | ticks a station holds a posture before re-sampling |
| `SIGNAL_CONNECTOME_DEEP` | — | full-brain `.cnx` for deliberation slots |
| `SIGNAL_CONNECTOME_DT_US` | 4000 | kernel step; changing this invalidates the tonic |
| `SIGNAL_CONNECTOME_TONIC_UV` | 2200 | columnar arousal; must clear the ignition cliff |
| `SIGNAL_CONNECTOME_STEER_UV` | 2500 | ring steering amplitude |
| `SIGNAL_CONNECTOME_NOISE_UV` | 0 | per-step membrane jitter |
| `SIGNAL_CONNECTOME_TURN_GAIN_X100` | 35 | ceiling on the brain's steering bias |
| `SIGNAL_CONNECTOME_BUDGET` | 120 | kernel-step units per tick (deep step = 25) |
| `SIGNAL_CONNECTOME_DEEP_SLOTS` | 3 | scarce deliberation slots |
| `SIGNAL_CONNECTOME_PROMOTE_STAKE` | 400 | stake to win a deep slot |
| `SIGNAL_CONNECTOME_DEMOTE_STAKE` | 200 | stake to lose one |
| `SIGNAL_CONNECTOME_AWAKE_HZ_X100` | 10 | descending-drive gate (0.10 Hz) |
| `SIGNAL_CONNECTOME_SEED` | 42 | per-agent RNG seed |
| `SIGNAL_CONNECTOME_DEBUG` | 0 | ticks between diagnostic dumps |
