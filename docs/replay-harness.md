# Deterministic Replay Harness

`signal_replay` is the seed+prefix counterfactual harness for research and
agent testing. It rebuilds a deterministic Signal world, replays a fixed input
history, branches one or more candidate actions for a bounded horizon, and emits
one JSONL row per branch.

This is not full chain-log world reconstruction. Chain-log verification remains
`signal_verify`, and ledger-to-world rebuild is still future work. The replay
harness is the narrow API needed by external experiments that want stable
counterfactual rollouts from the current simulator.

## Build And Run

```sh
make signal-replay
```

The Make target writes `/tmp/signal-replay.jsonl` by default. Override inputs
with Make variables:

```sh
make signal-replay \
  SIGNAL_REPLAY_SEED=4242 \
  SIGNAL_REPLAY_HISTORY='W,W,WA,D' \
  SIGNAL_REPLAY_HORIZON_TICKS=60 \
  SIGNAL_REPLAY_CANDIDATES='W,A,D'
```

Or call the binary directly:

```sh
./build/signal_replay \
  --seed 4242 \
  --history W,W,WA,D \
  --horizon-ticks 60 \
  --candidates W,A,D \
  --out /tmp/signal-replay.jsonl
```

Supported actions are `NONE`, `W`, `A`, `D`, `S`, `WA`, `WD`, `SA`, and `SD`.
Numeric action ids `0..8` are accepted for compact generated traces.

## Scenario Controls

- `--seed N`: world seed, default `2037`.
- `--station N`: station index used for the default spawn and goal.
- `--spawn X,Y`: explicit starting position after world construction.
- `--velocity X,Y`: explicit starting velocity.
- `--angle R`: explicit starting angle in radians.
- `--goal X,Y`: explicit utility target.
- `--history LIST`: comma-separated prefix actions to replay before branching.
- `--horizon-ticks N`: number of ticks to simulate per candidate.
- `--candidates LIST`: comma-separated candidate actions to branch.
- `--out PATH`: JSONL output path. Without it, rows go to stdout.

## Output Contract

Each row has schema `signal.replay_counterfactual.v1` and includes:

- `prefix_state_hash`: hash after replaying the shared input prefix.
- `state_hash`: hash after the candidate branch horizon.
- `event_hash`: hash of branch events in simulator order.
- Safety metrics: hull, hull loss, damage events, death events, dock/launch
  events, and repair events.
- Economy metrics: cargo, balance, buy/sell counts, buy cost, buy quantity, and
  sell credits.
- Motion metrics: end position, velocity, speed, angle, goal distance, progress,
  and scalar utility.
- `authority`: currently `deterministic_seed_prefix_replay`.

The hashes include the world tick/time, belt seed, player ship state, cargo
manifest, station identity, station inventory cache, fracture claim windows,
player ledger balance by session token and pubkey, and station chain tail.
Float fields are hashed as their exact IEEE-754 bits, not rounded display
values, so one-bit native/WASM or cross-build drift fails the diff. This makes
repeated runs with the same seed and prefix cheap to diff and safe to ingest
from CRLPLRIMES-style experiments.

## Determinism Check

```sh
./build/signal_replay --seed 4242 --history W,W,WA,D \
  --horizon-ticks 8 --candidates W,A,D --out /tmp/replay-a.jsonl
./build/signal_replay --seed 4242 --history W,W,WA,D \
  --horizon-ticks 8 --candidates W,A,D --out /tmp/replay-b.jsonl
diff -u /tmp/replay-a.jsonl /tmp/replay-b.jsonl
```

An empty diff is the expected result.

For the native-vs-WASM determinism gate, build the Emscripten replay CLI and
compare it against the native replay binary:

```sh
make replay-native-wasm
```

Long-horizon drift probes live in a separate scenario set so day-to-day checks
can stay quick while #588 work still has a hard accumulation test:

```sh
make replay-native-wasm-long
```

The long set includes two 10,000-tick probes and one 100,000-tick probe. A
cross-build mismatch prints the first differing JSON row plus both output paths.
