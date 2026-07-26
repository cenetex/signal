#!/usr/bin/env python3
"""Versioned deterministic topology corpus for Signal AI evaluation."""

from __future__ import annotations

import copy
import json
from pathlib import Path


CORPUS_NAME = "signal-ai-eval"
CORPUS_VERSION = 1
GENERATOR_VERSION = 1
EVALUATION_SCHEMA = "signal.ai_eval_world.v1"
PERMUTATION_GROUP = "relay-slot-permutation-v1"


def _scenario(
    seed: int,
    station: int,
    world: str,
    horizon: int,
    candidates: str,
    history: str = "W,WA,D",
) -> tuple[str, ...]:
    args = [
        "--seed", str(seed),
        "--station", str(station),
        "--history", history,
        "--horizon-ticks", str(horizon),
        "--candidates", candidates,
        "--evaluation-world", world,
    ]
    return tuple(args)


AI_EVAL_FAST_SCENARIOS = (
    _scenario(64901, 0, "seeded-only", 24, "NONE,W,WA,WD"),
    _scenario(64902, 0, "seeded-sparse", 24, "NONE,W,WA,WD"),
    _scenario(64903, 4, "outpost-low", 24, "NONE,W,WA,WD"),
    _scenario(64904, 64, "outpost-mid", 24, "NONE,W,WA,WD"),
    _scenario(64905, 127, "outpost-high", 24, "NONE,W,WA,WD"),
    _scenario(64906, 4, "scarcity", 24, "NONE,W,WA,WD"),
    _scenario(64907, 4, "weak-signal", 24, "NONE,W,WA,WD"),
    _scenario(64908, 4, "route-disrupted", 24, "NONE,W,WA,WD"),
    _scenario(64909, 4, "permutation-low", 24, "NONE,W,WA,WD"),
    _scenario(64909, 127, "permutation-high", 24, "NONE,W,WA,WD"),
)


AI_EVAL_LONG_SCENARIOS = (
    _scenario(64960, 4, "scarcity", 10000, "NONE,W", "W,WA,W,WD,A,D"),
    _scenario(64961, 4, "weak-signal", 10000, "NONE,W", "W,WD,W,WA,D,A"),
    _scenario(
        64962,
        4,
        "route-disrupted",
        10000,
        "NONE,W",
        "W,W,WA,D,WD,S",
    ),
    _scenario(
        64963,
        127,
        "outpost-high",
        100000,
        "NONE",
        "W,WA,W,WD,A,D,S",
    ),
)


EXPECTED_INVARIANTS: dict[str, tuple[str, ...]] = {
    "seeded-only": (
        "active_stations=3",
        "generated_outposts=0",
    ),
    "seeded-sparse": (
        "active_stations=2",
        "generated_outposts=0",
    ),
    "outpost-low": (
        "generated_outposts=1",
        "outpost_slots=4",
        "production_graph_present",
    ),
    "outpost-mid": (
        "generated_outposts=1",
        "outpost_slots=64",
        "production_graph_present",
    ),
    "outpost-high": (
        "generated_outposts=1",
        "outpost_slots=127",
        "production_graph_present",
    ),
    "scarcity": (
        "generated_outposts=1",
        "scarce_finished_goods>=4",
    ),
    "weak-signal": (
        "generated_outposts=1",
        "weak_signal_stations>=1",
    ),
    "route-disrupted": (
        "generated_outposts=1",
        "disconnected_stations>=1",
    ),
    "permutation-low": (
        "generated_outposts=1",
        "outpost_slots=4",
        f"permutation_group={PERMUTATION_GROUP}",
    ),
    "permutation-high": (
        "generated_outposts=1",
        "outpost_slots=127",
        f"permutation_group={PERMUTATION_GROUP}",
    ),
}


def option_value(args: tuple[str, ...], option: str) -> str | None:
    try:
        index = args.index(option)
    except ValueError:
        return None
    if index + 1 >= len(args):
        return None
    return args[index + 1]


def evaluation_world(args: tuple[str, ...]) -> str:
    return option_value(args, "--evaluation-world") or "none"


def scenario_manifest(
    args: tuple[str, ...],
    index: int,
) -> dict[str, object]:
    world = evaluation_world(args)
    return {
        "corpus": CORPUS_NAME if world != "none" else "signal-replay-default",
        "corpus_version": CORPUS_VERSION,
        "expected_invariants": list(EXPECTED_INVARIANTS.get(world, ())),
        "generator_version": GENERATOR_VERSION,
        "name": world if world != "none" else f"default-{index:03d}",
        "permutation_group": (
            PERMUTATION_GROUP
            if world in ("permutation-low", "permutation-high")
            else ""
        ),
    }


