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
- `--evaluation-world NAME`: apply a versioned AI evaluation topology after
  `world_reset()`. Supported worlds are `seeded-only`, `seeded-sparse`,
  `outpost-low`, `outpost-mid`, `outpost-high`, `scarcity`, `weak-signal`,
  `route-disrupted`, `permutation-low`, and `permutation-high`.
- `--out PATH`: JSONL output path. Without it, rows go to stdout.

## Output Contract

Each row has schema `signal.replay_counterfactual.v1` and includes:

- `prefix_state_hash`: hash after replaying the shared input prefix.
- `state_hash`: hash after the candidate branch horizon.
- `public_state_hash_schema`: public replay-state hash contract name.
- `public_state_hash_version`: currently `7`; this version removes bearer
  credentials from replay-state hashing and collapses legacy token-ledger
  keys to an explicit legacy marker.
- `state_digest_schema`: canonical peer/quorum digest schema.
- `state_digest_version`: numeric canonical digest version.
- `prefix_state_root`: canonical authoritative root at the branch point.
- `state_root`: canonical authoritative root after the branch horizon.
- `event_hash`: hash of branch events in simulator order.
- `public_event_hash_schema`: public replay-event hash contract name.
- `public_event_hash_version`: currently `3`; event attribution hashes typed
  public actor IDs and never raw killer/session tokens.
- State hashes include each station's versioned public authority registry
  (public keys, lifecycle, and trust decisions); private station keys are never
  part of replay state or output.
- Safety metrics: hull, hull loss, damage events, death events, dock/launch
  events, and repair events.
- Economy metrics: cargo, balance, buy/sell counts, buy cost, buy quantity, and
  sell credits.
- Motion metrics: end position, velocity, speed, angle, goal distance, progress,
  and scalar utility.
- `receipt_trust`: an always-on `signal.receipt_trust.v1` known-vector
  contract covering trusted SMELT and CRAFT origins plus every typed rejection
  or lifecycle verdict. Replay aborts if any vector produces a different
  semantic code; native/WASM and cross-runner gates compare the emitted codes
  exactly.
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
  sizes, station/NPC known-contract counts, station-held remote contract and
  remote market-memory counts, station/NPC HNN market trace stored counts,
  station flight-experience stored count, version sums, spatial signal-field
  load/noise/margin/SNR diagnostics, and max trace loads. The
  `signal.replay_ai_memory.v4` rows also include branch-wide bounded counters:
  active NPC ticks, selected/HNN diagnostic row peaks, selected assignment
  ticks by job family, HNN-backed assignment ticks, route support ticks, worker
  motion ticks, worker cargo ticks, scaffold-motion ticks, delivery-shipment
  ticks, and useful-outcome ticks. The state hash also includes station/NPC knowledge, job
  diagnostics, and HNN trace bodies so cognition drift changes the replay hash
  rather than only the display counters.
- `evaluation`: the `signal.ai_eval_world.v1` topology report. Every row
  records corpus version, generator version, scenario, active/generated/weak/
  disconnected station counts, production and consumption edge counts,
  outpost slots, a slot-sensitive topology hash, and a slot-invariant semantic
  topology hash.
- `outcome_facts`: strict `signal.ai_outcome_facts.v1` evidence used to derive
  per-head episode outcomes. It records feature-contract versions, bounded
  completion ticks, player and worker route progress, collision/death/stuck/
  recovery/loop counters, safety fallbacks, station needs served, and cargo
  manifest/receipt-chain integrity at both episode boundaries.
- `authority`: currently `deterministic_seed_prefix_replay`.

## AI Episode Outcome Contract

Every replay bundle uses schema `signal.replay_bundle.v3` and contains a
strictly validated `outcomes.json` report with schema
`signal.ai_episode_report.v1`. Each report has one
`signal.ai_episode_outcome.v1` record per replay row and intelligence head:
`flight`, `contract`, and `worker`.

Episode boundaries are deliberately narrower than an entire game session:

- a flight episode begins after the shared seed, prefix, and provenance setup,
  and ends at goal arrival, ship death, or the candidate horizon;
- a contract episode begins when a contract decision or completion/delivery
  event is observed, and ends at completion, a terminal rejection, or the
  horizon; rows with no contract evidence are `not_attempted`;
- a worker episode begins with a selected assignment and ends when the bounded
  fixture clears a delivery, completes a scaffold move, finishes a kit-backed
  repair, defaults a shipment, or reaches the horizon; rows with no assignment
  evidence are `not_attempted`.

Statuses are `not_attempted`, `in_progress`, `completed`, and `failed`.
`in_progress` is explicitly marked as truncated; reaching the horizon is never
silently scored as success or failure. A completed contract or worker episode
must have a deterministic completion tick or report construction fails.

Replay IDs hash the corpus/version, generator version, scenario index, and
exact scenario arguments. Episode IDs additionally hash the candidate, head,
and outcome report version. The four decision modes therefore operate on
exactly the same episode IDs:

- `teacher` is the authoritative deterministic fallback;
- `shadow` records the requested comparison mode while teacher decisions
  remain authoritative;
- `mixed` and `active` configure the worker brain accordingly, but every
  episode records effective mode, model decisions, and teacher fallbacks;
