#!/usr/bin/env python3
"""Build and validate deterministic per-head AI episode outcome reports."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any, Iterable


OUTCOME_FACTS_SCHEMA = "signal.ai_outcome_facts.v1"
OUTCOME_REPORT_SCHEMA = "signal.ai_episode_report.v1"
OUTCOME_EPISODE_SCHEMA = "signal.ai_episode_outcome.v1"
OUTCOME_REPORT_VERSION = 1
OUTCOME_CORPUS_NAME = "signal-ai-outcomes"

DECISION_MODES = ("teacher", "shadow", "mixed", "active")
HEADS = ("flight", "contract", "worker")
STATUSES = ("not_attempted", "in_progress", "completed", "failed")

FEATURE_CONTRACTS = {
    "flight": ("signal-flight-live-v2", 2),
    "contract": ("signal-contract-live-v2", 2),
    "worker": ("signal-npc-worker-v2", 2),
}

AI_OUTCOME_FAST_SCENARIOS = (
    (
        "--seed", "65101",
        "--station", "0",
        "--history", "W,WA,D",
        "--horizon-ticks", "120",
        "--candidates", "NONE,W,WA,WD",
        "--evaluation-world", "seeded-only",
        "--hnn-trace",
    ),
    (
        "--seed", "65102",
        "--station", "127",
        "--history", "W,WD,A",
        "--horizon-ticks", "120",
        "--candidates", "NONE,W,WA,WD",
        "--evaluation-world", "outpost-high",
        "--hnn-trace",
    ),
    (
        "--seed", "65103",
        "--station", "4",
        "--history", "W,WA,D",
        "--horizon-ticks", "240",
        "--candidates", "NONE",
        "--evaluation-world", "scarcity",
    ),
    (
        "--seed", "65105",
        "--spawn", "1000,-1000",
        "--goal", "1000,-1000",
        "--horizon-ticks", "8",
        "--candidates", "NONE,W",
        "--evaluation-world", "seeded-only",
    ),
    (
        "--seed", "6610",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--active-workers",
        "--evaluation-world", "seeded-only",
        "--provenance-script", "worker-tow-hnn",
    ),
    (
        "--seed", "6611",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--active-workers",
        "--evaluation-world", "seeded-only",
        "--provenance-script", "worker-repair-hnn",
    ),
    (
        "--seed", "6612",
        "--station", "0",
        "--horizon-ticks", "1",
        "--candidates", "NONE",
        "--active-workers",
        "--evaluation-world", "seeded-only",
        "--provenance-script", "worker-delivery-proof-hnn",
    ),
    (
        "--seed", "65104",
        "--station", "4",
        "--history", "W,WD,A",
        "--horizon-ticks", "240",
        "--candidates", "NONE",
        "--evaluation-world", "route-disrupted",
    ),
)


class OutcomeReportError(ValueError):
    """Raised when replay facts or an outcome report fail closed validation."""


def _canonical_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _stable_id(domain: str, value: object) -> str:
    digest = hashlib.sha256()
    digest.update(domain.encode("ascii"))
    digest.update(b"\0")
    digest.update(_canonical_json(value).encode("utf-8"))
    return digest.hexdigest()


def _require_dict(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OutcomeReportError(f"{label} must be an object")
    return value


def _require_list(value: object, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise OutcomeReportError(f"{label} must be an array")
    return value


def _require_str(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise OutcomeReportError(f"{label} must be a non-empty string")
    return value


def _require_bool(value: object, label: str) -> bool:
    if type(value) is not bool:
        raise OutcomeReportError(f"{label} must be boolean")
    return value


def _require_int(
    value: object,
    label: str,
    *,
    minimum: int | None = None,
    maximum: int | None = None,
) -> int:
    if type(value) is not int:
        raise OutcomeReportError(f"{label} must be an integer")
    if minimum is not None and value < minimum:
        raise OutcomeReportError(f"{label} must be >= {minimum}")
    if maximum is not None and value > maximum:
        raise OutcomeReportError(f"{label} must be <= {maximum}")
    return value


def _require_number(
    value: object,
    label: str,
    *,
    minimum: float | None = None,
) -> float:
    if type(value) not in (int, float):
        raise OutcomeReportError(f"{label} must be numeric")
    number = float(value)
    if not (-float("inf") < number < float("inf")):
        raise OutcomeReportError(f"{label} must be finite")
    if minimum is not None and number < minimum:
        raise OutcomeReportError(f"{label} must be >= {minimum}")
    return number


def _optional_tick(value: object, label: str) -> int | None:
    if value is None:
        return None
    return _require_int(value, label, minimum=0)


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise OutcomeReportError(f"cannot read replay output {path}: {exc}") from exc
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise OutcomeReportError(
                f"{path}:{line_number}: invalid JSON: {exc}"
            ) from exc
        if not isinstance(value, dict):
            raise OutcomeReportError(
                f"{path}:{line_number}: replay row must be an object"
            )
        rows.append(value)
    if not rows:
        raise OutcomeReportError(f"{path}: replay output has no rows")
    return rows


def _validate_lineage(value: object, label: str) -> dict[str, Any]:
    lineage = _require_dict(value, label)
    manifest_count = _require_int(
        lineage.get("manifest_count"),
        f"{label}.manifest_count",
        minimum=0,
    )
    receipt_count = _require_int(
        lineage.get("receipt_count"),
        f"{label}.receipt_count",
        minimum=0,
    )
    for field in ("missing_receipt_chains", "invalid_receipt_chains"):
        _require_int(
            lineage.get(field),
            f"{label}.{field}",
            minimum=0,
            maximum=manifest_count,
        )
    parity = _require_bool(
        lineage.get("receipt_manifest_parity"),
        f"{label}.receipt_manifest_parity",
    )
    if parity != (manifest_count == receipt_count):
        raise OutcomeReportError(f"{label}.receipt_manifest_parity is inconsistent")
    identity_hash = _require_str(
        lineage.get("identity_hash"),
        f"{label}.identity_hash",
    )
    if len(identity_hash) != 64 or any(
        char not in "0123456789abcdef" for char in identity_hash
    ):
        raise OutcomeReportError(f"{label}.identity_hash must be lowercase sha256")
    return lineage


def validate_outcome_facts(value: object) -> dict[str, Any]:
    facts = _require_dict(value, "outcome_facts")
    if facts.get("schema") != OUTCOME_FACTS_SCHEMA:
        raise OutcomeReportError(
            f"unsupported outcome facts schema: {facts.get('schema')!r}"
        )
    if facts.get("report_version") != OUTCOME_REPORT_VERSION:
        raise OutcomeReportError(
            f"unsupported outcome report version: {facts.get('report_version')!r}"
        )

    contracts = _require_dict(
        facts.get("feature_contracts"),
        "outcome_facts.feature_contracts",
    )
    for head, (feature_set, encoder_version) in FEATURE_CONTRACTS.items():
        contract = _require_dict(
            contracts.get(head),
            f"outcome_facts.feature_contracts.{head}",
        )
        if contract.get("feature_set") != feature_set:
            raise OutcomeReportError(
                f"{head} feature contract is {contract.get('feature_set')!r}; "
                f"expected {feature_set!r}"
            )
        if contract.get("encoder_version") != encoder_version:
            raise OutcomeReportError(
                f"{head} encoder version is {contract.get('encoder_version')!r}; "
                f"expected {encoder_version}"
            )
        _require_bool(
            contract.get("model_loaded"),
            f"outcome_facts.feature_contracts.{head}.model_loaded",
        )

    ticks_executed = _require_int(
        facts.get("ticks_executed"),
        "outcome_facts.ticks_executed",
        minimum=0,
        maximum=120000,
    )
    for field in (
        "goal_completion_tick",
        "contract_completion_tick",
        "worker_completion_tick",
    ):
        _optional_tick(facts.get(field), f"outcome_facts.{field}")

    route = _require_dict(facts.get("route"), "outcome_facts.route")
    for field in ("start_distance", "end_distance", "distance_traveled"):
        _require_number(route.get(field), f"outcome_facts.route.{field}", minimum=0)
    _require_number(route.get("progress"), "outcome_facts.route.progress")
    efficiency = _require_number(
        route.get("efficiency"),
        "outcome_facts.route.efficiency",
        minimum=0,
    )
    if efficiency > 1:
        raise OutcomeReportError("outcome_facts.route.efficiency must be <= 1")
    for field in ("stuck_ticks", "recovery_events"):
        _require_int(
            route.get(field),
            f"outcome_facts.route.{field}",
            minimum=0,
            maximum=ticks_executed,
        )
    _require_int(
        route.get("loop_revisits"),
        "outcome_facts.route.loop_revisits",
        minimum=0,
        maximum=65535,
    )

    worker_route = _require_dict(
        facts.get("worker_route"),
        "outcome_facts.worker_route",
    )
    worker_tick_cap = ticks_executed + 1
    for field in (
        "assignment_ticks",
        "motion_ticks",
        "route_support_ticks",
        "useful_outcome_ticks",
        "stuck_ticks",
        "recovery_events",
    ):
        _require_int(
            worker_route.get(field),
            f"outcome_facts.worker_route.{field}",
            minimum=0,
            maximum=worker_tick_cap,
        )
    _require_int(
        worker_route.get("loop_revisits"),
        "outcome_facts.worker_route.loop_revisits",
        minimum=0,
        maximum=65535,
    )
    if worker_route["route_support_ticks"] > worker_route["motion_ticks"]:
        raise OutcomeReportError(
            "outcome_facts.worker_route route support exceeds motion"
        )
    if worker_route["stuck_ticks"] > worker_route["assignment_ticks"]:
        raise OutcomeReportError(
            "outcome_facts.worker_route stuck ticks exceed assignments"
        )
    worker_efficiency = _require_number(
        worker_route.get("efficiency"),
        "outcome_facts.worker_route.efficiency",
        minimum=0,
    )
    if worker_efficiency > 1:
        raise OutcomeReportError(
            "outcome_facts.worker_route.efficiency must be <= 1"
        )
    if worker_route["assignment_ticks"] == 0 and worker_efficiency != 0:
        raise OutcomeReportError(
            "outcome_facts.worker_route efficiency requires assignments"
        )

    safety = _require_dict(facts.get("safety"), "outcome_facts.safety")
    for field in (
        "collision_events",
        "death_events",
        "safety_overrides",
        "order_rejected_events",
    ):
        _require_int(safety.get(field), f"outcome_facts.safety.{field}", minimum=0)
    _require_number(
        safety.get("damage_amount"),
        "outcome_facts.safety.damage_amount",
        minimum=0,
    )

    decisions = _require_dict(facts.get("decisions"), "outcome_facts.decisions")
    for field in (
        "flight",
        "contract",
        "contract_teacher_fallbacks",
        "worker",
        "worker_teacher_fallbacks",
    ):
        _require_int(decisions.get(field), f"outcome_facts.decisions.{field}", minimum=0)
    if decisions["flight"] != 1:
        raise OutcomeReportError("flight replay must have exactly one decision")
    if decisions["contract_teacher_fallbacks"] > decisions["contract"]:
        raise OutcomeReportError("contract teacher fallbacks exceed contract decisions")
    if decisions["worker_teacher_fallbacks"] > decisions["worker"]:
        raise OutcomeReportError("worker teacher fallbacks exceed worker decisions")

    contract_counts = _require_dict(facts.get("contracts"), "outcome_facts.contracts")
    for field in ("start_active", "end_active", "completed"):
        _require_int(
            contract_counts.get(field),
            f"outcome_facts.contracts.{field}",
            minimum=0,
        )

    station_need = _require_dict(
        facts.get("station_need"),
        "outcome_facts.station_need",
    )
    for field in (
        "contract_completions",
        "sell_events",
        "sell_value",
        "delivery_cleared",
        "repair_events",
        "scaffolds_placed",
    ):
        _require_int(
            station_need.get(field),
            f"outcome_facts.station_need.{field}",
            minimum=0,
        )

    cargo = _require_dict(facts.get("cargo"), "outcome_facts.cargo")
    start_lineage = _validate_lineage(
        cargo.get("start"),
        "outcome_facts.cargo.start",
    )
    end_lineage = _validate_lineage(
        cargo.get("end"),
        "outcome_facts.cargo.end",
    )
    identity_unchanged = _require_bool(
        cargo.get("identity_unchanged"),
        "outcome_facts.cargo.identity_unchanged",
    )
    if identity_unchanged != (
        start_lineage["identity_hash"] == end_lineage["identity_hash"]
    ):
        raise OutcomeReportError(
            "outcome_facts.cargo.identity_unchanged is inconsistent"
        )
    integrity_preserved = _require_bool(
        cargo.get("lineage_integrity_preserved"),
        "outcome_facts.cargo.lineage_integrity_preserved",
    )
    expected_integrity = (
        start_lineage["receipt_manifest_parity"]
        and end_lineage["receipt_manifest_parity"]
        and start_lineage["missing_receipt_chains"] == 0
        and end_lineage["missing_receipt_chains"] == 0
        and start_lineage["invalid_receipt_chains"] == 0
        and end_lineage["invalid_receipt_chains"] == 0
    )
    if integrity_preserved != expected_integrity:
        raise OutcomeReportError(
            "outcome_facts.cargo.lineage_integrity_preserved is inconsistent"
        )
    return facts


def validate_outcome_output(path: Path) -> str | None:
    try:
        rows = _load_jsonl(path)
        for row_index, row in enumerate(rows):
            try:
                validate_outcome_facts(row.get("outcome_facts"))
            except OutcomeReportError as exc:
                return f"row {row_index}: {exc}"
    except OutcomeReportError as exc:
        return str(exc)
    return None


def _ai_int(ai: dict[str, Any], field: str) -> int:
    value = ai.get(field, 0)
    return value if type(value) is int and value >= 0 else 0


def _worker_attempted(facts: dict[str, Any], ai: dict[str, Any]) -> bool:
    return (
        facts["decisions"]["worker"] > 0
        or _ai_int(ai, "worker_assignment_ticks") > 0
        or _ai_int(ai, "worker_mine_assignment_ticks") > 0
        or _ai_int(ai, "worker_haul_assignment_ticks") > 0
        or _ai_int(ai, "worker_tow_assignment_ticks") > 0
        or _ai_int(ai, "worker_delivery_assignment_ticks") > 0
        or _ai_int(ai, "worker_scout_assignment_ticks") > 0
        or _ai_int(ai, "worker_repair_assignment_ticks") > 0
    )


def _status_for(
    head: str,
    facts: dict[str, Any],
    ai: dict[str, Any],
) -> tuple[str, int | None]:
    safety = facts["safety"]
    need = facts["station_need"]
    if head == "flight":
        if safety["death_events"] > 0:
            return "failed", None
        completion_tick = facts["goal_completion_tick"]
        if completion_tick is not None:
            return "completed", completion_tick
        return "in_progress", None

    if head == "contract":
        attempted = (
            facts["decisions"]["contract"] > 0
            or facts["contracts"]["completed"] > 0
            or need["delivery_cleared"] > 0
        )
        if not attempted:
            return "not_attempted", None
        if (
            safety["order_rejected_events"] > 0
            and facts["contracts"]["completed"] == 0
            and need["delivery_cleared"] == 0
        ):
            return "failed", None
        if facts["contracts"]["completed"] > 0 or need["delivery_cleared"] > 0:
            completion_tick = facts["contract_completion_tick"]
            if completion_tick is None and need["delivery_cleared"] > 0:
                completion_tick = facts["worker_completion_tick"]
            if completion_tick is None:
                raise OutcomeReportError(
                    "completed contract episode has no deterministic completion tick"
                )
            return "completed", completion_tick
        return "in_progress", None

    attempted = _worker_attempted(facts, ai)
    if not attempted:
        return "not_attempted", None
    if _ai_int(ai, "npc_delivery_shipments_defaulted") > 0:
        return "failed", None
    if (
        facts["worker_completion_tick"] is not None
        or need["delivery_cleared"] > 0
        or need["scaffolds_placed"] > 0
        or (
            need["repair_events"] > 0
            and _ai_int(ai, "worker_repair_assignment_ticks") > 0
        )
    ):
        completion_tick = facts["worker_completion_tick"]
        if completion_tick is None:
            raise OutcomeReportError(
                "completed worker episode has no deterministic completion tick"
            )
        return "completed", completion_tick
    return "in_progress", None


def _decision_summary(
    head: str,
    mode: str,
    facts: dict[str, Any],
) -> dict[str, Any]:
    decisions = facts["decisions"]
    if head == "flight":
        return {
            "requested_mode": mode,
            "effective_mode": "replay_counterfactual",
            "decision_source": "replay_candidate",
            "decision_count": decisions["flight"],
            "teacher_fallback_count": 0,
            "model_decision_count": 0,
        }

    decision_count = decisions[head]
    fallback_count = decisions[f"{head}_teacher_fallbacks"]
    model_count = decision_count - fallback_count
    if mode in ("teacher", "shadow") or model_count == 0:
        effective_mode = "teacher"
    else:
        effective_mode = mode
    decision_source = (
        "teacher_fallback"
        if fallback_count > 0 and model_count == 0
        else "model"
        if model_count > 0
        else "none"
    )
    return {
        "requested_mode": mode,
        "effective_mode": effective_mode,
        "decision_source": decision_source,
        "decision_count": decision_count,
        "teacher_fallback_count": fallback_count,
        "model_decision_count": model_count,
    }


def _worker_diversity(ai: dict[str, Any]) -> int:
    fields = (
        "worker_mine_assignment_ticks",
        "worker_haul_assignment_ticks",
        "worker_tow_assignment_ticks",
        "worker_delivery_assignment_ticks",
        "worker_scout_assignment_ticks",
        "worker_repair_assignment_ticks",
    )
    return sum(1 for field in fields if _ai_int(ai, field) > 0)


def _contract_diversity(need: dict[str, Any]) -> int:
    fields = (
        "contract_completions",
        "sell_events",
        "delivery_cleared",
        "repair_events",
    )
    return sum(1 for field in fields if need[field] > 0)


def _flight_diversity(candidate_name: str) -> int:
    turning = "A" in candidate_name or "D" in candidate_name
    thrusting = "W" in candidate_name or "S" in candidate_name
    return int(turning) + int(thrusting)


def _episode_from_row(
    *,
    mode: str,
    corpus: str,
    corpus_version: int,
    generator_version: int,
    scenario_index: int,
    scenario_name: str,
    scenario_args: list[str],
    row: dict[str, Any],
    head: str,
) -> dict[str, Any]:
    if row.get("schema") != "signal.replay_counterfactual.v1":
        raise OutcomeReportError(
            f"scenario {scenario_index} has unsupported replay schema"
        )
    candidate = _require_int(row.get("candidate"), "replay.candidate", minimum=0)
    candidate_name = _require_str(
        row.get("candidate_name"),
        "replay.candidate_name",
    )
    facts = validate_outcome_facts(row.get("outcome_facts"))
    ai_value = row.get("ai", {})
    ai = ai_value if isinstance(ai_value, dict) else {}

    replay_identity = {
        "corpus": corpus,
        "corpus_version": corpus_version,
        "generator_version": generator_version,
        "scenario_index": scenario_index,
        "args": scenario_args,
    }
    replay_id = _stable_id("signal-ai-replay-v1", replay_identity)
    episode_id = _stable_id(
        "signal-ai-episode-v1",
        {
            "replay_id": replay_id,
            "candidate": candidate,
            "head": head,
            "report_version": OUTCOME_REPORT_VERSION,
        },
    )
    status, ticks_to_completion = _status_for(head, facts, ai)

    need = facts["station_need"]
    if head == "worker":
        station_need_served = (
            need["delivery_cleared"]
            + need["repair_events"]
            + need["scaffolds_placed"]
        )
        diversity = _worker_diversity(ai)
        route = facts["worker_route"]
    elif head == "contract":
        station_need_served = (
            need["contract_completions"]
            + need["delivery_cleared"]
            + need["sell_events"]
        )
        diversity = _contract_diversity(need)
        route = facts["worker_route"]
    else:
        station_need_served = 0
        diversity = _flight_diversity(candidate_name)
        route = facts["route"]

    if head == "flight":
        route_distance = route["distance_traveled"]
        route_progress = route["progress"]
    else:
        route_distance = route["motion_ticks"]
        route_progress = route["useful_outcome_ticks"]

    cargo = facts["cargo"]
    return {
        "schema": OUTCOME_EPISODE_SCHEMA,
        "report_version": OUTCOME_REPORT_VERSION,
        "episode_id": episode_id,
        "replay_id": replay_id,
        "scenario_index": scenario_index,
        "scenario": scenario_name,
        "head": head,
        "candidate": candidate,
        "candidate_name": candidate_name,
        "status": status,
        "ticks_observed": facts["ticks_executed"],
        "ticks_to_completion": ticks_to_completion,
        "truncated": status == "in_progress",
        "decision": _decision_summary(head, mode, facts),
        "metrics": {
            "completion": status == "completed",
            "collision_events": facts["safety"]["collision_events"],
            "damage_amount": facts["safety"]["damage_amount"],
            "death_events": facts["safety"]["death_events"],
            "stuck_ticks": route["stuck_ticks"],
            "recovery_events": route["recovery_events"],
            "safety_overrides": facts["safety"]["safety_overrides"],
            "repeated_loops": route["loop_revisits"],
            "behavioral_diversity": diversity,
            "route_distance": route_distance,
            "route_progress": route_progress,
            "route_efficiency": route["efficiency"],
            "station_need_served": station_need_served,
            "cargo_identity_unchanged": cargo["identity_unchanged"],
            "provenance_preserved": cargo["lineage_integrity_preserved"],
            "start_manifest_count": cargo["start"]["manifest_count"],
            "end_manifest_count": cargo["end"]["manifest_count"],
            "missing_receipt_chains": cargo["end"]["missing_receipt_chains"],
            "invalid_receipt_chains": cargo["end"]["invalid_receipt_chains"],
            "receipt_manifest_parity": cargo["end"]["receipt_manifest_parity"],
        },
        "feature_contract": facts["feature_contracts"][head],
    }


def build_episode_report(
    *,
    decision_mode: str,
    corpus: str,
    corpus_version: int,
    generator_version: int,
    scenario_entries: Iterable[tuple[dict[str, Any], Path]],
) -> dict[str, Any]:
    if decision_mode not in DECISION_MODES:
        raise OutcomeReportError(f"unsupported decision mode: {decision_mode!r}")
    _require_str(corpus, "corpus")
    _require_int(corpus_version, "corpus_version", minimum=1)
    _require_int(generator_version, "generator_version", minimum=1)

    episodes: list[dict[str, Any]] = []
    replays: list[dict[str, Any]] = []
    for entry, path in scenario_entries:
        scenario_index = _require_int(
            entry.get("index"),
            "scenario.index",
            minimum=0,
        )
        raw_args = _require_list(entry.get("args"), "scenario.args")
        scenario_args = [
            _require_str(value, f"scenario.args[{index}]")
            for index, value in enumerate(raw_args)
        ]
        evaluation = _require_dict(entry.get("evaluation"), "scenario.evaluation")
        scenario_name = _require_str(
            evaluation.get("name"),
            "scenario.evaluation.name",
        )
        replay_id = _stable_id(
            "signal-ai-replay-v1",
            {
                "corpus": corpus,
                "corpus_version": corpus_version,
                "generator_version": generator_version,
                "scenario_index": scenario_index,
                "args": scenario_args,
            },
        )
        rows = _load_jsonl(path)
        replays.append(
            {
                "replay_id": replay_id,
                "scenario_index": scenario_index,
                "scenario": scenario_name,
                "args": scenario_args,
                "candidate_count": len(rows),
            }
        )
        for row in rows:
            for head in HEADS:
                episodes.append(
                    _episode_from_row(
                        mode=decision_mode,
                        corpus=corpus,
                        corpus_version=corpus_version,
                        generator_version=generator_version,
                        scenario_index=scenario_index,
                        scenario_name=scenario_name,
                        scenario_args=scenario_args,
                        row=row,
                        head=head,
                    )
                )

    if not episodes:
        raise OutcomeReportError("outcome report has no episodes")
    episodes.sort(
        key=lambda episode: (
            episode["scenario_index"],
            episode["candidate"],
            HEADS.index(episode["head"]),
        )
    )
    replays.sort(key=lambda replay: replay["scenario_index"])
    report_core = {
        "schema": OUTCOME_REPORT_SCHEMA,
        "report_version": OUTCOME_REPORT_VERSION,
        "decision_mode": decision_mode,
        "corpus": corpus,
        "corpus_version": corpus_version,
        "generator_version": generator_version,
        "replays": replays,
        "episodes": episodes,
    }
    report_core["report_id"] = _stable_id(
        "signal-ai-episode-report-v1",
        report_core,
    )
    validate_episode_report(report_core, expected_mode=decision_mode)
    return report_core


def validate_episode_report(
    value: object,
    *,
    expected_mode: str | None = None,
) -> dict[str, Any]:
    report = _require_dict(value, "outcome report")
    if report.get("schema") != OUTCOME_REPORT_SCHEMA:
        raise OutcomeReportError(
            f"unsupported outcome report schema: {report.get('schema')!r}"
        )
    if report.get("report_version") != OUTCOME_REPORT_VERSION:
        raise OutcomeReportError(
            f"unsupported outcome report version: {report.get('report_version')!r}"
        )
    mode = report.get("decision_mode")
    if mode not in DECISION_MODES:
        raise OutcomeReportError(f"unsupported outcome report mode: {mode!r}")
    if expected_mode is not None and mode != expected_mode:
        raise OutcomeReportError(
            f"outcome report mode is {mode!r}; expected {expected_mode!r}"
        )
    _require_str(report.get("corpus"), "outcome report.corpus")
    _require_int(report.get("corpus_version"), "outcome report.corpus_version", minimum=1)
    _require_int(
        report.get("generator_version"),
        "outcome report.generator_version",
        minimum=1,
    )
    report_id = _require_str(report.get("report_id"), "outcome report.report_id")
    if len(report_id) != 64:
        raise OutcomeReportError("outcome report.report_id must be sha256")

    replays = _require_list(report.get("replays"), "outcome report.replays")
    episodes = _require_list(report.get("episodes"), "outcome report.episodes")
    if not replays or not episodes:
        raise OutcomeReportError("outcome report must contain replays and episodes")
    replay_ids: set[str] = set()
    for replay_index, replay_value in enumerate(replays):
        replay = _require_dict(replay_value, f"outcome report.replays[{replay_index}]")
        replay_id = _require_str(
            replay.get("replay_id"),
            f"outcome report.replays[{replay_index}].replay_id",
        )
        if replay_id in replay_ids:
            raise OutcomeReportError(f"duplicate replay_id: {replay_id}")
        replay_ids.add(replay_id)

    episode_ids: set[str] = set()
    for episode_index, episode_value in enumerate(episodes):
        label = f"outcome report.episodes[{episode_index}]"
        episode = _require_dict(episode_value, label)
        if episode.get("schema") != OUTCOME_EPISODE_SCHEMA:
            raise OutcomeReportError(f"{label} has unsupported schema")
        if episode.get("report_version") != OUTCOME_REPORT_VERSION:
            raise OutcomeReportError(f"{label} has unsupported report version")
        episode_id = _require_str(episode.get("episode_id"), f"{label}.episode_id")
        if episode_id in episode_ids:
            raise OutcomeReportError(f"duplicate episode_id: {episode_id}")
        episode_ids.add(episode_id)
        if episode.get("replay_id") not in replay_ids:
            raise OutcomeReportError(f"{label} references an unknown replay_id")
        if episode.get("head") not in HEADS:
            raise OutcomeReportError(f"{label} has an unsupported head")
        if episode.get("status") not in STATUSES:
            raise OutcomeReportError(f"{label} has an unsupported status")
        _require_bool(episode.get("truncated"), f"{label}.truncated")
        status = episode["status"]
        if episode["truncated"] != (status == "in_progress"):
            raise OutcomeReportError(f"{label}.truncated disagrees with status")
        ticks_to_completion = episode.get("ticks_to_completion")
        if status == "completed":
            if ticks_to_completion is None:
                raise OutcomeReportError(
                    f"{label}.ticks_to_completion is required when completed"
                )
            _optional_tick(ticks_to_completion, f"{label}.ticks_to_completion")
        elif ticks_to_completion is not None:
            raise OutcomeReportError(
                f"{label}.ticks_to_completion must be null unless completed"
            )
        decision = _require_dict(episode.get("decision"), f"{label}.decision")
        if decision.get("requested_mode") != mode:
            raise OutcomeReportError(f"{label} requested mode does not match report")
        metrics = _require_dict(episode.get("metrics"), f"{label}.metrics")
        _require_bool(metrics.get("provenance_preserved"), f"{label}.metrics.provenance_preserved")
        _require_int(
            metrics.get("repeated_loops"),
            f"{label}.metrics.repeated_loops",
            minimum=0,
        )
        feature_contract = _require_dict(
            episode.get("feature_contract"),
            f"{label}.feature_contract",
        )
        expected_contract = FEATURE_CONTRACTS[episode["head"]]
        if (
            feature_contract.get("feature_set") != expected_contract[0]
            or feature_contract.get("encoder_version") != expected_contract[1]
        ):
            raise OutcomeReportError(f"{label} feature contract mismatch")
    return report


def load_episode_report(
    path: Path,
    *,
    expected_mode: str | None = None,
) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise OutcomeReportError(f"cannot load outcome report {path}: {exc}") from exc
    return validate_episode_report(value, expected_mode=expected_mode)


def serialize_episode_report(report: dict[str, Any]) -> bytes:
    validate_episode_report(report)
    return (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
