#!/usr/bin/env python3
"""Run signal_replay twice per scenario and require byte-identical output."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "signal_replay"

FAST_SCENARIOS = (
    (
        "--seed", "2037",
        "--history", "W,W,WA,D",
        "--horizon-ticks", "36",
        "--candidates", "NONE,W,A,D,S,WA,WD,SA,SD",
    ),
    (
        "--seed", "4242",
        "--station", "1",
        "--history", "W,WD,WD,S,A",
        "--horizon-ticks", "48",
        "--candidates", "NONE,W,WD,SD",
    ),
    (
        "--seed", "9001",
        "--spawn", "1200,-1800",
        "--velocity", "12.5,-7.25",
        "--angle", "0.75",
        "--goal", "2600,-900",
        "--history", "A,W,WA,W,D",
        "--horizon-ticks", "24",
        "--candidates", "A,D,WA,WD",
    ),
    (
        "--seed", "2037",
        "--station", "0",
        "--history", "W,W,W,W,WA,WA,D,D,S,W,WD,WD",
        "--horizon-ticks", "240",
        "--candidates", "NONE,W,WA,WD",
    ),
    (
        "--seed", "5150",
        "--station", "2",
        "--spawn", "-2500,1800",
        "--velocity", "-18.0,9.0",
        "--angle", "-1.25",
        "--goal", "-4100,900",
        "--history", "S,SA,A,W,WA,W,D,WD",
        "--horizon-ticks", "180",
        "--candidates", "S,SA,SD,W",
    ),
    (
        "--seed", "6060",
        "--station", "0",
        "--history", "W,WA,W,WD,A,D",
        "--horizon-ticks", "24",
        "--candidates", "W,WA,WD,S",
        "--hnn-trace",
    ),
    (
        "--seed", "7777",
        "--station", "1",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "buy-sell",
    ),
    (
        "--seed", "8181",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "pod-tow-sell",
    ),
    (
        "--seed", "5880",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "mine-fracture",
    ),
    (
        "--seed", "5890",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "asteroid-death",
    ),
    (
        "--seed", "5900",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "planned-outpost",
    ),
    (
        "--seed", "5910",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "station-jostle",
    ),
    (
        "--seed", "5920",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "player-ram",
    ),
    (
        "--seed", "5930",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "npc-ram",
    ),
    (
        "--seed", "5940",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "thrown-rock-hit",
    ),
    (
        "--seed", "5941",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--provenance-script", "fracture-claim",
    ),
    (
        "--seed", "6601",
        "--station", "0",
        "--horizon-ticks", "2000",
        "--candidates", "NONE",
        "--active-workers",
    ),
    (
        "--seed", "6610",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--active-workers",
        "--provenance-script", "worker-tow-hnn",
    ),
    (
        "--seed", "6611",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--active-workers",
        "--provenance-script", "worker-repair-hnn",
    ),
)

LONG_SCENARIOS = (
    (
        "--seed", "588100",
        "--station", "0",
        "--spawn", "1500,-1200",
        "--velocity", "19.25,-11.75",
        "--angle", "0.37",
        "--goal", "7800,2400",
        "--history", "W,WA,W,WD,W,A,D,S,WA,WD",
        "--horizon-ticks", "10000",
        "--candidates", "WA",
    ),
    (
        "--seed", "588101",
        "--station", "2",
        "--spawn", "-3200,2100",
        "--velocity", "-7.5,14.0",
        "--angle", "-2.10",
        "--goal", "-8400,-1800",
        "--history", "W,W,WD,WD,A,W,SA,D",
        "--horizon-ticks", "10000",
        "--candidates", "WD",
    ),
    (
        "--seed", "588102",
        "--station", "1",
        "--history", "W,WA,W,WA,D,W,WD,S,W,A",
        "--horizon-ticks", "100000",
        "--candidates", "W",
    ),
    (
        "--seed", "6601",
        "--station", "0",
        "--horizon-ticks", "10000",
        "--candidates", "NONE",
        "--active-workers",
    ),
)

SCENARIO_SETS = {
    "fast": FAST_SCENARIOS,
    "long": LONG_SCENARIOS,
    "all": FAST_SCENARIOS + LONG_SCENARIOS,
}


def scenarios_for_name(name: str) -> tuple[tuple[str, ...], ...] | None:
    return SCENARIO_SETS.get(name)


def run_once(binary: Path, args: tuple[str, ...], out: Path) -> None:
    subprocess.run(
        [str(binary), *args, "--out", str(out)],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def validate_active_worker_output(
    path: Path,
    require_worker_tow: bool = False,
    require_worker_repair: bool = False,
) -> str | None:
    rows = []
    for line in path.read_text().splitlines():
        if line.strip():
            rows.append(json.loads(line))
    if not rows:
        return "active-worker scenario produced no rows"

    ai_rows = [row.get("ai") for row in rows if isinstance(row.get("ai"), dict)]
    if not ai_rows:
        return "active-worker scenario produced no ai object"
    if max(int(ai.get("active_npcs", 0)) for ai in ai_rows) <= 0:
        return "active-worker scenario had no active NPCs"

    knowledge = max(
        int(ai.get("npc_knowledge_items", 0)) +
        int(ai.get("station_knowledge_items", 0))
        for ai in ai_rows
    )
    if knowledge <= 0:
        return "active-worker scenario had no exchanged knowledge"

    hnn_market = max(
        int(ai.get("npc_hnn_market_stored", 0)) +
        int(ai.get("station_hnn_market_stored", 0))
        for ai in ai_rows
    )
    if hnn_market <= 0:
        return "active-worker scenario had no HNN market memory"

    activity_fields = (
        "worker_selected_rows",
        "workers_travel_to_pickup",
        "workers_travel_to_dest",
        "workers_unloading",
        "workers_returning",
        "workers_towing_scaffold",
        "workers_with_finished_cargo",
        "npc_delivery_shipments_active",
        "npc_delivery_shipments_picked_up",
        "npc_delivery_shipments_delivered",
        "npc_delivery_shipments_cleared",
        "npc_delivery_shipments_defaulted",
        "npc_delivery_shipments_black_market_sold",
    )
    activity = max(
        sum(int(ai.get(field, 0)) for field in activity_fields)
        for ai in ai_rows
    )
    if activity <= 0:
        return "active-worker scenario had no worker activity counters"

    if require_worker_tow or require_worker_repair:
        selected = max(int(ai.get("worker_selected_rows", 0)) for ai in ai_rows)
        if selected <= 0:
            return "active-worker scenario had no selected worker rows"
        hologram = max(int(ai.get("worker_hologram_rows", 0)) for ai in ai_rows)
        if hologram <= 0:
            return "active-worker scenario had no hologram-backed worker rows"

    if require_worker_tow:
        tow = max(int(ai.get("worker_tow_assignments", 0)) for ai in ai_rows)
        if tow <= 0:
            return "active-worker scenario had no selected tow assignments"
        hnn_tow = max(
            int(ai.get("worker_hologram_tow_assignments", 0))
            for ai in ai_rows
        )
        if hnn_tow <= 0:
            return "active-worker scenario had no HNN-backed tow assignments"
        towing = max(
            int(ai.get("workers_towing_scaffold", 0)) +
            int(ai.get("scaffolds_towed_by_worker", 0))
            for ai in ai_rows
        )
        if towing <= 0:
            return "active-worker scenario had no worker scaffold pickup"

    if require_worker_repair:
        repair = max(int(ai.get("worker_repair_assignments", 0)) for ai in ai_rows)
        if repair <= 0:
            return "active-worker scenario had no selected repair assignments"
        hnn_repair = max(
            int(ai.get("worker_hologram_repair_assignments", 0))
            for ai in ai_rows
        )
        if hnn_repair <= 0:
            return "active-worker scenario had no HNN-backed repair assignments"

    return None


def main() -> int:
    scenario_set_name = "fast"
    args = sys.argv[1:]
    if "--scenario-set" in args:
        idx = args.index("--scenario-set")
        if idx + 1 >= len(args):
            print("--scenario-set requires a value", file=sys.stderr)
            return 2
        scenario_set_name = args[idx + 1]
        del args[idx:idx + 2]

    if len(args) > 1:
        print(
            "usage: check_replay_repeatability.py [SIGNAL_REPLAY] "
            "[--scenario-set fast|long|all]",
            file=sys.stderr,
        )
        return 2

    scenarios = scenarios_for_name(scenario_set_name)
    if scenarios is None:
        print(f"unknown scenario set: {scenario_set_name}", file=sys.stderr)
        return 2

    binary = Path(args[0]) if args else DEFAULT_BINARY
    if not binary.exists():
        print(f"signal_replay binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="signal-replay-repeat-") as tmp:
        tmpdir = Path(tmp)
        for i, scenario_args in enumerate(scenarios):
            left = tmpdir / f"scenario-{i}-a.jsonl"
            right = tmpdir / f"scenario-{i}-b.jsonl"
            run_once(binary, scenario_args, left)
            run_once(binary, scenario_args, right)
            left_bytes = left.read_bytes()
            right_bytes = right.read_bytes()
            if left_bytes != right_bytes:
                print(f"signal_replay scenario {i} was not repeatable", file=sys.stderr)
                print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                print(f"  left: {left}", file=sys.stderr)
                print(f"  right: {right}", file=sys.stderr)
                return 1
            if not left_bytes:
                print(f"signal_replay scenario {i} produced no output", file=sys.stderr)
                return 1
            if "--active-workers" in scenario_args:
                failure = validate_active_worker_output(
                    left,
                    require_worker_tow="worker-tow-hnn" in scenario_args,
                    require_worker_repair="worker-repair-hnn" in scenario_args,
                )
                if failure is not None:
                    print(f"signal_replay scenario {i} failed AI coverage: {failure}", file=sys.stderr)
                    print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                    print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                    print(f"  output: {left}", file=sys.stderr)
                    return 1

    print(
        f"signal replay repeatability check passed "
        f"({len(scenarios)} {scenario_set_name} scenarios)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
