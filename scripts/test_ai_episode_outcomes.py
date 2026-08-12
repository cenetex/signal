#!/usr/bin/env python3
"""Unit tests for deterministic AI episode outcome report contracts."""

from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from ai_episode_outcomes import (
    FEATURE_CONTRACTS,
    OUTCOME_FACTS_SCHEMA,
    OUTCOME_REPORT_VERSION,
    OutcomeReportError,
    build_episode_report,
    validate_episode_report,
    validate_outcome_facts,
)


def lineage() -> dict[str, object]:
    return {
        "manifest_count": 0,
        "receipt_count": 0,
        "missing_receipt_chains": 0,
        "invalid_receipt_chains": 0,
        "receipt_manifest_parity": True,
        "identity_hash": "12" * 32,
    }


def facts() -> dict[str, object]:
    return {
        "schema": OUTCOME_FACTS_SCHEMA,
        "report_version": OUTCOME_REPORT_VERSION,
        "feature_contracts": {
            head: {
                "feature_set": contract[0],
                "encoder_version": contract[1],
                "model_loaded": head == "flight",
            }
            for head, contract in FEATURE_CONTRACTS.items()
        },
        "ticks_executed": 24,
        "goal_completion_tick": None,
        "contract_completion_tick": None,
        "worker_completion_tick": None,
        "route": {
            "start_distance": 1000.0,
            "end_distance": 800.0,
            "distance_traveled": 250.0,
            "progress": 200.0,
            "efficiency": 0.8,
            "stuck_ticks": 0,
            "recovery_events": 0,
            "loop_revisits": 0,
        },
        "worker_route": {
            "assignment_ticks": 0,
            "motion_ticks": 0,
            "route_support_ticks": 0,
            "useful_outcome_ticks": 0,
            "efficiency": 0.0,
            "stuck_ticks": 0,
            "recovery_events": 0,
            "loop_revisits": 0,
        },
        "safety": {
            "collision_events": 0,
            "damage_amount": 0.0,
            "death_events": 0,
            "safety_overrides": 0,
            "order_rejected_events": 0,
        },
        "decisions": {
            "flight": 1,
            "contract": 0,
            "contract_teacher_fallbacks": 0,
            "worker": 0,
            "worker_teacher_fallbacks": 0,
        },
        "contracts": {
            "start_active": 0,
            "end_active": 0,
            "completed": 0,
        },
        "station_need": {
            "contract_completions": 0,
            "sell_events": 0,
            "sell_value": 0,
            "delivery_cleared": 0,
            "repair_events": 0,
            "scaffolds_placed": 0,
        },
        "cargo": {
            "start": lineage(),
            "end": lineage(),
            "identity_unchanged": True,
            "lineage_integrity_preserved": True,
        },
    }


def replay_row(
    candidate: int,
    outcome_facts: dict[str, object],
    *,
    ai: dict[str, object] | None = None,
) -> dict[str, object]:
    row: dict[str, object] = {
        "schema": "signal.replay_counterfactual.v1",
        "candidate": candidate,
        "candidate_name": ("NONE", "W", "A", "D")[candidate],
        "outcome_facts": outcome_facts,
    }
    if ai is not None:
        row["ai"] = ai
    return row


def scenario_entry() -> dict[str, object]:
    return {
        "index": 0,
        "args": ["--seed", "651", "--candidates", "NONE,W,A,D"],
        "evaluation": {"name": "seeded-only"},
        "file": "scenario-000.jsonl",
    }


