#!/usr/bin/env python3
"""Fail when docs assert stale code-owned constants.

`server/sim_save.c` is the source of truth for SAVE_VERSION and
MIN_SAVE_VERSION. Docs keep drifting into asserting "current format is
vN" (CLAUDE.md claimed v62 while the code was at v75), which actively
misleads readers and AI agents that treat those docs as context.

Rule: any prose claim of the form "current format is vN" /
"current save format is vN" in a scanned doc must equal the live
SAVE_VERSION, and any "minimum accepted (version) vM" claim must equal
MIN_SAVE_VERSION. Historical migration notes ("v62 expands the ledger…")
are fine — they don't assert currency and aren't matched.

`shared/cell_stress.h` likewise owns the V1 cell-damage thresholds. The
specific V1 table in `docs/cell-damage-balance.md` is intentionally
checked while historical prose elsewhere remains unrestricted.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SIM_SAVE = ROOT / "server" / "sim_save.c"
CELL_STRESS = ROOT / "shared" / "cell_stress.h"
CELL_DAMAGE_DOC = ROOT / "docs" / "cell-damage-balance.md"

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


def _enum_constant(source: str, name: str) -> int:
    m = re.search(rf"^\s*{name}\s*=\s*(\d+)\s*,", source, re.MULTILINE)
    if not m:
        raise ValueError(f"could not find {name} in {CELL_STRESS}")
    return int(m.group(1))


def _table_rows(source: str) -> dict[str, tuple[int, list[str]]]:
    rows = {}
    for lineno, line in enumerate(source.splitlines(), start=1):
        if not line.lstrip().startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) == 3:
            rows[cells[0]] = (lineno, cells)
    return rows


def cell_stress_failures(header: str, doc: str) -> list[str]:
    """Return line-specific mismatches for the current V1 threshold table."""
    try:
        expected = {
            "triangle": _enum_constant(
                header, "CELL_STRESS_TRIANGLE_FAILURE"),
            "standard": _enum_constant(
                header, "CELL_STRESS_STANDARD_FAILURE"),
            "hub_stage": _enum_constant(header, "CELL_STRESS_HUB_STAGE"),
            "hub_failure": _enum_constant(
                header, "CELL_STRESS_HUB_FAILURE"),
        }
    except ValueError as exc:
        return [str(exc)]

    failures = []
    rows = _table_rows(doc)
    required_rows = {
        "Directional triangle mount": "triangle",
        "Standard complete-edge weld": "standard",
    }
    for label, constant in required_rows.items():
        row = rows.get(label)
        if row is None:
            failures.append(
                f"{CELL_DAMAGE_DOC.relative_to(ROOT)}: missing V1 table row "
                f"'{label}'")
            continue
        lineno, cells = row
        if not cells[2].isdigit() or int(cells[2]) != expected[constant]:
            failures.append(
                f"{CELL_DAMAGE_DOC.relative_to(ROOT)}:{lineno}: '{label}' "
                f"threshold is {cells[2]!r}, but {constant} is "
                f"{expected[constant]}")

    hub_label = "Reinforced hub spoke/weld"
    hub_row = rows.get(hub_label)
    if hub_row is None:
        failures.append(
            f"{CELL_DAMAGE_DOC.relative_to(ROOT)}: missing V1 table row "
            f"'{hub_label}'")
        return failures

    lineno, cells = hub_row
    stage_values = [
        int(value) for value in re.findall(
            r"\d+", cells[1].split("fails", maxsplit=1)[0])
    ]
    expected_stages = [expected["hub_stage"], expected["hub_stage"] * 2]
    if stage_values != expected_stages:
        failures.append(
            f"{CELL_DAMAGE_DOC.relative_to(ROOT)}:{lineno}: hub stages are "
            f"{stage_values}, but CELL_STRESS_HUB_STAGE requires "
            f"{expected_stages}")
    if (not cells[2].isdigit()
            or int(cells[2]) != expected["hub_failure"]):
        failures.append(
            f"{CELL_DAMAGE_DOC.relative_to(ROOT)}:{lineno}: hub failure "
            f"threshold is {cells[2]!r}, but CELL_STRESS_HUB_FAILURE is "
            f"{expected['hub_failure']}")
    return failures


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

    failures.extend(cell_stress_failures(
        CELL_STRESS.read_text(encoding="utf-8"),
        CELL_DAMAGE_DOC.read_text(encoding="utf-8"),
    ))

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        print("doc freshness check FAILED — update code and its checked "
              "documentation together", file=sys.stderr)
        return 1

    print(f"doc freshness check passed "
          f"(SAVE_VERSION={save_version}, "
          f"MIN_SAVE_VERSION={min_save_version}; "
          f"cell thresholds synchronized)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
