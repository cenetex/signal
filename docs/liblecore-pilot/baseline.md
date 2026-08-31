# Signal HNN baseline

This baseline was captured from Signal commit `7c14c7b` before selecting a
liblecore backend. Signal's built-in normalized radix-2 implementation remains
the active backend.

## Environment

- Host: Apple arm64, Darwin 25.3.0
- Compiler: Apple Clang 17.0.0
- CMake: 4.1.2
- Build: `RelWithDebInfo`, contraction disabled, fast-math disabled
- Replay corpus: `fast`

## Results

- HNN-focused native tests: 18/18 passed.
- Fast replay repeatability: 21/21 scenarios passed.
- The representative HNN replay produced the same SHA-256 on five runs:
  `d0e147c9ee5261d1f3a38a097708a7d5c0e486266ffde08a938b4ccbf2f1533f`.
- Representative HNN replay command: seed 6060, prefix `W,WA,W,WD,A,D`,
  24-tick horizon, candidates `W,WA,WD,S`, HNN trace enabled.
- Wall time: 0.36 seconds cold; four warm runs were 0.16 seconds each.

These coarse replay timings are observational and host-specific. The replay
evidence PR adds the shared primitive/action benchmark and compares built-in,
liblecore direct, and liblecore radix-2 on the same host and corpus.
