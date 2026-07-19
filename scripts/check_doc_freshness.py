#!/usr/bin/env python3
"""Fail when docs assert a stale world.sav format version.

`server/sim_save.c` is the source of truth for SAVE_VERSION and
MIN_SAVE_VERSION. Docs keep drifting into asserting "current format is
vN" (CLAUDE.md claimed v62 while the code was at v75), which actively
misleads readers and AI agents that treat those docs as context.

Rule: any prose claim of the form "current format is vN" /
"current save format is vN" in a scanned doc must equal the live
SAVE_VERSION, and any "minimum accepted (version) vM" claim must equal
MIN_SAVE_VERSION. Historical migration notes ("v62 expands the ledger…")
are fine — they don't assert currency and aren't matched.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SIM_SAVE = ROOT / "server" / "sim_save.c"

SCANNED_DOCS = [
    ROOT / "CLAUDE.md",
    ROOT / "ARCHITECTURE.md",
    ROOT / "ENG.md",
    ROOT / "PRD.md",
    ROOT / "README.md",
    *sorted((ROOT / "docs").glob("*.md")),
]

CURRENT_RE = re.compile(
    r"current(?:\s+save)?\s+format\s+is\s+v(\d+)", re.IGNORECASE
)
MINIMUM_RE = re.compile(
    r"minimum[\s-]accepted(?:\s+version)?\s*(?:is\s*)?v(\d+)", re.IGNORECASE
)


def _define(source: str, name: str) -> int:
    m = re.search(rf"^#define\s+{name}\s+(\d+)", source, re.MULTILINE)
    if not m:
        print(f"error: could not find #define {name} in {SIM_SAVE}",
              file=sys.stderr)
        sys.exit(2)
    return int(m.group(1))


def main() -> int:
    sim_save_text = SIM_SAVE.read_text(encoding="utf-8")
    save_version = _define(sim_save_text, "SAVE_VERSION")
    min_save_version = _define(sim_save_text, "MIN_SAVE_VERSION")

    failures = []
    for doc in SCANNED_DOCS:
        if not doc.is_file():
            continue
        for lineno, line in enumerate(
                doc.read_text(encoding="utf-8").splitlines(), start=1):
            m = CURRENT_RE.search(line)
            if m and int(m.group(1)) != save_version:
                failures.append(
                    f"{doc.relative_to(ROOT)}:{lineno}: claims current save "
                    f"format is v{m.group(1)}, but SAVE_VERSION is "
                    f"{save_version}")
            m = MINIMUM_RE.search(line)
            if m and int(m.group(1)) != min_save_version:
                failures.append(
                    f"{doc.relative_to(ROOT)}:{lineno}: claims minimum "
                    f"accepted save version is v{m.group(1)}, but "
                    f"MIN_SAVE_VERSION is {min_save_version}")

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        print("doc freshness check FAILED — update the doc to point at "
              "SAVE_VERSION/MIN_SAVE_VERSION in server/sim_save.c instead "
              "of duplicating the numbers", file=sys.stderr)
        return 1

    print(f"doc freshness check passed "
          f"(SAVE_VERSION={save_version}, "
          f"MIN_SAVE_VERSION={min_save_version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