def _load_rows(path: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError("replay row is not an object")
            rows.append(value)
    return rows


def validate_evaluation_output(
    path: Path,
    args: tuple[str, ...],
) -> str | None:
    world = evaluation_world(args)
    try:
        rows = _load_rows(path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        return f"invalid evaluation output: {exc}"
    if not rows:
        return "evaluation scenario produced no rows"

    baseline: dict[str, object] | None = None
    for row in rows:
        evaluation = row.get("evaluation")
        if not isinstance(evaluation, dict):
            return "evaluation scenario produced no evaluation object"
        if evaluation.get("schema") != EVALUATION_SCHEMA:
            return "evaluation scenario has an unsupported schema"
        if evaluation.get("corpus_version") != CORPUS_VERSION:
            return "evaluation scenario has the wrong corpus version"
        if evaluation.get("generator_version") != GENERATOR_VERSION:
            return "evaluation scenario has the wrong generator version"
        if evaluation.get("scenario") != world:
            return "evaluation scenario name does not match its replay input"
        if baseline is None:
            baseline = evaluation
        elif evaluation != baseline:
            return "evaluation topology summary changed between candidates"

    assert baseline is not None
    exact: dict[str, int] = {}
    minimum: dict[str, int] = {}
    slots: list[int] | None = None
    if world == "seeded-only":
        exact = {"active_stations": 3, "generated_outposts": 0}
    elif world == "seeded-sparse":
        exact = {"active_stations": 2, "generated_outposts": 0}
    elif world in ("outpost-low", "permutation-low"):
        exact = {"generated_outposts": 1}
        slots = [4]
    elif world == "outpost-mid":
        exact = {"generated_outposts": 1}
        slots = [64]
    elif world in ("outpost-high", "permutation-high"):
        exact = {"generated_outposts": 1}
        slots = [127]
    elif world == "scarcity":
        exact = {"generated_outposts": 1}
        minimum = {"scarce_finished_goods": 4}
    elif world == "weak-signal":
        exact = {"generated_outposts": 1}
        minimum = {"weak_signal_stations": 1}
    elif world == "route-disrupted":
        exact = {"generated_outposts": 1}
        minimum = {"disconnected_stations": 1}

    for field, expected in exact.items():
        if baseline.get(field) != expected:
            return f"{world} expected {field}={expected}"
    for field, expected in minimum.items():
        value = baseline.get(field)
        if not isinstance(value, int) or value < expected:
            return f"{world} expected {field}>={expected}"
    if slots is not None and baseline.get("outpost_slots") != slots:
        return f"{world} expected outpost_slots={slots}"
    if world.startswith("outpost-"):
        if int(baseline.get("production_edges", 0)) <= 0:
            return f"{world} has no production graph"
        if int(baseline.get("consumption_edges", 0)) <= 0:
            return f"{world} has no consumption graph"
    if world in ("permutation-low", "permutation-high"):
        if baseline.get("permutation_group") != PERMUTATION_GROUP:
            return f"{world} has the wrong permutation group"
    return None


def _semantic_row(row: dict[str, object]) -> dict[str, object]:
    normalized = copy.deepcopy(row)
    for field in (
        "station",
        "prefix_state_hash",
        "state_hash",
        "end_current_station",
    ):
        normalized.pop(field, None)
    evaluation = normalized.get("evaluation")
    if isinstance(evaluation, dict):
        for field in ("scenario", "outpost_slots", "topology_hash"):
            evaluation.pop(field, None)
    return normalized


def validate_permutation_pair(outputs: dict[str, Path]) -> str | None:
    low_path = outputs.get("permutation-low")
    high_path = outputs.get("permutation-high")
    if low_path is None and high_path is None:
        return None
    if low_path is None or high_path is None:
        return "evaluation corpus is missing one station-permutation pair member"
    try:
        low_rows = _load_rows(low_path)
        high_rows = _load_rows(high_path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        return f"invalid station-permutation output: {exc}"
    if len(low_rows) != len(high_rows):
        return "station-permutation pair emitted different candidate counts"

    low_by_candidate = {row.get("candidate_name"): row for row in low_rows}
    high_by_candidate = {row.get("candidate_name"): row for row in high_rows}
    if low_by_candidate.keys() != high_by_candidate.keys():
        return "station-permutation pair emitted different candidate sets"

    for candidate, low_row in low_by_candidate.items():
        high_row = high_by_candidate[candidate]
        low_eval = low_row.get("evaluation")
        high_eval = high_row.get("evaluation")
        if not isinstance(low_eval, dict) or not isinstance(high_eval, dict):
            return "station-permutation pair is missing evaluation summaries"
        if low_eval.get("topology_hash") == high_eval.get("topology_hash"):
            return "station-permutation pair did not change slot topology"
        if (
            low_eval.get("semantic_topology_hash")
            != high_eval.get("semantic_topology_hash")
        ):
            return "station-permutation pair changed semantic topology"
        if _semantic_row(low_row) != _semantic_row(high_row):
            return (
                "station-permutation pair changed semantic candidate facts "
                f"or legal outcome for {candidate}"
            )
    return None
