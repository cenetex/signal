# Station strategy comparison

This study compares the complete `SIGNAL_CONNECTOME_STRATEGY` switch: station
postures, reward learning, and worker job reassignment. Both settings use the
same connectome, its fixed seed of 42, and the drive-history fix in PR #736.

## Neural coverage audit

The five-minute and thirty-minute studies used the fly navigation circuit plus
station strategy. Their outcome tables describe that combination.

| Component | Earlier study coverage |
|---|---|
| Fly navigation connectome | Active: 4,564 neurons from the committed nav.cnx |
| Full fly connectome | Optional deep circuit unset; zero deep promotions |
| Station posture learning | Active in the strategy-on episodes |
| Holographic market memory | Shared simulation job-memory and gossip paths use the built-in HNN backend |
| Trained worker policy | Checkpoint absent; existing job rules selected work |
| Built-in CRLPLRIMES flight policy | Available in the binary; connectome NPCs take the separate fly steering path |
| Trained contract policy | Checkpoint absent |
| Holographic pilot | Separate NPC pilot mode; the study workers use connectome mode |
| leCore HNN backend | Build uses builtin-radix2; leCore option is off |

The optional full fly circuit is documented as 138,584 neurons. The current local
server configuration at commit 37764277c1b5fdf27b44cfd734f0889e3715a35a also uses
nav.cnx, and adds an active trained worker checkpoint. This is a source/configuration
check; deployment state requires its own check.

The probe now mirrors the server's worker checkpoint loading and holographic pilot
initialization. Each result reports circuit neuron counts, kernel steps, HNN backend,
checkpoint load state, and flight, contract, and worker inference counts. The runner
accepts --worker-checkpoint, sets active mode, records the checkpoint hash, and checks
that the requested model loaded. A missing requested checkpoint ends the run before
simulation.

Worker activation has a further scope limit in npc_worker_score_assignment:
the model ranks mining, hauling, and other options, while the active dispatch handles
self-refit, escort, and patrol choices. The existing job rules handle the remaining
choices. An inference count measures scoring; it does not establish that a selected
action changed the world. Also, connectome worker reassignment requires strategy-on,
so a strategy-off episode can load the model and still record zero worker decisions.

The fly steering path retains shared route planning, speed control, and collision
avoidance. Several pilot types are alternatives selected by NPC brain mode. Testing
the whole suite therefore needs separate mode coverage and action coverage.

### Thirty-minute worker load check

Seed 2037 ran for 216,000 ticks with the worker checkpoint loaded in active mode,
strategy off/on, and a full repeat of each. All earlier outcome fields matched
the original thirty-minute run exactly: 88/139 smelt units and 0/3 ship losses.
Both settings reported 4,564 fast neurons, zero deep neurons, 6,469,230 connectome
steps, and zero worker decisions or inferences. Flight and contract inferences
were also zero. The repeat JSON matched exactly.

Measured source: b4d8d4b99d5205cfd0f286711939793dbc7e7e01.
Worker checkpoint SHA256:
c346439b6ce1a6681c037167e16447e0f75f5491a2eafd88685b6e5d36292f8e.

[Audit outcomes](evidence/connectome-worker-audit-2026-09-12/comparison.json)
and [manifest](evidence/connectome-worker-audit-2026-09-12/manifest.json)
preserve the numeric evidence. Raw logs and station archives remain local.

This exposed a planner timer bug: assignment ran before dock/idle countdown,
then role logic consumed the expiry tick and either reset the timer or departed.
The fix lets assignment run on the tick where the timer reaches zero. A repair
assignment regression starts with a positive timer, verifies it waits for the
first tick, and verifies the repair occurs on the expiry tick. It failed before
the fix.

### Thirty-minute check after the timer fix

The same seed, checkpoint, and duration ran again from source
3b3432a with the timer fix. Strategy-on recorded **3,151 worker decisions and
9,391 candidate inferences**, with zero teacher decisions. Strategy-off recorded
zero worker decisions, consistent with its assignment gate. Both settings kept
the fly navigation circuit active.

Every earlier outcome field stayed identical in this seed, including 88/139
smelt units and 0/3 ship losses. All freight, crafting, construction, and contract
completion counts stayed at zero. The full repeat JSON and uncompressed signed
chain contents matched exactly. The result confirms restored model use; measured
economic outcomes stayed unchanged in this one-seed check. The full five-seed
study still describes the earlier implementation.

[Corrected outcomes](evidence/connectome-worker-fixed-2026-09-12/comparison.json),
[manifest](evidence/connectome-worker-fixed-2026-09-12/manifest.json), and
[production windows](evidence/connectome-worker-fixed-2026-09-12/production-windows.json).

Validation: all 1,711 C tests passed, with eight optional-tool warnings. The
17 assignment tests and eight Python analysis tests passed. Two roster fixtures
now hold other workers at dock so they test one replacement request, and observe
the new worker when it spawns.

