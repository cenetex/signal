# HNN confidence calibration

Signal now has a fixed abstention gate around holographic flight advice. The
gate is private to Signal and does not change contracts, the feature encoder,
the action vocabulary, HoloNet routing, or any authoritative game rule.

## Frozen artifact

[`hnn-calibration.json`](./hnn-calibration.json) is generated offline from 56
deterministic null replay scenarios on each of the three numeric backends. It
is tied to:

- HNN contract version 1;
- dimension 1,024;
- key generator version 1;
- pilot encoder version 2;
- trace format version 1;
- trace capacity 128;
- the exact action-vocabulary hash; and
- `builtin-radix2`, `lecore-direct`, and `lecore-radix2`.

The runtime table is generated into
`shared/holographic_nn_calibration_data.h`. Compile-time checks reject a stale
contract, encoder, dimension, seed, trace format, or capacity. The runtime
also checks the action-vocabulary hash before accepting advice. Thresholds
are constants; live player traffic cannot update them.

## Null controls and result

The corpus covers queries paired with unrelated traces, rotated action
labels, 129-item overloaded traces, and weak, sparse, scarce, or disrupted
worlds. Every fifth scenario is held out from threshold selection.

The checked result is 0 false-confidence decisions in 36 held-out trials
(12 per backend), or 0.00%, below the 1% limit. All 24 overloaded trials were
rejected by capacity. All 18 matched-trace controls were accepted.

## Runtime behavior

`SIGNAL_HNN_CONFIDENCE_MODE` accepts two values:

- `shadow` (default): score the HNN and report the gate result, but always use
  the deterministic teacher;
- `mixed`: use HNN advice only when capacity, score, margin, legal-action, and
  safety checks all pass. Every rejection uses the deterministic teacher.

Unknown or missing values resolve to `shadow`. The mode and calibration are
read as fixed process state; there is no threshold drift during a run.

## Reproduction

Build `signal_replay` once for each backend, then run:

```text
python3 scripts/calibrate_hnn_confidence.py \
  --builtin build/signal_replay \
  --direct build-lecore-direct/signal_replay \
  --radix2 build-lecore-radix2/signal_replay \
  --artifact docs/liblecore-pilot/hnn-calibration.json \
  --header shared/holographic_nn_calibration_data.h \
  --check
```

This is an offline development check. Signal does not require Python or
NumPy at runtime.

## Pilot decision

The confidence work passes its false-confidence and fallback gates. The
overall libleCore recommendation remains **hold**: keep the built-in backend
as the default while libleCore remains ABI 0. `lecore-radix2` remains the
preferred optional pilot backend; `lecore-direct` remains a correctness
oracle.
