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
  --hnn-trace \
  --active-workers \
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
- `--hnn-trace`: train a holographic trace from the prefix and score each
  branch-start candidate without changing the rollout.
- `--active-workers`: keep seeded NPC workers active after `world_reset()` and
  include worker/gossip/HNN memory metrics in each output row. Without this
  flag, replay disables seeded NPC ships so fast physics/economy probes stay
  narrow.
- `--hnn-cleanup-steps N`: retrieval cleanup steps for `--hnn-trace`, default
  `3`, range `0..8`.
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
- Optional `hnn` object when `--hnn-trace` is set: flat trace contract,
  routed HoloNet contract, route diagnostics, stored count/load/fidelity, raw
  HNN top action, gated top legal action, legal action mask, candidate
  score/rank, candidate legal rank, raw and gated margins, and all action
  scores at the branch start. The trace is trained only from prefix actions,
  so utility-vs-HNN-rank analysis remains counterfactual.
- Optional `ai` object when `--active-workers` is set: active NPC count,
  worker diagnostic rows, selected rows, hologram-backed rows, selected
  mine/haul/tow/proof/scout/repair rows, HNN-backed rows per job family,
  worker travel/towing/cargo state counts, NPC-owned delivery-shipment status
  counts, scaffold loose/towing/snapping/placed counts, station/NPC knowledge
  sizes, station/NPC known-contract counts, station/NPC HNN market trace
  stored counts, station flight-experience stored count, version sums, and max
  trace loads. The `signal.replay_ai_memory.v2` rows also include branch-wide
  bounded counters: active NPC ticks, selected/HNN diagnostic row peaks,
  selected assignment ticks by job family, HNN-backed assignment ticks, route
  support ticks, worker motion ticks, worker cargo ticks, scaffold-motion
  ticks, delivery-shipment ticks, and useful-outcome ticks. The state hash also
  includes station/NPC knowledge, job
  diagnostics, and HNN trace bodies so cognition drift changes the replay hash
  rather than only the display counters.
- `authority`: currently `deterministic_seed_prefix_replay`.

The fast repeatability gate includes focused active-worker fixtures for
`worker-tow-hnn`, `worker-repair-hnn`, and `worker-delivery-proof-hnn`. The tow
fixture requires an HNN-backed scaffold pickup. The repair fixture requires an
HNN-backed repair assignment and fails unless the worker actually consumes
repair kits and recovers hull. The delivery-proof fixture requires an
HNN-backed delivery-proof assignment, real bound-cargo pickup, destination
delivery, receipt memory emission, and origin proof clearing.

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

HNN replay evaluation is deterministic too:

```sh
./build/signal_replay --seed 6060 --history W,WA,W,WD,A,D \
  --horizon-ticks 24 --candidates W,WA,WD,S --hnn-trace \
  --out /tmp/replay-hnn.jsonl
```

Compare candidate `utility` against `hnn.candidate_allowed_rank`,
`hnn.top_allowed_action`, `hnn.candidate_score`, and
`hnn.holonet.last_route` to test whether routed prefix memory helps or hurts
legal candidate ordering after the safety gate.

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
The fast set includes focused `worker-tow-hnn`, `worker-repair-hnn`, and
`worker-delivery-proof-hnn` fixtures that force selected worker jobs with
hologram-backed diagnostic rows, advance the workers through scaffold pickup,
kit-backed hull recovery, or bound-cargo delivery/proof clearing, and validate
the selected job-family/HNN/outcome counters. The long set also includes an
active-worker probe that keeps seeded NPC workers alive for 10,000 ticks and
validates that the output contains active NPCs, exchanged knowledge, HNN market
memory, branch-wide active ticks, and useful worker outcome, route-support, or
worker-motion ticks.
That makes neural-worker, gossip, and holographic-memory drift part of the
deterministic replay surface instead of a live-session anecdote. This is still
not full ledger replay or a complete economic autonomy proof; it is the first
deterministic gate for the active worker/gossip/HNN path.
