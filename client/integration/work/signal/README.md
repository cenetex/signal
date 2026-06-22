# Signal Client Brain Bundle

Generated static C inference bundles exported from CRLPLRIMES checkpoints for
Signal client integration work.

## Contents

- `signal_client_brain.*`: unified typed client API.
  - `float signal_client_brain_score(enum signal_brain_task task, const float *features)`
  - `int signal_client_brain_select_best(enum signal_brain_task task, const float *features, size_t candidate_count, float *scores_out)`
  - `signal_client_brain_task_contract signal_client_brain_contract(enum signal_brain_task task)`
- `signal_client_flight.*`: reactive ship flight scorer.
  - Feature set: `signal-flight-live-v2`
  - Encoder version: `2`
  - Shape: `48 -> 32 -> 16 -> 1`
- `signal_client_tactical.*`: tactical mining option scorer.
  - Feature set: `signal-mining-grammar-v1`
  - Encoder version: `3`
  - Shape: `70 -> 32 -> 16 -> 1`
- `signal_client_strategic.*`: strategic NPC worker option scorer.
  - Feature set: `signal-npc-worker-v2`
  - Encoder version: `1`
  - Shape: `78 -> 32 -> 16 -> 1`

Each `.c` file contains the neuron weights and biases as aligned `static const`
float arrays. The hot path has no checkpoint parser, no heap allocation, no temp
files, and uses fixed-size stack scratch buffers.

The unified API dispatches by `enum signal_brain_task` and preserves each
head's feature contract. Call `signal_client_brain_contract(task)` when wiring a
feature encoder so the caller can validate the expected feature set, encoder
version, input length, and checkpoint hash.

`client/neural_singleplayer.c` now initializes this static bundle directly for
local single-player/client shadow work. It validates the three task contracts,
logs each checkpoint hash prefix and zero-vector probe score, and no longer
writes a temporary `.nnckpt` file.

The client also runs live flight shadow scoring during simulation ticks when
the local player has an active autopilot flight target. It reuses
`signal_brain_build_flight_candidate_features(...)`, the same 48-feature
candidate encoder used by the legacy flight brain, then scores the nine
candidate actions through `signal_client_brain_score(SIGNAL_BRAIN_TASK_FLIGHT,
...)`. Manual free-flight is intentionally skipped until it has a real target
contract.

Shadow rows are emitted as sparse JSONL. In local single-player, the row can
include the exact server-side autopilot teacher decision captured at the same
decision point as the candidate feature matrix:

```json
{"schema":"crlp.signal_client_flight_shadow.v1","sample":1,"tick":120,"task":"flight","feature_set":"signal-flight-live-v2","feature_hash":"...","allowed_mask":"0x1eb","best_raw":{"index":6,"name":"WD","score":1.23},"best_allowed":{"index":6,"name":"WD","score":1.23},"manual_intent":{"index":0,"name":"NONE","score":0.41},"teacher":{"index":6,"name":"WD","score":1.23,"matches_best_allowed":true},"allowed_margin":0.17,"actions":[...]}
```

`feature_hash` is FNV-1a over the exact float bits for the full 9x48 candidate
matrix. `manual_intent` is the client's sampled manual input for that tick.
`teacher` is present when the local server exposed the authoritative autopilot
choice for the same candidate matrix; remote/network paths can still emit rows
without teacher labels until the protocol carries that snapshot.

## Shadow Evaluation Gate

After collecting client stdout or extracted JSONL, run:

```sh
node scripts/analyze-signal-client-brain-shadow.mjs \
  --input /tmp/signal-client-brain-shadow.jsonl \
  --min-rows 100 \
  --min-teacher-rows 100 \
  --min-teacher-match-rate 0.85
```

or:

```sh
make signal-client-brain-shadow \
  SIGNAL_CLIENT_BRAIN_SHADOW_LOG=/tmp/signal-client-brain-shadow.jsonl \
  SIGNAL_CLIENT_BRAIN_SHADOW_MIN_ROWS=100 \
  SIGNAL_CLIENT_BRAIN_SHADOW_MIN_TEACHER_ROWS=100 \
  SIGNAL_CLIENT_BRAIN_SHADOW_MIN_MATCH=0.85
```

The analyzer reports teacher top-1 match rate, teacher rank counts,
best-allowed margins, best-minus-teacher score deltas, action confusion, and
model feature-contract versions. Treat these thresholds as the promotion gate
from shadow mode to any active client control.

## Integration Stance

Use this directory as the Signal-side output of the CRLPLRIMES training pipeline.
Regenerate it after retraining; do not hand-edit generated sources.

The flight brain is the first active-client candidate because its feature set
matches the current live ship contract. The tactical mining brain should remain
shadow/gated until replay runs show it is no longer trading safety for utility.
The strategic NPC worker brain has stronger offline signals, but the current
Signal server loader still has older `signal-npc-worker-v1` expectations, so it
needs an explicit feature-contract bridge before active use.

## Export Command

Run from `/Users/ratimics/develop/crlplrimes`:

```sh
node scripts/export_signal_client_brain.mjs \
  --checkpoint build/signal_flight.nnckpt \
  --out ../signal/client/integration/work/signal \
  --name flight \
  --symbol signal_client_flight \
  --require-feature-set signal-flight-live-v2 \
  --require-feature-version 2

node scripts/export_signal_client_brain.mjs \
  --checkpoint build/float/signal_mining_experiment/signal_mining.nnckpt \
  --out ../signal/client/integration/work/signal \
  --name tactical \
  --symbol signal_client_tactical \
  --require-feature-set signal-mining-grammar-v1 \
  --require-feature-version 3

node scripts/export_signal_client_brain.mjs \
  --checkpoint build/float/signal_npc_worker_economy/signal_npc_worker.nnckpt \
  --out ../signal/client/integration/work/signal \
  --name strategic \
  --symbol signal_client_strategic \
  --require-feature-set signal-npc-worker-v2 \
  --require-feature-version 1
```

## Compile Smoke

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -I../signal/client/integration/work/signal \
  -c ../signal/client/integration/work/signal/signal_client_flight.c \
  -o /tmp/signal_client_flight.o

cc -std=c11 -Wall -Wextra -Werror \
  -I../signal/client/integration/work/signal \
  -c ../signal/client/integration/work/signal/signal_client_brain.c \
  -o /tmp/signal_client_brain.o
```
