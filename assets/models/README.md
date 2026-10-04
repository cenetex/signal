# Models

## `signal_npc_worker.nnckpt`

Trained NPC worker-option ranker (`signal-npc-worker-v2`, feature encoder 2,
layers `78-32-16-1`, 3073 parameters).

- **sha256:** `c346439b6ce1a6681c037167e16447e0f75f5491a2eafd88685b6e5d36292f8e`
- **size:** 12,855 bytes
- **trained by:** the crlplrimes `signal-npc-worker-economy` target
  (96 train groups / 768 test groups) — top1 77.5%, top3 95.2%, regret 0.22
  against a 2.54 teacher baseline.

The server loads it via `SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT` (see
`fly.toml`) and scores NPC worker options with it when
`SIGNAL_NPC_WORKER_BRAIN_MODE` is `mixed` or `active`.

The trainer builds its feature vector by calling the runtime's own
`signal_npc_worker_build_features()`, and both sides `_Static_assert` the
feature and option counts, so this artifact can only be loaded against a
matching feature contract. **Regenerate it whenever the v2 feature layout
changes** by rerunning that crlplrimes target; a stale checkpoint is rejected
at load with an encoder/feature-set error rather than silently mis-scoring.
