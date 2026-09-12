# Station strategy comparison

This study compares the complete `SIGNAL_CONNECTOME_STRATEGY` switch: station
postures, reward learning, and worker job reassignment. Both settings use the
same connectome, its fixed seed of 42, and the drive-history fix in PR #736.

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
