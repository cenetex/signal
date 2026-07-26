#!/usr/bin/env python3
"""Compare teacher, shadow, mixed, and active AI episode outcome bundles."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from ai_episode_outcomes import (
    DECISION_MODES,
    HEADS,
    OUTCOME_REPORT_VERSION,
    STATUSES,
    OutcomeReportError,
    load_episode_report,
)
from check_replay_bundles import BUNDLE_SCHEMA, load_bundle


COMPARISON_SCHEMA = "signal.ai_episode_comparison.v1"
COMPARISON_VERSION = 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare deterministic AI episode outcomes across the four "
            "decision modes."
        )
    )
    parser.add_argument(
        "bundles",
        nargs="+",
        metavar="MODE=PATH",
        help="one bundle for each of teacher, shadow, mixed, and active",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit the machine-readable comparison report",
    )
    return parser.parse_args()


def parse_bundle_specs(specs: list[str]) -> dict[str, Path]:
    bundles: dict[str, Path] = {}
    for spec in specs:
        if "=" not in spec:
            raise OutcomeReportError(f"bundle must use MODE=PATH syntax: {spec!r}")
        mode, raw_path = spec.split("=", 1)
        if mode not in DECISION_MODES:
            raise OutcomeReportError(f"unsupported decision mode: {mode!r}")
        if mode in bundles:
            raise OutcomeReportError(f"duplicate bundle for mode: {mode}")
        if not raw_path:
            raise OutcomeReportError(f"bundle path is empty for mode: {mode}")
        bundles[mode] = Path(raw_path).resolve()
    missing = [mode for mode in DECISION_MODES if mode not in bundles]
    if missing:
        raise OutcomeReportError(
            f"missing decision-mode bundles: {', '.join(missing)}"
        )
    return bundles


def load_mode_bundle(
    mode: str,
    path: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    loaded = load_bundle(path)
    if loaded is None:
        raise OutcomeReportError(f"invalid replay bundle for {mode}: {path}")
    manifest, _outputs = loaded
    if manifest.get("schema") != BUNDLE_SCHEMA:
        raise OutcomeReportError(f"{mode} bundle has unsupported schema")
    if manifest.get("decision_mode") != mode:
        raise OutcomeReportError(
            f"{mode} bundle declares {manifest.get('decision_mode')!r}"
        )
    outcome_entry = manifest["outcomes"]
    report = load_episode_report(
        path / outcome_entry["file"],
        expected_mode=mode,
    )
    return manifest, report


def _episode_identity(episode: dict[str, Any]) -> tuple[object, ...]:
    return (
        episode["episode_id"],
        episode["replay_id"],
        episode["scenario_index"],
        episode["scenario"],
        episode["head"],
        episode["candidate"],
        episode["candidate_name"],
    )


def _round(value: float) -> float:
    return round(value, 6)


def summarize_head(episodes: list[dict[str, Any]]) -> dict[str, Any]:
    status_counts = {status: 0 for status in STATUSES}
    route_efficiencies: list[float] = []
    ticks_to_completion: list[int] = []
    totals = {
        "collision_events": 0,
        "death_events": 0,
        "stuck_ticks": 0,
        "recovery_events": 0,
        "safety_overrides": 0,
        "repeated_loops": 0,
        "station_need_served": 0,
        "behavioral_diversity": 0,
        "provenance_failures": 0,
        "invalid_receipt_chains": 0,
        "missing_receipt_chains": 0,
    }
    for episode in episodes:
        status_counts[episode["status"]] += 1
        if episode["status"] == "not_attempted":
            continue
        metrics = episode["metrics"]
        route_efficiencies.append(float(metrics["route_efficiency"]))
        if episode["ticks_to_completion"] is not None:
            ticks_to_completion.append(int(episode["ticks_to_completion"]))
        for field in totals:
            if field == "provenance_failures":
                totals[field] += int(not metrics["provenance_preserved"])
            else:
                totals[field] += int(metrics[field])

    attempted = len(episodes) - status_counts["not_attempted"]
    completed = status_counts["completed"]
    completion_rate = completed / attempted if attempted else None
    return {
        "episodes": len(episodes),
        "attempted": attempted,
        "status": status_counts,
        "completion_rate": (
            _round(completion_rate) if completion_rate is not None else None
        ),
        "average_route_efficiency": (
            _round(sum(route_efficiencies) / len(route_efficiencies))
            if route_efficiencies
            else None
        ),
        "average_ticks_to_completion": (
            _round(sum(ticks_to_completion) / len(ticks_to_completion))
            if ticks_to_completion
            else None
        ),
        **totals,
    }


def build_comparison(
    bundles: dict[str, Path],
    manifests: dict[str, dict[str, Any]],
    reports: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    baseline_mode = DECISION_MODES[0]
    baseline_manifest = manifests[baseline_mode]
    baseline_report = reports[baseline_mode]
    metadata_fields = ("corpus", "corpus_version", "generator_version")
    for mode in DECISION_MODES[1:]:
        for field in metadata_fields:
            if reports[mode].get(field) != baseline_report.get(field):
                raise OutcomeReportError(
                    f"{mode} report {field} differs from {baseline_mode}"
                )

    baseline_episodes = {
        episode["episode_id"]: episode
        for episode in baseline_report["episodes"]
    }
    baseline_ids = set(baseline_episodes)
    for mode in DECISION_MODES[1:]:
        candidate_episodes = {
            episode["episode_id"]: episode
            for episode in reports[mode]["episodes"]
        }
        candidate_ids = set(candidate_episodes)
        if candidate_ids != baseline_ids:
            missing = len(baseline_ids - candidate_ids)
            extra = len(candidate_ids - baseline_ids)
            raise OutcomeReportError(
                f"{mode} episode IDs differ: missing={missing} extra={extra}"
            )
        for episode_id, baseline_episode in baseline_episodes.items():
            if (
                _episode_identity(candidate_episodes[episode_id])
                != _episode_identity(baseline_episode)
            ):
                raise OutcomeReportError(
                    f"{mode} semantic identity differs for episode {episode_id}"
                )

    modes: dict[str, Any] = {}
    for mode in DECISION_MODES:
        episodes = reports[mode]["episodes"]
        modes[mode] = {
            "bundle": str(bundles[mode]),
            "report_id": reports[mode]["report_id"],
            "heads": {
                head: summarize_head(
                    [episode for episode in episodes if episode["head"] == head]
                )
                for head in HEADS
            },
        }

    core = {
        "schema": COMPARISON_SCHEMA,
        "comparison_version": COMPARISON_VERSION,
        "outcome_report_version": OUTCOME_REPORT_VERSION,
        "corpus": baseline_report["corpus"],
        "corpus_version": baseline_report["corpus_version"],
        "generator_version": baseline_report["generator_version"],
        "scenario_set": baseline_manifest["scenario_set"],
        "episode_count_per_mode": len(baseline_ids),
        "shared_episode_ids": True,
        "modes": modes,
    }
    digest = hashlib.sha256(
        json.dumps(core, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    core["comparison_id"] = digest
    return core


def print_text(comparison: dict[str, Any]) -> None:
    print(
        f"schema={comparison['schema']} "
        f"corpus={comparison['corpus']} "
        f"episodes_per_mode={comparison['episode_count_per_mode']} "
        f"shared_ids={str(comparison['shared_episode_ids']).lower()}"
    )
    for mode in DECISION_MODES:
        for head in HEADS:
            summary = comparison["modes"][mode]["heads"][head]
            status = summary["status"]
            print(
                f"{mode}/{head}: attempted={summary['attempted']} "
                f"completed={status['completed']} failed={status['failed']} "
                f"in_progress={status['in_progress']} "
                f"not_attempted={status['not_attempted']} "
                f"collisions={summary['collision_events']} "
                f"stuck={summary['stuck_ticks']} "
                f"provenance_failures={summary['provenance_failures']} "
                f"route_efficiency={summary['average_route_efficiency']}"
            )


def main() -> int:
    args = parse_args()
    try:
        bundle_paths = parse_bundle_specs(args.bundles)
        manifests: dict[str, dict[str, Any]] = {}
        reports: dict[str, dict[str, Any]] = {}
        for mode in DECISION_MODES:
            manifests[mode], reports[mode] = load_mode_bundle(
                mode,
                bundle_paths[mode],
            )
        comparison = build_comparison(bundle_paths, manifests, reports)
    except OutcomeReportError as exc:
        print(f"AI episode outcome comparison failed: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(comparison, indent=2, sort_keys=True))
    else:
        print_text(comparison)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
