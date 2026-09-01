# Runtime HNN shadow-versus-mixed evaluation

This is a limited deterministic evaluation of one holographic worker across three route conditions and all three numeric backends.

- Result: **hold-shadow-default**
- Checks: 162/162 passed
- Mixed HNN actions selected: 0 of 8631 decisions
- Pilot deaths: 0
- Pilot hull loss: 0.000

The safety and route comparisons passed. The confidence gate did not accept any runtime flight decision, so mixed mode never took control and produced the same outcome as shadow mode. This supports keeping shadow mode as the default; it does not yet support wider mixed-mode use.

## Cases

| Backend | Case | Shadow completion | Mixed completion | Mixed HNN selections |
| --- | --- | ---: | ---: | ---: |
| builtin-radix2 | seeded-route | 910 | 910 | 0 |
| builtin-radix2 | weak-signal-route | — | — | 0 |
| builtin-radix2 | disrupted-route | 453 | 453 | 0 |
| lecore-direct | seeded-route | 910 | 910 | 0 |
| lecore-direct | weak-signal-route | — | — | 0 |
| lecore-direct | disrupted-route | 453 | 453 | 0 |
| lecore-radix2 | seeded-route | 910 | 910 | 0 |
| lecore-radix2 | weak-signal-route | — | — | 0 |
| lecore-radix2 | disrupted-route | 453 | 453 | 0 |

## Reproduce

Run `scripts/run_hnn_mixed_evaluation.py` with the built-in, libleCore direct, and libleCore radix-2 replay binaries.
