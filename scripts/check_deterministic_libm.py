#!/usr/bin/env python3
"""Fail on raw libm calls in files migrated to deterministic sim math."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

MIGRATED_FILES = (
    "server/sim_flight.c",
    "server/sim_physics.c",
    "server/sim_ship.c",
    "shared/laser.c",
    "shared/tractor.c",
)

BANNED = {
    "sqrtf": "use v2_len() or fixp_sqrtf()",
    "sinf": "use v2_from_angle() or fixp_sinf()",
    "cosf": "use v2_from_angle() or fixp_cosf()",
    "atan2f": "use fixp_atan2f()",
    "asinf": "use fixp_asinf()",
    "expf": "use fixp_expf()",
    "tanf": "use fixp_tanf()",
    "powf": "use fixp_powf() or checked integer math",
}

CALL_RE = re.compile(r"\b(" + "|".join(map(re.escape, sorted(BANNED))) + r")\s*\(")


def strip_line_comments(line: str, in_block: bool) -> tuple[str, bool]:
    out: list[str] = []
    i = 0
    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end == -1:
                return "".join(out), True
            i = end + 2
            in_block = False
            continue
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if line.startswith("//", i):
            break
        out.append(line[i])
        i += 1
    return "".join(out), in_block


def main() -> int:
    findings: list[tuple[str, int, str, str]] = []
    for rel in MIGRATED_FILES:
        path = ROOT / rel
        in_block_comment = False
        for lineno, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
            line, in_block_comment = strip_line_comments(raw, in_block_comment)
            for match in CALL_RE.finditer(line):
                api = match.group(1)
                findings.append((rel, lineno, api, BANNED[api]))

    if findings:
        print("Raw libm calls found in deterministic-migrated sim files:", file=sys.stderr)
        for rel, lineno, api, replacement in findings:
            print(f"  {rel}:{lineno}: {api}() is banned; {replacement}", file=sys.stderr)
        return 1

    print("deterministic libm check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
