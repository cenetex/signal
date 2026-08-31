# liblecore replay evidence

The pilot passes its correctness and replay gates. The recommendation is
**hold**: keep Signal's built-in radix-2 backend as the default, keep
`lecore-direct` as a correctness oracle, and use `lecore-radix2` as the
preferred optional pilot backend. Calibrated abstention still needs to land,
and liblecore is still an ABI-0 preview.

The checked summary is
[`replay-evidence.json`](replay-evidence.json). The full report, including all
run durations and bundle digests, is reproducible with
`scripts/run_liblecore_pilot.py`.

## Correctness and outcomes

- Primitive bind, unbind, and action-score differences are below `1e-5`.
- Fast replay chose the same legal action in 4/4 HNN rows for both liblecore
  modes.
- All 44 fast rows and all four long rows preserved safety, cargo, receipt,
  event, and episode-outcome facts.
- The fast set was byte-repeatable across five runs per backend. The long set
  was byte-repeatable across three runs per backend.
- Native and WebAssembly bundles were byte-identical across all 21 fast
  scenarios for both liblecore modes.

Four fast active-worker state roots and one long active-worker state root
differ across numeric backends. This is expected because Signal deliberately
includes exact HNN trace floats in the diagnostic state root. The comparator
does not hide that difference: it reports it separately and still requires
identical legal choices, events, safety facts, cargo lineage, receipt trust,
and episode outcomes.

## Performance

These Apple arm64 measurements are observational, not a portable speed claim.
Times are median and p95 microseconds per call from 256 samples, with eight
calls per timing batch.

| Backend | Bind median / p95 | Unbind median / p95 | Action score median / p95 | Scratch | Context |
| --- | ---: | ---: | ---: | ---: | ---: |
| Signal radix-2 | 28.000 / 30.375 | 28.000 / 29.875 | 4.250 / 4.625 | 16,384 B | 0 B |
| liblecore direct | 703.500 / 718.750 | 708.125 / 726.875 | 12.375 / 13.750 | 4,096 B | 4,224 B |
| liblecore radix-2 | 16.125 / 17.875 | 15.875 / 17.500 | 12.250 / 13.500 | 16,384 B | 24,704 B |

The direct backend is about 25 times slower for bind/unbind, as expected for
the quadratic reference implementation. The liblecore radix-2 backend is
faster for bind/unbind but slower for nine-action cleanup because it computes
full cosine normalization while Signal can use a dot product for known unit
vectors.

Fast replay p95 was 2.405 seconds for Signal, 2.705 seconds for direct, and
2.403 seconds for liblecore radix-2. Long replay p95 was 24.727, 24.749, and
24.767 seconds respectively. The optimized liblecore mode is within the 10%
runtime gate on both sets.

## Reproduce

Build one replay binary per backend and the benchmark target, then run:

```sh
python3 scripts/run_liblecore_pilot.py \
  --benchmark build-hnn-pilot/signal_hnn_pilot_benchmark \
  --builtin build-builtin/signal_replay \
  --direct build-direct/signal_replay \
  --radix2 build-radix2/signal_replay \
  --output-dir /tmp/signal-liblecore-evidence \
  --fast-runs 5 --long-runs 3 --samples 256
```

CI builds all three native replay variants and runs the strict cross-backend
comparison. The existing built-in native/WebAssembly and cross-runner gates
remain unchanged.
