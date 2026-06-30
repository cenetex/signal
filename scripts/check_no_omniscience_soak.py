#!/usr/bin/env python3
"""Deterministic no-omniscience replay gate for active worker gossip.

The generic replay-repeatability gate proves deterministic output. This gate
adds product-shaped assertions: fresh active workers must move local memory and
HNN market traces through replay-visible state, while focused fixtures must show
HNN-backed worker specialization without relying on a global task oracle.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "signal_replay"


@dataclass(frozen=True)
class Scenario:
    name: str
    args: tuple[str, ...]
    thresholds: dict[str, int]


SCENARIOS = (
    Scenario(
        name="fresh-world-local-gossip",
        args=(
            "--seed", "6601",
            "--station", "0",
            "--horizon-ticks", "10000",
            "--candidates", "NONE",
            "--active-workers",
        ),
        thresholds={
            "active_npcs": 1,
            "station_knowledge_items": 1,
            "hnn_market_stored_total": 1,
            "station_hnn_market_versions": 1,
            "worker_motion_ticks": 1000,
            "worker_route_support_ticks": 100,
        },
    ),
    Scenario(
        name="hnn-tow-specialization",
        args=(
            "--seed", "6610",
            "--station", "0",
            "--horizon-ticks", "1",
            "--candidates", "NONE",
            "--active-workers",
            "--provenance-script", "worker-tow-hnn",
        ),
        thresholds={
            "active_npcs": 1,
            "knowledge_items_total": 1,
            "hnn_market_stored_total": 1,
            "worker_selected_rows": 1,
            "worker_hologram_rows": 1,
            "worker_tow_assignments": 1,
            "worker_hologram_tow_assignments": 1,
            "workers_towing_scaffold": 1,
            "scaffolds_towed_by_worker": 1,
            "worker_useful_outcome_ticks": 1,
        },
    ),
    Scenario(
        name="hnn-repair-specialization",
        args=(
            "--seed", "6611",
            "--station", "0",
            "--horizon-ticks", "1",
            "--candidates", "NONE",
            "--active-workers",
            "--provenance-script", "worker-repair-hnn",
        ),
        thresholds={
            "active_npcs": 1,
            "knowledge_items_total": 1,
            "hnn_market_stored_total": 1,
            "worker_selected_rows": 1,
            "worker_hologram_rows": 1,
            "worker_repair_assignments": 1,
            "worker_hologram_repair_assignments": 1,
            "worker_useful_outcome_ticks": 1,
        },
    ),
    Scenario(
        name="hnn-delivery-proof-specialization",
        args=(
            "--seed", "6612",
            "--station", "0",
            "--horizon-ticks", "1",
            "--candidates", "NONE",
            "--active-workers",
            "--provenance-script", "worker-delivery-proof-hnn",
        ),
        thresholds={
            "active_npcs": 1,
            "knowledge_items_total": 1,
            "hnn_market_stored_total": 1,
            "worker_selected_rows": 1,
            "worker_hologram_rows": 1,
            "worker_delivery_assignments": 1,
            "worker_hologram_delivery_assignments": 1,
            "npc_delivery_shipments_cleared": 1,
            "worker_useful_outcome_ticks": 1,
        },
    ),
)


def run_once(binary: Path, scenario: Scenario, out: Path) -> None:
    subprocess.run(
        [str(binary), *scenario.args, "--out", str(out)],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def rows_from(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in path.read_text().splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def max_ai(ai_rows: list[dict[str, Any]], key: str) -> int:
    if not ai_rows:
        return 0
    return max(int(ai.get(key, 0)) for ai in ai_rows)


def summarize(rows: list[dict[str, Any]]) -> dict[str, int]:
    ai_rows = [row.get("ai") for row in rows if isinstance(row.get("ai"), dict)]
    summary: dict[str, int] = {
        "rows": len(rows),
        "ai_rows": len(ai_rows),
    }
    fields = (
        "active_npcs",
        "npc_known_contracts",
        "npc_knowledge_items",
        "station_known_contracts",
        "station_knowledge_items",
        "npc_hnn_market_stored",
        "station_hnn_market_stored",
        "station_hnn_market_versions",
        "worker_selected_rows",
        "worker_hologram_rows",
        "worker_tow_assignments",
        "worker_hologram_tow_assignments",
        "workers_towing_scaffold",
        "scaffolds_towed_by_worker",
        "worker_repair_assignments",
        "worker_hologram_repair_assignments",
        "worker_delivery_assignments",
        "worker_hologram_delivery_assignments",
        "npc_delivery_shipments_cleared",
        "worker_motion_ticks",
        "worker_route_support_ticks",
        "worker_useful_outcome_ticks",
    )
    for field in fields:
        summary[field] = max_ai(ai_rows, field)
    summary["knowledge_items_total"] = (
        summary["npc_knowledge_items"] + summary["station_knowledge_items"]
    )
    summary["hnn_market_stored_total"] = (
        summary["npc_hnn_market_stored"] +
        summary["station_hnn_market_stored"]
    )
    return summary


def validate(scenario: Scenario, summary: dict[str, int]) -> list[str]:
    failures: list[str] = []
    if summary["rows"] <= 0:
        failures.append("no replay rows")
    if summary["ai_rows"] <= 0:
        failures.append("no ai memory rows")
    for key, minimum in scenario.thresholds.items():
        value = summary.get(key, 0)
        if value < minimum:
            failures.append(f"{key} {value} < {minimum}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run deterministic no-omniscience active-worker replay soaks."
    )
    parser.add_argument("binary", nargs="?", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--json", action="store_true", help="emit JSON summary")
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"signal_replay binary not found: {args.binary}")
        return 1

    summaries: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="signal-no-omniscience-") as tmp:
        tmpdir = Path(tmp)
        for scenario in SCENARIOS:
            left = tmpdir / f"{scenario.name}-a.jsonl"
            right = tmpdir / f"{scenario.name}-b.jsonl"
            run_once(args.binary, scenario, left)
            run_once(args.binary, scenario, right)
            if left.read_bytes() != right.read_bytes():
                print(f"no-omniscience scenario not deterministic: {scenario.name}")
                print(f"  left: {left}")
                print(f"  right: {right}")
                return 1
            rows = rows_from(left)
            summary = summarize(rows)
            failures = validate(scenario, summary)
            summaries.append({
                "name": scenario.name,
                "thresholds": scenario.thresholds,
                "summary": summary,
                "failures": failures,
            })
            if failures:
                print(f"no-omniscience scenario failed: {scenario.name}")
                for failure in failures:
                    print(f"  {failure}")
                print(f"  output: {left}")
                return 1

    if args.json:
        print(json.dumps({"scenarios": summaries}, indent=2))
    else:
        print(f"no-omniscience soak passed ({len(SCENARIOS)} deterministic scenarios)")
        for item in summaries:
            summary = item["summary"]
            print(
                f"  {item['name']}: "
                f"active={summary['active_npcs']} "
                f"knowledge={summary['knowledge_items_total']} "
                f"hnn_market={summary['hnn_market_stored_total']} "
                f"motion={summary['worker_motion_ticks']} "
                f"route={summary['worker_route_support_ticks']} "
                f"useful={summary['worker_useful_outcome_ticks']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
