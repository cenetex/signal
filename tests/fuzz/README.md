# Receipt and handoff fuzzing

Run the same headless path used by CI with a short local budget:

```sh
make fuzz-receipts FUZZ_TIME=2 FUZZ_TIMEOUT=10
```

The target configures CMake with `BUILD_TESTS_ONLY=ON`,
`BUILD_TOOLS=OFF`, and `SIGNAL_BUILD_FUZZERS=ON`, so configuration
returns before desktop ALSA, X11, or OpenGL discovery. It builds only
`fuzz_cargo_receipt` and its production decoder/crypto dependencies.

Each run has two phases:

1. Replay every tracked file in `tests/fuzz/corpus/` with `-runs=0`.
2. Explore `receipt-chain`, `receipt-store`, and `handoff` separately for
   `FUZZ_TIME` seconds each.

New coverage inputs are written below `build-fuzz/corpus/<mode>/`. Crash and
timeout artifacts are written to `tests/fuzz/artifacts/` with a mode prefix,
such as `handoff-<hash>`, so replay can select the same forced mode.

Replay the tracked corpus and any local crash artifacts without libFuzzer:

```sh
make fuzz-receipts-standalone
```

For a single mode-specific artifact, the equivalent direct command is:

```sh
SIGNAL_FUZZ_MODE=handoff \
  ./build-fuzz/fuzz_cargo_receipt_standalone \
  tests/fuzz/artifacts/handoff-<hash>
```

The tracked seed format uses its first byte modulo three to select chain,
store, or handoff mode. Do not place generated coverage inputs in the tracked
directory without minimizing and reviewing them first.