Reproduce with a matching worker artifact from a clean committed checkout:

```sh
python3 scripts/compare_connectome_strategy.py \
  --probe build-study/connectome_swarm_probe \
  --worker-checkpoint /absolute/path/to/signal_npc_worker.nnckpt \
  --output /tmp/signal-worker-fixed \
  --ticks 216000 --seeds 2037 --workers 4 --timeout 3600
```

## Thirty-minute follow-up

The longer run shows an output and survival tradeoff. Strategy produced **260
smelted units versus 236** with it off, a **10.2%** increase. Ship losses were
**four versus one**. Per-world output improved twice and declined three times;
the median difference was minus one unit.

The same five seeds ran for 216,000 ticks each, or 30 simulated minutes. Both
settings for seed 2037 repeated for the full horizon. Four local processes ran
at once. The executable, circuit, runner, and brain settings matched the earlier
study exactly. Every first-five-minute smelt count also matched the earlier run.

| World seed | Smelt units: off | Smelt units: on | Difference | Ships lost: off / on |
|---|---:|---:|---:|---:|
| 2037 | 88 | 139 | +51 | 0 / 3 |
| 2141 | 28 | 64 | +36 | 0 / 0 |
| 3253 | 44 | 14 | -30 | 0 / 0 |
| 4363 | 44 | 43 | -1 | 1 / 1 |
| 5471 | 32 | 0 | -32 | 0 / 0 |
| **Total** | **236** | **260** | **+24** | **1 / 4** |

The three additional strategy-on losses were all in seed 2037. Its active fleet
ended at two ships, versus five with strategy off. Across all worlds, observed
hull loss was 247.47 off and 353.60 on; four seeds had lower observed damage with
strategy, while seed 2037 had substantially more. This is a concentrated risk.

### Output over time

The table counts newly produced units within each interval, summed across the
five worlds. Interval boundaries use simulation ticks from verified SMELT records.

| Simulated minutes | Strategy off | Strategy on |
|---|---:|---:|
| 0–5 | 143 | 196 |
| 5–10 | 45 | 64 |
| 10–15 | 48 | 0 |
| 15–20 | 0 | 0 |
| 20–25 | 0 | 0 |
| 25–30 | 0 | 0 |

Strategy produced 64 additional units after minute five; the off runs produced
93. Its aggregate lead narrowed from 53 units at five minutes to 24 at thirty.
Both settings produced zero units in the final fifteen minutes. Freight delivery,
NPC contract completion, craft events, and construction contributions stayed at
zero throughout all ten primary episodes.

**Take:** strategy can improve early output in some worlds, with a survival cost
in one of these longer runs. The next question is why fresh-world production
ends in both settings. The current evidence establishes the timing and outcomes;
resource supply, cargo movement, and worker behavior need inspection to explain
that limit. A later reward comparison should track completed work and ship losses
alongside an active freight scenario.

Both repeat runs matched every JSON fact and the uncompressed signed chain bytes.
All history checks passed and event buffers stayed below capacity. Measured source:
`b8169101b914798bb32625e591745dc642f91bdf`. The interval analyzer is recorded by its
own source hash in `production-windows.json`; its parser and archive checks bring
the comparison test suite to eight passing tests.

Evidence: [manifest](evidence/connectome-strategy-2026-09-12-30min/manifest.json),
[paired outcomes](evidence/connectome-strategy-2026-09-12-30min/comparison.json), and
[five-minute production windows](evidence/connectome-strategy-2026-09-12-30min/production-windows.json).
The repository contains numeric evidence and hashes. Raw logs and signed chain
archives remain in the local study output directory.

Reproduce the longer run from a clean committed checkout with the probe built:

```sh
python3 scripts/compare_connectome_strategy.py \
  --probe build-study/connectome_swarm_probe \
  --output /tmp/signal-strategy-long \
  --ticks 216000 --workers 4 --timeout 3600
python3 scripts/analyze_connectome_windows.py \
  --study /tmp/signal-strategy-long \
  --output /tmp/signal-strategy-long-windows.json
```

## Original five-minute results: 12 September 2026

The result is mixed. Strategy raised total smelt output by **37.1%** and one
world had fewer ship losses. Across seeds, output improved twice, declined twice,
and tied once. The median output difference was zero.

| World seed | Smelt units: off | Smelt units: on | Difference | Ships lost: off / on |
|---|---:|---:|---:|---:|
| 2037 | 88 | 110 | +22 | 0 / 0 |
| 2141 | 0 | 64 | +64 | 0 / 0 |
| 3253 | 0 | 0 | +0 | 0 / 0 |
| 4363 | 23 | 22 | -1 | 1 / 0 |
| 5471 | 32 | 0 | -32 | 0 / 0 |
| **Total** | **143** | **196** | **+53** | **1 / 0** |

