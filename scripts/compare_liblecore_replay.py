#!/usr/bin/env python3
"""Compare built-in and liblecore replay bundles without hiding authority drift."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPORT_SCHEMA = "signal.liblecore_replay_comparison.v1"
EXPECTED_BACKENDS = {
    "builtin": "builtin-radix2",
    "direct": "lecore-direct",
    "radix2": "lecore-radix2",
}
AUTHORITY_FIELDS = (
    "authority",
    "event_hash",
    "receipt_trust",
    "outcome_facts",
    "start_hull",
    "end_hull",
    "hull_loss",
    "start_cargo",
    "end_cargo",
    "start_balance",
    "end_balance",
    "end_manifest_count",
    "damage_events",
    "death_events",
    "dock_events",
    "launch_events",
    "pickup_events",
    "buy_events",
    "sell_events",
    "repair_events",
    "fracture_events",
    "outpost_placed_events",
)
SCORE_KEYS = {
    "candidate_score",
    "top_score",
    "top_allowed_score",
    "score",
    "route_similarity",
    "trace_fidelity",
    "fidelity_estimate",
}
MARGIN_KEYS = {"margin", "allowed_margin", "last_margin"}


class ComparisonError(RuntimeError):
    pass


@dataclass(frozen=True)
class Bundle:
    root: Path
    manifest: dict[str, Any]
    scenarios: tuple[tuple[dict[str, Any], ...], ...]
    outcomes: bytes


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_bundle(root: Path) -> Bundle:
    root = root.resolve()
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file():
        raise ComparisonError(f"missing replay manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = manifest.get("scenarios")
    if not isinstance(entries, list) or not entries:
        raise ComparisonError(f"manifest has no scenarios: {manifest_path}")
    scenarios: list[tuple[dict[str, Any], ...]] = []
    for expected_index, entry in enumerate(entries):
        if not isinstance(entry, dict) or entry.get("index") != expected_index:
            raise ComparisonError(f"bad scenario index {expected_index}: {manifest_path}")
        path = root / str(entry.get("file", ""))
        if not path.is_file():
            raise ComparisonError(f"missing replay scenario: {path}")
        data = path.read_bytes()
        if entry.get("sha256") != sha256(data) or entry.get("size") != len(data):
            raise ComparisonError(f"scenario hash or size mismatch: {path}")
        rows = tuple(
            json.loads(line)
            for line in data.decode("utf-8").splitlines()
            if line.strip()
        )
        if not rows:
            raise ComparisonError(f"empty replay scenario: {path}")
        scenarios.append(rows)
    outcomes_entry = manifest.get("outcomes")
    if not isinstance(outcomes_entry, dict):
        raise ComparisonError(f"manifest has no outcome report: {manifest_path}")
    outcomes_path = root / str(outcomes_entry.get("file", ""))
    if not outcomes_path.is_file():
        raise ComparisonError(f"missing outcome report: {outcomes_path}")
    outcomes = outcomes_path.read_bytes()
    if (outcomes_entry.get("sha256") != sha256(outcomes) or
            outcomes_entry.get("size") != len(outcomes)):
        raise ComparisonError(f"outcome hash or size mismatch: {outcomes_path}")
    return Bundle(root, manifest, tuple(scenarios), outcomes)


def row_identity(row: dict[str, Any]) -> tuple[Any, ...]:
    evaluation = row.get("evaluation", {})
    return (
        row.get("schema"),
        row.get("seed"),
        row.get("station"),
        row.get("provenance_script"),
        row.get("prefix_ticks"),
        row.get("horizon_ticks"),
        row.get("candidate"),
        row.get("candidate_name"),
        evaluation.get("scenario") if isinstance(evaluation, dict) else None,
    )


def collect_numeric_deltas(
    left: Any,
    right: Any,
    score_deltas: list[float],
    margin_deltas: list[float],
    key: str = "",
) -> None:
    if isinstance(left, dict) and isinstance(right, dict):
        if left.keys() != right.keys():
            return
        for child_key in left:
            collect_numeric_deltas(
                left[child_key],
                right[child_key],
                score_deltas,
                margin_deltas,
                child_key,
            )
        return
    if isinstance(left, list) and isinstance(right, list):
        for left_item, right_item in zip(left, right):
            collect_numeric_deltas(
                left_item, right_item, score_deltas, margin_deltas, key
            )
        return
    if isinstance(left, bool) or isinstance(right, bool):
        return
    if (isinstance(left, (int, float)) and isinstance(right, (int, float)) and
            math.isfinite(float(left)) and math.isfinite(float(right))):
        delta = abs(float(left) - float(right))
        if key in SCORE_KEYS:
            score_deltas.append(delta)
        if key in MARGIN_KEYS:
            margin_deltas.append(delta)


def compare_backend(
    reference: Bundle,
    candidate: Bundle,
    label: str,
    tolerance: float,
    require_hnn: bool,
) -> tuple[dict[str, Any], list[str]]:
    failures: list[str] = []
    reference_entries = reference.manifest["scenarios"]
    candidate_entries = candidate.manifest["scenarios"]
    if reference.manifest.get("scenario_set") != candidate.manifest.get("scenario_set"):
        failures.append(f"{label}: scenario set differs")
    if len(reference_entries) != len(candidate_entries):
        failures.append(f"{label}: scenario count differs")
    score_deltas: list[float] = []
    margin_deltas: list[float] = []
    total_rows = 0
    hnn_rows = 0
    raw_action_agreements = 0
    legal_action_agreements = 0
    legal_rank_agreements = 0
    authority_agreements = 0
    state_root_agreements = 0
    checked_scenarios = min(len(reference.scenarios), len(candidate.scenarios))
    for scenario_index in range(checked_scenarios):
        ref_entry = reference_entries[scenario_index]
        candidate_entry = candidate_entries[scenario_index]
        if ref_entry.get("args") != candidate_entry.get("args"):
            failures.append(f"{label}: scenario {scenario_index} arguments differ")
        ref_rows = reference.scenarios[scenario_index]
        candidate_rows = candidate.scenarios[scenario_index]
        if len(ref_rows) != len(candidate_rows):
            failures.append(f"{label}: scenario {scenario_index} row count differs")
        for row_index, (ref_row, candidate_row) in enumerate(
            zip(ref_rows, candidate_rows)
        ):
            total_rows += 1
            location = f"{label}: scenario {scenario_index} row {row_index}"
            if row_identity(ref_row) != row_identity(candidate_row):
                failures.append(f"{location} identity differs")
                continue
            reference_backend = ref_row.get("hnn_backend", {})
            if reference_backend.get("active_backend") != EXPECTED_BACKENDS["builtin"]:
                failures.append(f"{location} reference reports the wrong backend")
            backend = candidate_row.get("hnn_backend", {})
            if backend.get("active_backend") != EXPECTED_BACKENDS[label]:
                failures.append(f"{location} reports the wrong backend")
            authority_equal = all(
                ref_row.get(field) == candidate_row.get(field)
                for field in AUTHORITY_FIELDS
            )
            if authority_equal:
                authority_agreements += 1
            else:
                failures.append(f"{location} changed safety or authority output")
            if ref_row.get("state_root") == candidate_row.get("state_root"):
                state_root_agreements += 1
            ref_hnn = ref_row.get("hnn")
            candidate_hnn = candidate_row.get("hnn")
            if (ref_hnn is None) != (candidate_hnn is None):
                failures.append(f"{location} HNN trace presence differs")
                continue
            if not isinstance(ref_hnn, dict) or not isinstance(candidate_hnn, dict):
                continue
            hnn_rows += 1
            if ref_hnn.keys() != candidate_hnn.keys():
                failures.append(f"{location} HNN object fields differ")
            if ref_hnn.get("top_action") == candidate_hnn.get("top_action"):
                raw_action_agreements += 1
            else:
                failures.append(f"{location} raw top action differs")
            if (ref_hnn.get("top_allowed_action") ==
                    candidate_hnn.get("top_allowed_action")):
                legal_action_agreements += 1
            else:
                failures.append(f"{location} legal top action differs")
            if (ref_hnn.get("candidate_allowed_rank") ==
                    candidate_hnn.get("candidate_allowed_rank")):
                legal_rank_agreements += 1
            else:
                failures.append(f"{location} legal candidate rank differs")
            if ref_hnn.get("allowed_mask") != candidate_hnn.get("allowed_mask"):
                failures.append(f"{location} legal action mask differs")
            ref_scores = ref_hnn.get("scores")
            candidate_scores = candidate_hnn.get("scores")
            if not isinstance(ref_scores, list) or not isinstance(candidate_scores, list):
                failures.append(f"{location} action scores are missing")
            elif len(ref_scores) != len(candidate_scores):
                failures.append(f"{location} action score count differs")
            else:
                for ref_score, candidate_score in zip(ref_scores, candidate_scores):
                    if not isinstance(ref_score, dict) or not isinstance(candidate_score, dict):
                        failures.append(f"{location} action score row is malformed")
                        break
                    for field in ("index", "name", "allowed"):
                        if ref_score.get(field) != candidate_score.get(field):
                            failures.append(
                                f"{location} action score {field} differs"
                            )
                            break
            collect_numeric_deltas(
                ref_hnn, candidate_hnn, score_deltas, margin_deltas
            )
    if require_hnn and hnn_rows == 0:
        failures.append(f"{label}: corpus contains no HNN trace rows")
    max_score_delta = max(score_deltas, default=0.0)
    max_margin_delta = max(margin_deltas, default=0.0)
    if max_score_delta > tolerance:
        failures.append(
            f"{label}: score delta {max_score_delta:.9g} exceeds {tolerance:.9g}"
        )
    if max_margin_delta > tolerance:
        failures.append(
            f"{label}: margin delta {max_margin_delta:.9g} exceeds {tolerance:.9g}"
        )
    outcomes_equal = reference.outcomes == candidate.outcomes
    if not outcomes_equal:
        failures.append(f"{label}: episode outcome report differs")
    report = {
        "active_backend": EXPECTED_BACKENDS[label],
        "authority_agreement": {"agreed": authority_agreements, "total": total_rows},
        "episode_outcomes_identical": outcomes_equal,
        "hnn_rows": hnn_rows,
        "legal_action_agreement": {
            "agreed": legal_action_agreements,
            "total": hnn_rows,
        },
        "legal_rank_agreement": {
            "agreed": legal_rank_agreements,
            "total": hnn_rows,
        },
        "max_margin_delta": max_margin_delta,
        "max_score_delta": max_score_delta,
        "raw_action_agreement": {
            "agreed": raw_action_agreements,
            "total": hnn_rows,
        },
        "state_root_agreement": {
            "agreed": state_root_agreements,
            "total": total_rows,
            "note": "Informational: cognition floats are covered by state roots.",
        },
        "passed": not failures,
    }
    return report, failures


def compare_bundles(
    roots: dict[str, Path],
    tolerance: float = 1.0e-5,
    require_hnn: bool = True,
) -> tuple[dict[str, Any], list[str]]:
    if set(roots) != set(EXPECTED_BACKENDS):
        raise ComparisonError("expected builtin, direct, and radix2 bundle roots")
    if not math.isfinite(tolerance) or tolerance < 0.0:
        raise ComparisonError("tolerance must be finite and non-negative")
    bundles = {label: load_bundle(path) for label, path in roots.items()}
    reference = bundles["builtin"]
    comparisons: dict[str, Any] = {}
    failures: list[str] = []
    for label in ("direct", "radix2"):
        comparison, backend_failures = compare_backend(
            reference, bundles[label], label, tolerance, require_hnn
        )
        comparisons[label] = comparison
        failures.extend(backend_failures)
    row_count = sum(len(rows) for rows in reference.scenarios)
    report = {
        "schema": REPORT_SCHEMA,
        "tolerance": tolerance,
        "reference_backend": EXPECTED_BACKENDS["builtin"],
        "corpus": {
            "scenario_set": reference.manifest.get("scenario_set"),
            "scenarios": len(reference.scenarios),
            "rows": row_count,
        },
        "comparisons": comparisons,
        "failures": failures,
        "passed": not failures,
    }
    return report, failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare built-in, liblecore direct, and radix-2 replay bundles."
    )
    parser.add_argument("--builtin", required=True, type=Path)
    parser.add_argument("--direct", required=True, type=Path)
    parser.add_argument("--radix2", required=True, type=Path)
    parser.add_argument("--tolerance", type=float, default=1.0e-5)
    parser.add_argument("--allow-no-hnn", action="store_true")
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report, failures = compare_bundles(
            {
                "builtin": args.builtin,
                "direct": args.direct,
                "radix2": args.radix2,
            },
            tolerance=args.tolerance,
            require_hnn=not args.allow_no_hnn,
        )
    except (ComparisonError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"liblecore replay comparison failed: {exc}", file=sys.stderr)
        return 1
    output = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(
        "liblecore replay comparison passed "
        f"({report['corpus']['scenarios']} scenarios, "
        f"{report['corpus']['rows']} rows)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