class EpisodeOutcomeTests(unittest.TestCase):
    def build(
        self,
        rows: list[dict[str, object]],
        *,
        mode: str = "teacher",
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "scenario-000.jsonl"
            path.write_text(
                "".join(json.dumps(row) + "\n" for row in rows),
                encoding="utf-8",
            )
            return build_episode_report(
                decision_mode=mode,
                corpus="signal-ai-outcomes",
                corpus_version=1,
                generator_version=1,
                scenario_entries=[(scenario_entry(), path)],
            )

    def test_statuses_distinguish_completion_failure_and_truncation(self) -> None:
        completed = facts()
        completed["goal_completion_tick"] = 7
        failed = facts()
        failed["safety"]["death_events"] = 1
        contract = facts()
        contract["decisions"]["contract"] = 1
        contract["decisions"]["contract_teacher_fallbacks"] = 1
        contract["contracts"]["completed"] = 1
        contract["contract_completion_tick"] = 11
        worker = facts()
        report = self.build(
            [
                replay_row(0, completed),
                replay_row(1, failed),
                replay_row(2, contract),
                replay_row(
                    3,
                    worker,
                    ai={"worker_assignment_ticks": 4},
                ),
            ]
        )
        by_key = {
            (episode["candidate"], episode["head"]): episode
            for episode in report["episodes"]
        }
        self.assertEqual(by_key[(0, "flight")]["status"], "completed")
        self.assertEqual(by_key[(0, "flight")]["ticks_to_completion"], 7)
        self.assertEqual(by_key[(1, "flight")]["status"], "failed")
        self.assertEqual(by_key[(2, "contract")]["status"], "completed")
        self.assertEqual(by_key[(3, "worker")]["status"], "in_progress")
        self.assertTrue(by_key[(3, "worker")]["truncated"])
        self.assertEqual(by_key[(0, "worker")]["status"], "not_attempted")

    def test_episode_ids_are_mode_invariant(self) -> None:
        rows = [replay_row(0, facts()), replay_row(1, facts())]
        teacher = self.build(rows, mode="teacher")
        active = self.build(rows, mode="active")
        self.assertEqual(
            {episode["episode_id"] for episode in teacher["episodes"]},
            {episode["episode_id"] for episode in active["episodes"]},
        )
        self.assertNotEqual(teacher["report_id"], active["report_id"])

    def test_provenance_failure_is_preserved_in_metrics(self) -> None:
        broken = facts()
        broken["cargo"]["end"]["manifest_count"] = 1
        broken["cargo"]["end"]["receipt_count"] = 1
        broken["cargo"]["end"]["invalid_receipt_chains"] = 1
        broken["cargo"]["end"]["identity_hash"] = "34" * 32
        broken["cargo"]["identity_unchanged"] = False
        broken["cargo"]["lineage_integrity_preserved"] = False
        report = self.build([replay_row(0, broken)])
        for episode in report["episodes"]:
            self.assertFalse(episode["metrics"]["provenance_preserved"])
            self.assertEqual(episode["metrics"]["invalid_receipt_chains"], 1)

    def test_bounded_route_counter_change_fails_closed(self) -> None:
        unbounded = facts()
        unbounded["worker_route"]["loop_revisits"] = 65536
        with self.assertRaisesRegex(OutcomeReportError, "must be <= 65535"):
            validate_outcome_facts(unbounded)

    def test_inconsistent_lineage_claim_fails_closed(self) -> None:
        inconsistent = facts()
        inconsistent["cargo"]["end"]["manifest_count"] = 1
        inconsistent["cargo"]["end"]["receipt_count"] = 1
        inconsistent["cargo"]["end"]["missing_receipt_chains"] = 1
        with self.assertRaisesRegex(
            OutcomeReportError,
            "lineage_integrity_preserved is inconsistent",
        ):
            validate_outcome_facts(inconsistent)

    def test_outcome_facts_schema_change_fails_closed(self) -> None:
        stale = facts()
        stale["schema"] = "signal.ai_outcome_facts.v0"
        with self.assertRaisesRegex(OutcomeReportError, "unsupported outcome facts"):
            validate_outcome_facts(stale)

    def test_report_version_change_fails_closed(self) -> None:
        report = self.build([replay_row(0, facts())])
        stale = copy.deepcopy(report)
        stale["report_version"] = OUTCOME_REPORT_VERSION + 1
        with self.assertRaisesRegex(OutcomeReportError, "unsupported outcome report"):
            validate_episode_report(stale)


if __name__ == "__main__":
    unittest.main()