- flight remains a replay counterfactual candidate in all four modes and says
  so explicitly rather than claiming an active model decision.

Unknown fact/report schemas, encoder-version drift, malformed route metrics,
non-finite values, or broken cargo lineage fail closed. Attempted-episode
summaries include completion rate/ticks, collision and death events, stuck and
recovery counts, repeated route cells, safety overrides, route efficiency,
behavioral diversity, station need served, and receipt-chain failures.
`not_attempted` rows are counted but do not pollute those metric totals.
The short corpus exercises both completed and horizon-truncated flight
episodes plus completed contract, tow, repair, and delivery-proof episodes.

Run the short deterministic gates with:

```sh
make replay-ai-outcome-repeatability
make replay-ai-outcome-native-wasm
make replay-ai-outcome-modes
```

The first also runs schema/version unit tests. The native/WASM gate compares
the raw facts byte-for-byte, including `outcomes.json`. The four-mode gate
requires identical episode IDs before emitting
`signal.ai_episode_comparison.v1` summaries.

## AI Evaluation World Corpus

The `signal-ai-eval` corpus version 1 is generated by evaluation-world
generator version 1. It is intentionally separate from the existing gameplay
replay set: it adds controlled topology pressure without replacing the normal
flight, provenance, worker, or long-horizon probes.

The short pull-request set contains:

- seeded-only and two-active-station sparse worlds;
- the same generated relay/industrial outpost in representative low (4),
  middle (64), and high (127) non-seeded slots;
- finished-goods scarcity;
- a connected weak-signal outpost;
- a disconnected route outpost;
- a semantic permutation pair that moves the same outpost from slot 4 to slot
  127.

The permutation gate requires different slot-sensitive topology hashes, equal
semantic topology hashes, the same candidate set, and equal legal branch
outcomes after station-index fields and raw state hashes are remapped away.
This prevents a byte-stable replay from hiding station-slot dependence.

Run the short repeatability and native/WASM gates with:

```sh
make replay-ai-eval-repeatability
make replay-ai-eval-native-wasm
```

The longer native set covers scarcity, weak signal, route disruption, and a
100,000-tick high-slot outpost probe:

```sh
make replay-ai-eval-repeatability-long
```

The Deterministic Replay workflow accepts `ai-eval-fast` and `ai-eval-long`
manual scenario sets. Its weekly scheduled job exports the long native bundle
as a 30-day artifact. Bundle manifests use `signal.replay_bundle.v3`; every
scenario entry records the corpus/generator versions and expected invariants,
every JSONL row repeats those versions with its measured topology summary, and
the manifest authenticates the versioned `outcomes.json` report.

The fast repeatability gate includes focused active-worker fixtures for
`worker-tow-hnn`, `worker-repair-hnn`, `worker-delivery-proof-hnn`, and
`worker-gossip-courier`. The tow fixture requires an HNN-backed scaffold
pickup. The repair fixture requires an HNN-backed repair assignment and fails
unless the worker actually consumes repair kits and recovers hull. The
delivery-proof fixture requires an HNN-backed delivery-proof assignment, real
bound-cargo pickup, destination delivery, receipt memory emission, and origin
proof clearing. The gossip-courier fixture starts with a contract known only at
one station and fails unless a worker physically carries that contract and its
market-memory impression into a different station, where the spatial
signal-field records the received demand locally.

The `*_state_hash` fields are public replay diagnostics governed by
`public_state_hash_schema` and `public_state_hash_version`; token changes do
not change them. The `event_hash` contract is independently versioned by
`public_event_hash_schema` and `public_event_hash_version`. The
`*_state_root` fields use the audited coverage and exclusions in
[`authoritative-state-digest.md`](authoritative-state-digest.md). Float fields
are hashed as their exact IEEE-754 bits, not rounded display values, so one-bit
native/WASM or cross-build drift fails the diff. This makes repeated runs with
the same seed and prefix cheap to diff and safe to ingest from
CRLPLRIMES-style experiments.

The replay lane also runs `signal_replay --self-test-public-hash`. Its
in-process fixture mutates player and NPC session tokens, asteroid
tow/throw/fracture tokens, fracture-claim token rows, legacy token-shaped
ledger keys, and legacy death/NPC-kill token fields, then requires both public
hashes to remain unchanged. It also mutates one public world field and one
public event field and requires the corresponding hashes to change.

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

CI also exports the same replay scenario set as stable artifacts on Linux x64,
macOS ARM, and WASM. A final cross-runner job compares the manifests and every
JSONL scenario byte-for-byte. Each bundle includes per-scenario SHA-256 and
size metadata, and all three runner bundles are retained as workflow artifacts
so a cross-platform mismatch can be inspected rather than reproduced blindly.
Use the **Deterministic Replay** workflow's `scenario_set=long` dispatch to run
the same artifact gate over the long-horizon probes.

The bundle tools can also be used directly:

```sh
python3 scripts/build_replay_bundle.py \
  ./build/signal_replay /tmp/replay-native --scenario-set fast
python3 scripts/build_replay_bundle.py \
  ./build-replay-wasm/signal_replay.js /tmp/replay-wasm --scenario-set fast
python3 scripts/check_replay_bundles.py \
  /tmp/replay-native /tmp/replay-wasm
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
