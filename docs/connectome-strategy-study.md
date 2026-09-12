# Station strategy comparison

This study compares the complete `SIGNAL_CONNECTOME_STRATEGY` switch: station
postures, reward learning, and worker job reassignment. Both settings use the
same connectome, its fixed seed of 42, and the drive-history fix in PR #736.

## Results: 12 September 2026

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
