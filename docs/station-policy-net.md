# Station Policy Net: Stronger-Model Plan

**Status:** plan. Current shipped model is `assets/models/signal_npc_worker.nnckpt`.

## Goal

Make the station the actual strategist: a policy that chooses postures from
world state well enough to beat the heuristic, not merely imitate it. Today the
station posture sample is drive weights + bandit + the trained worker model's
option prior (`server/signal_connectome_brain.c`, `cb_station_model_bias`).

## Where we are

- The shipped model is a worker-option utility ranker: `signal-npc-worker-v2`,
  feature encoder 2, `78-32-16-1`, trained by the crlplrimes
  `signal-npc-worker-economy` target on a synthetic environment.
- Trainer and runtime share one feature builder
  (`signal_npc_worker_build_features`), enforced by `_Static_assert`, so the
  artifact cannot silently mis-score.
- It biases station postures by ranking five representative options
  (`cb_station_model_bias`), bounded and confidence-tapered.

## The four gaps

1. **Feature coverage.** The trainer's synthetic world does not model several
   v2 features: route success/proof memory, hologram resonance, source memory,
   provenance pressure, trust bias, black-market acceptance, policy screening,
   black-market station, contraband opportunity. Those weights are trained on a
   constant zero and then see real values at inference — a distribution shift.
2. **Objective.** It is behavior cloning against the teacher's utility, so it
   cannot exceed the teacher. A stronger policy needs an outcome signal.
3. **Data.** Training is synthetic only. The runtime already emits real rows via
   `SIGNAL_NPC_WORKER_TRACE` (schema `signal.npc_worker_shadow.v3`), and those
   are unused.
4. **Decision level.** The model ranks per-fly worker options; the station wants
   a preference over 5 postures. The current mapping is a bridge, not a trained
   station objective.

## Plan

### Phase 1 — Trainer parity (unblocks everything)
Extend the crlplrimes `signal_npc_worker` environment to emit the full v2
feature set: route memory, hologram/VSA resonance, trust/proof, and the
black-market/policy flags. Reuse the runtime's candidate struct verbatim.

Acceptance: a coverage assertion in the trainer that every one of the 78
features is exercised (non-constant across the generated set); retrain and
confirm the zeroed-feature weights are no longer inert.

### Phase 2 — Real-data training
Add a dataset builder that consumes `SIGNAL_NPC_WORKER_TRACE` rows from a
persistent/soak run and produces `(78 features, chosen option, outcome)` rows.
Train on real rows; hold out whole groups (not rows) to avoid leakage.

Acceptance: held-out top1 / top3 / regret vs the teacher on real rows, plus a
calibration report. Reject if it does not beat the teacher-fallback baseline.

### Phase 3 — Outcome objective
Replace imitation with a verifier-grounded score: ore delivered, credits earned,
contracts completed, survival, per station window. The crlplrimes harness
already supports verifier/RL modes; keep the deterministic verifier as the
authority and let the net only *propose*.

Acceptance: positive regret reduction over the teacher on held-out scenarios,
no replay divergence, and the deterministic gates still pass.

### Phase 4 — Station-level head
Train a small policy over the 5 postures (or over the 12 options the postures
map to) against station-window outcomes, so the station objective is trained
directly instead of bridged from a per-fly ranker. Ship as a second artifact or
extend the same encoder; either way it goes through the shared feature builder.

Acceptance: on the probe, the model-biased distribution beats the bandit-only
run on delivered ore / productive fly-ticks over the same seed.

### Phase 5 — Shipping and regeneration
- Artifact in `assets/models/`, sha256 recorded, `SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT` set.
- Add a CI check that loads the shipped checkpoint through the runtime loader
  (fails on encoder/feature mismatch) so a stale artifact never reaches deploy.
- Regeneration command documented; the `_Static_assert`s make a layout change a
  compile error rather than a silent mis-score.

## Determinism

The model forward is `double`. That is fine while the station policy is
runtime-only (the connectome is not in the replay bundle). If a net ever enters
the deterministic replay path, move it to fixed-point or extend the
deterministic-libm allowlist, and gate it with `signal_connectome_checksum()` /
the replay cross-build checks.

## Tuning knobs (shipped now)

| Knob | Default | Meaning |
|---|---|---|
| `SIGNAL_CONNECTOME_STRATEGY_MODEL_WEIGHT` | 50 | 0..100 ceiling on the model's posture bias (0 ignores the model) |
| `SIGNAL_CONNECTOME_STRATEGY_PERIOD` | 300 | ticks a station holds a posture |

The bias ceiling is 16 at weight 100 and tapers when the model's top two scores
are close, so a low-confidence model cannot outvote the drive weights.

## Risks

- **Overfitting the teacher's blind spots** if trained on `SIGNAL_NPC_WORKER_TRACE`
  selection only; use held-out groups and the outcome field.
- **Feature drift** is now a load-time failure, which is safer but means a
  trainer update can block a deploy until retrained — intentional.
- **Model dominance** is bounded by the weight knob, but the default 50 should be
  validated against the probe on the target VM before raising it.
