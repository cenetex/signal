# cenetex/merkle

Single-header C Merkle Mountain Range. Drop-in for any project that
wants append-only commitments with O(log N) inclusion proofs.

- **Upstream**: https://github.com/cenetex/merkle
- **Pinned commit**: `4b821ad` (post-v1.0.0 — torn-write hardening from PR #1, dense round-trip + libFuzzer harness from PR #2)
- **License**: MIT-0 (see upstream `LICENSE`)
- **Spec**: upstream `SPEC.md` (canonical-form contract)
- **Used by**: Signal #285 — destroyed-rock ledger; the closed-epoch
  MMR root over destroyed `rock_pub`s is what gets posted on-chain
  as the rock-permanence anchor.

The vendored header is unmodified from upstream. To update: re-fetch
`include/merkle.h` from the repo at a newer pinned commit, replace
this file's commit reference, and re-run the test suite.

## Why vendor

Same pattern as `vendor/fastfilter/`: pin a commit, never edit in
place. The single-header drop-in shape is the upstream's intended
distribution; submodule churn for one file isn't worth it.

We own this primitive (Cenetex publishes `cenetex/merkle`), so the
canonical-form spec is ours to coordinate with the on-chain
verifier. That coordination is the whole reason it lives in Cenetex
namespace rather than as part of Signal — kyro, ratibot, hyperscape,
and any future Cenetex project that needs append-only commitments
hangs off the same primitive.