The largest gain was 64 units in seed 2141. The other four seeds together produced
143 units with strategy off and 132 with it on. This makes the aggregate increase
sensitive to the starting world.

| Other outcome, summed over the five worlds | Strategy off | Strategy on |
|---|---:|---:|
| Freight units delivered | 0 | 0 |
| NPC contracts completed | 0 | 0 |
| Craft events | 0 | 0 |
| Construction contributions | 0 | 0 |
| Observed hull loss | 182.59 | 103.00 |
| Travel time, NPC-seconds | 3,244.93 | 3,052.95 |
| Towing time, NPC-seconds | 351.70 | 314.51 |
| Distance, world units | 336,343.79 | 345,358.52 |

The damage reduction is concentrated in seed 4363, which lost one ship with
strategy off. Freight, contracts, crafting, and construction remained at zero
throughout these fresh-world episodes. Their effectiveness remains unresolved at
this horizon. Each episode began with five NPCs, using the normal seeded roster.

**Recommendation:** keep the strategy classified as experimental. A useful next
comparison would put the current time-based reward beside a completion-based
reward in a world with an active freight route. The current comparison measures
the whole strategy switch; a separate control is needed to isolate reward learning
from job reassignment and posture modulation.

Both full-horizon repeat runs matched every JSON fact, including the connectome
checksum. The uncompressed signed chain files also matched byte for byte. All
station histories passed verification. Every event buffer stayed below capacity.

Measured source: `afe6f23006126e3f7cd14b5151ba09482468e4fb`, which includes the
drive fix `41978b061df22476d537740f6efae0647e38469e`. Build: Release, macOS ARM64.
Sixteen connectome tests and four comparison tests passed. The parser, workflow,
path classification, banned API, and deterministic math checks passed.

Evidence: [manifest and artifact hashes](evidence/connectome-strategy-2026-09-12/manifest.json),
[all paired facts and summaries](evidence/connectome-strategy-2026-09-12/comparison.json),
and [numeric episode reports](evidence/connectome-strategy-2026-09-12/).
Raw episode logs and signed chain archives remain in the local study output
directory. The manifest lists the complete original artifact set; the repository
contains its numeric reports and hashes.
Archives retain their original filesystem metadata, so their compressed hashes
can differ between repeat runs while the contained chain bytes match.

## Fixed design

- Fresh server genesis, including cargo origin records; zero connected players.
- World seeds: 2037, 2141, 3253, 4363, 5471.
- 36,000 simulation ticks per episode: five minutes at 120 Hz.
- Circuit: committed `assets/connectome/nav.cnx`; budget: 30; strategy period: 300.
- One off/on pair per seed. Both modes for seed 2037 repeat for the full horizon.
- Separate processes and temporary history directories for every episode.
- Environment reduced to basic process settings and the listed brain settings.
- Differences are strategy on minus off. Every pair is reported, including zero
  output or a lost fleet. Repeat episodes check exact equality of all JSON facts.

## Measures

- **Smelt output units:** new SMELT records in verified station histories, one per
  output unit. Genesis records are subtracted. This is whole-world production.
- **Delivered units:** increases in shipment `quantity_delivered`, sampled after
  every tick. Shipment IDs distinguish reused slots. Loading cargo counts as zero.
- **Contract completions:** NPC completion events. A full event buffer makes the
  runner reject the episode for review.
- **Craft events / construction contributions:** new verified CRAFT and
  CONSTRUCTION records. A craft event can produce several units.
- **Destroyed ships:** newly destroyed ship assets, counted once per asset ID.
- **Observed hull loss:** positive hull decreases between samples, plus remaining
  hull when an asset is destroyed between samples. Same-tick repair can hide some
  damage, so this is a lower bound. Replacement ships start fresh observations.
- **Travel / idle / docked / towing time:** sum of NPC ticks in those states,
  divided by 120 for seconds. These are exposure measures; output measures above
  describe completed work.
- **Distance:** sampled displacement for each continuing ship asset. Replacement
  positions start a fresh distance track.

A five-seed fresh-world study describes these starting conditions and this
horizon. Player actions, mature economies, larger fleets, and longer learning
periods need separate measurements. The strategy switch changes several behaviors
at once, so this comparison measures the combined effect. Station craft and smelt
records describe total production, including autonomous station processes.

## Reproduce

```sh
cmake -S . -B build-study -DBUILD_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-study --target connectome_swarm_probe -j 4
python3 scripts/test_compare_connectome_strategy.py
python3 scripts/compare_connectome_strategy.py \
  --probe build-study/connectome_swarm_probe \
  --output /tmp/signal-strategy-comparison
```

The runner requires a clean committed checkout and rebuilds its probe. It records
source, circuit, runner, and executable hashes. JSON facts, raw logs, signed chain
archives, paired differences, repeat checks, and artifact hashes are retained in
the output directory. A failed run keeps its available evidence and failure reason.
