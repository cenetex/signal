#!/usr/bin/env python3
"""Run a small runtime HNN shadow-versus-mixed safety evaluation."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any


BACKENDS = {
    "builtin": "builtin-radix2",
    "direct": "lecore-direct",
    "radix2": "lecore-radix2",
}
CASES = (
    {"name": "seeded-route", "seed": 2037, "world": "seeded-only"},
    {"name": "weak-signal-route", "seed": 4099, "world": "weak-signal"},
    {"name": "disrupted-route", "seed": 7919, "world": "route-disrupted"},
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builtin", required=True, type=Path)
    parser.add_argument("--direct", required=True, type=Path)
    parser.add_argument("--radix2", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--horizon-ticks", type=int, default=960)
    return parser.parse_args()


def run_case(binary: Path, case: dict[str, Any], mode: str, horizon: int) -> dict[str, Any]:
    env = os.environ.copy()
    env["SIGNAL_HNN_CONFIDENCE_MODE"] = mode
    with tempfile.TemporaryDirectory(prefix="signal-hnn-mixed-") as directory:
        output = Path(directory) / "row.jsonl"
        subprocess.run(
            [
                str(binary),
                "--seed",
                str(case["seed"]),
                "--evaluation-world",
                str(case["world"]),
                "--hnn-pilot",
                "--hnn-confidence-mode",
                mode,
                "--horizon-ticks",
                str(horizon),
                "--candidates",
                "NONE",
                "--out",
                str(output),
            ],
            check=True,
            env=env,
        )
        rows = [json.loads(line) for line in output.read_text().splitlines() if line]
    if len(rows) != 1:
        raise RuntimeError(f"expected one replay row, got {len(rows)}")
    return rows[0]


def check(name: str, passed: bool, detail: str) -> dict[str, Any]:
    return {"name": name, "passed": bool(passed), "detail": detail}


def pilot_checks(label: str, row: dict[str, Any], horizon: int) -> list[dict[str, Any]]:
    pilot = row["hnn_pilot"]
    gate = pilot["gate"]
    reasons = gate["reasons"]
    decisions = gate["bootstrap_teacher"] + gate["evaluated"]
    selected = gate["selected_hnn"] + gate["selected_teacher"]
    reason_total = sum(int(value) for value in reasons.values())
    lineage_ok = row["outcome_facts"]["cargo"]["lineage_integrity_preserved"]
    return [
        check(
            f"{label}: runtime gate evaluated",
            gate["evaluated"] > 0,
            f"{gate['evaluated']} post-bootstrap decisions",
        ),
        check(
            f"{label}: every pilot tick selected one controller",
            decisions == selected == horizon,
            f"decisions={decisions}, selected={selected}, horizon={horizon}",
        ),
        check(
            f"{label}: every gate result has one reason",
            reason_total == gate["evaluated"],
            f"reasons={reason_total}, evaluated={gate['evaluated']}",
        ),
        check(
            f"{label}: pilot survived without hull loss",
            not pilot["safety"]["destroyed"] and pilot["safety"]["hull_loss"] <= 0.001,
            f"destroyed={pilot['safety']['destroyed']}, hull_loss={pilot['safety']['hull_loss']}",
        ),
        check(
            f"{label}: cargo lineage stayed valid",
            bool(lineage_ok),
            f"lineage_integrity_preserved={lineage_ok}",
        ),
    ]


def compare_modes(label: str, shadow: dict[str, Any], mixed: dict[str, Any]) -> list[dict[str, Any]]:
    sp = shadow["hnn_pilot"]
    mp = mixed["hnn_pilot"]
    shadow_tick = sp["completion_tick"]
    mixed_tick = mp["completion_tick"]
    completion_ok = shadow_tick is None or (
        mixed_tick is not None and mixed_tick <= int(shadow_tick * 1.10) + 1
    )
    return [
        check(
            f"{label}: shadow never controls the ship",
            sp["gate"]["selected_hnn"] == 0,
            f"selected_hnn={sp['gate']['selected_hnn']}",
        ),
        check(
            f"{label}: mixed safety is no worse than shadow",
            not mp["safety"]["destroyed"]
            and mp["safety"]["hull_loss"] <= sp["safety"]["hull_loss"] + 0.001,
            f"shadow_loss={sp['safety']['hull_loss']}, mixed_loss={mp['safety']['hull_loss']}",
        ),
        check(
            f"{label}: mixed target completion is within 10 percent",
            completion_ok,
            f"shadow_tick={shadow_tick}, mixed_tick={mixed_tick}",
        ),
        check(
            f"{label}: mixed route progress is within 5 percent",
            mp["route"]["progress"] + 0.001 >= sp["route"]["progress"] * 0.95,
            f"shadow={sp['route']['progress']}, mixed={mp['route']['progress']}",
        ),
    ]


def compare_backends(
    case_name: str,
    mode: str,
    builtin: dict[str, Any],
    candidate: dict[str, Any],
    candidate_name: str,
) -> list[dict[str, Any]]:
    bp = builtin["hnn_pilot"]
    cp = candidate["hnn_pilot"]
    return [
        check(
            f"{case_name}/{mode}/{candidate_name}: safety agrees with builtin",
            cp["safety"]["destroyed"] == bp["safety"]["destroyed"]
            and abs(cp["safety"]["hull_loss"] - bp["safety"]["hull_loss"]) <= 0.001,
            f"builtin_loss={bp['safety']['hull_loss']}, candidate_loss={cp['safety']['hull_loss']}",
        ),
        check(
            f"{case_name}/{mode}/{candidate_name}: route progress agrees with builtin",
            abs(cp["route"]["progress"] - bp["route"]["progress"]) <= 2.0,
            f"builtin={bp['route']['progress']}, candidate={cp['route']['progress']}",
        ),
        check(
            f"{case_name}/{mode}/{candidate_name}: gate selections agree with builtin",
            cp["gate"]["selected_hnn"] == bp["gate"]["selected_hnn"]
            and cp["gate"]["selected_teacher"] == bp["gate"]["selected_teacher"],
            "selected_hnn/selected_teacher compared",
        ),
    ]


def render_markdown(report: dict[str, Any]) -> str:
    summary = report["summary"]
    if not summary["comparison_ok"]:
        conclusion = (
            "One or more safety, completion, or backend checks failed. "
            "Mixed mode should not be expanded."
        )
    elif not summary["mixed_mode_exercised"]:
        conclusion = (
            "The safety and route comparisons passed. The confidence gate did not "
            "accept any runtime flight decision, so mixed mode never took control "
            "and produced the same outcome as shadow mode. This supports keeping "
            "shadow mode as the default; it does not yet support wider mixed-mode use."
        )
    else:
        conclusion = (
            "Mixed mode selected HNN actions while preserving the limited pilot's "
            "safety, completion, and backend gates. This supports only the bounded "
            "pilot scope covered here."
        )
    lines = [
        "# Runtime HNN shadow-versus-mixed evaluation",
        "",
        "This is a limited deterministic evaluation of one holographic worker across three route conditions and all three numeric backends.",
        "",
        f"- Result: **{summary['recommendation']}**",
        f"- Checks: {summary['passed_checks']}/{summary['total_checks']} passed",
        f"- Mixed HNN actions selected: {summary['mixed_selected_hnn']} of {summary['mixed_decisions']} decisions",
        f"- Pilot deaths: {summary['pilot_deaths']}",
        f"- Pilot hull loss: {summary['pilot_hull_loss']:.3f}",
        "",
        conclusion,
        "",
        "## Cases",
        "",
        "| Backend | Case | Shadow completion | Mixed completion | Mixed HNN selections |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for result in report["results"]:
        if result["mode"] != "mixed":
            continue
        shadow = next(
            item
            for item in report["results"]
            if item["backend"] == result["backend"]
            and item["case"] == result["case"]
            and item["mode"] == "shadow"
        )
        shadow_tick = shadow["pilot"]["completion_tick"]
        mixed_tick = result["pilot"]["completion_tick"]
        lines.append(
            f"| {result['backend']} | {result['case']} | "
            f"{shadow_tick if shadow_tick is not None else '—'} | "
            f"{mixed_tick if mixed_tick is not None else '—'} | "
            f"{result['pilot']['gate']['selected_hnn']} |"
        )
    lines.extend(
        [
            "",
            "## Reproduce",
            "",
            "Run `scripts/run_hnn_mixed_evaluation.py` with the built-in, libleCore direct, and libleCore radix-2 replay binaries.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    if args.horizon_ticks < 1:
        raise SystemExit("--horizon-ticks must be positive")
    binaries = {
        "builtin": args.builtin.resolve(),
        "direct": args.direct.resolve(),
        "radix2": args.radix2.resolve(),
    }
    for binary in binaries.values():
        if not binary.is_file():
            raise SystemExit(f"missing replay binary: {binary}")

    raw: dict[tuple[str, str, str], dict[str, Any]] = {}
    results: list[dict[str, Any]] = []
    checks: list[dict[str, Any]] = []
    for backend, binary in binaries.items():
        for case in CASES:
            for mode in ("shadow", "mixed"):
                row = run_case(binary, case, mode, args.horizon_ticks)
                actual_backend = row["hnn_backend"]["active_backend"]
                if actual_backend != BACKENDS[backend]:
                    raise RuntimeError(
                        f"{backend} binary reported {actual_backend}, expected {BACKENDS[backend]}"
                    )
                key = (backend, str(case["name"]), mode)
                raw[key] = row
                results.append(
                    {
                        "backend": actual_backend,
                        "case": case["name"],
                        "seed": case["seed"],
                        "world": case["world"],
                        "mode": mode,
                        "pilot": row["hnn_pilot"],
                    }
                )
                checks.extend(
                    pilot_checks(
                        f"{actual_backend}/{case['name']}/{mode}",
                        row,
                        args.horizon_ticks,
                    )
                )

    for backend in binaries:
        for case in CASES:
            case_name = str(case["name"])
            checks.extend(
                compare_modes(
                    f"{BACKENDS[backend]}/{case_name}",
                    raw[(backend, case_name, "shadow")],
                    raw[(backend, case_name, "mixed")],
                )
            )

    for case in CASES:
        case_name = str(case["name"])
        for mode in ("shadow", "mixed"):
            for candidate in ("direct", "radix2"):
                checks.extend(
                    compare_backends(
                        case_name,
                        mode,
                        raw[("builtin", case_name, mode)],
                        raw[(candidate, case_name, mode)],
                        BACKENDS[candidate],
                    )
                )

    mixed_results = [item for item in results if item["mode"] == "mixed"]
    mixed_selected_hnn = sum(
        int(item["pilot"]["gate"]["selected_hnn"]) for item in mixed_results
    )
    mixed_decisions = sum(
        int(item["pilot"]["gate"]["evaluated"]) for item in mixed_results
    )
    pilot_deaths = sum(
        int(bool(item["pilot"]["safety"]["destroyed"])) for item in results
    )
    pilot_hull_loss = sum(
        float(item["pilot"]["safety"]["hull_loss"]) for item in results
    )
    passed_checks = sum(int(item["passed"]) for item in checks)
    comparison_ok = passed_checks == len(checks)
    if not comparison_ok:
        recommendation = "do-not-expand"
    elif mixed_selected_hnn == 0:
        recommendation = "hold-shadow-default"
    else:
        recommendation = "limited-mixed-pilot-supported"

    report = {
        "schema": "signal.hnn_mixed_evaluation.v1",
        "scope": {
            "cases": len(CASES),
            "backends": list(BACKENDS.values()),
            "modes": ["shadow", "mixed"],
            "horizon_ticks": args.horizon_ticks,
        },
        "summary": {
            "recommendation": recommendation,
            "comparison_ok": comparison_ok,
            "mixed_mode_exercised": mixed_selected_hnn > 0,
            "mixed_selected_hnn": mixed_selected_hnn,
            "mixed_decisions": mixed_decisions,
            "pilot_deaths": pilot_deaths,
            "pilot_hull_loss": pilot_hull_loss,
            "passed_checks": passed_checks,
            "total_checks": len(checks),
        },
        "results": results,
        "checks": {
            "passed": passed_checks,
            "total": len(checks),
            "failures": [item for item in checks if not item["passed"]],
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(render_markdown(report))
    print(json.dumps(report["summary"], sort_keys=True))
    return 0 if comparison_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
