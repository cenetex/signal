# Binary Fuse filter (xor_singleheader)

Single-header C11 implementation of XOR and Binary Fuse filters by
Daniel Lemire, Thomas Mueller Graf, and contributors.

- **Upstream**: https://github.com/FastFilter/xor_singleheader
- **Pinned commit**: `c482686f623fb87ebfb20f0a6c4ffd428c4447ad` (master @ 2026-04-29)
- **License**: Apache License 2.0 (see LICENSE file in upstream repo)
- **Used by**: Signal #285 — destroyed-rock ledger (Tier 1 in-memory
  membership filter for permanent floating terrain). Closed-epoch
  filters are constructed deterministically with the epoch number as
  the seed so two operators independently building the same filter
  from the same key set produce bit-identical output. The hash of
  that filter is what gets posted on-chain in slice 3.

The vendored header is unmodified from upstream. Updates: re-fetch
from the same URL pinned to a newer commit and update this file.

## Why vendored, not submoduled

Single-header drop-in is the upstream's intended distribution shape
(it's the `_singleheader` repo). Submodule churn for one file isn't
worth it.

## Why this filter family, not Bloom

- **Deterministic construction**: same key set + same seed ⇒
  bit-identical filter output on every machine. On-chain commitments
  can be 32-byte hashes any verifier recomputes.
- **~9 bits/element** at 1/256 false-positive rate (vs. ~11.5 for
  optimized Bloom).
- **~3 memory accesses per query** (vs. ~8 for Bloom at matched FP).
- **Build-once + immutable**: ideal for closed-epoch snapshots that
  get committed on-chain.

The trade-off is no inserts after construction — we rebuild from
scratch at each epoch close, which is exactly the lifecycle the
on-chain anchoring story wants.
