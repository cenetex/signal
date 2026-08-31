#!/usr/bin/env python3
"""Unit tests for the Signal x liblecore replay comparison gate."""

from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from compare_liblecore_replay import compare_bundles


def replay_row(backend: str, score_delta: float = 0.0) -> dict[str, object]:
    return {
        "schema": "signal.replay_counterfactual.v1",
        "seed": 6060,
        "station": 0,
        "provenance_script": "none",
        "prefix_ticks": 6,
        "horizon_ticks": 24,
        "candidate": 1,
        "candidate_name": "W",
        "event_hash": "event",
        "state_root": "state",
        "start_hull": 100.0,
        "end_hull": 100.0,
        "hull_loss": 0.0,
        "start_cargo": 0.0,
        "end_cargo": 0.0,
        "start_balance": 0.0,
        "end_balance": 0.0,
        "end_manifest_count": 0,
        "damage_events": 0,
        "death_events": 0,
        "dock_events": 0,
        "launch_events": 0,
        "pickup_events": 0,
        "buy_events": 0,
        "sell_events": 0,
        "repair_events": 0,
        "fracture_events": 0,
        "outpost_placed_events": 0,
        "authority": "deterministic_seed_prefix_replay",
        "receipt_trust": {"schema": "signal.receipt_trust.v1", "trusted": 0},
        "outcome_facts": {
            "schema": "signal.ai_outcome_facts.v1",
            "safety": {"collision_events": 0, "death_events": 0},
            "cargo": {"lineage_integrity_preserved": True},
        },
        "evaluation": {"scenario": "none"},
        "hnn_backend": {"active_backend": backend},
        "hnn": {
            "top_action": 3,
            "top_allowed_action": 3,
            "allowed_mask": "0x06e",
            "candidate_allowed_rank": 4,
            "candidate_score": 0.25 + score_delta,
            "top_score": 0.57 + score_delta,
            "top_allowed_score": 0.57 + score_delta,
            "margin": 0.15 + score_delta,
            "allowed_margin": 0.15 + score_delta,
            "trace_fidelity": 0.85 + score_delta,
            "scores": [
                {"index": 0, "score": 0.1 + score_delta},
                {"index": 3, "score": 0.57 + score_delta},
            ],
        },
    }


def write_bundle(root: Path, row: dict[str, object]) -> None:
    root.mkdir()
    scenario = (json.dumps(row, sort_keys=True) + "\n").encode()
    outcomes = b'{"schema":"signal.ai_episode_report.v1"}\n'
    (root / "scenario-000.jsonl").write_bytes(scenario)
    (root / "outcomes.json").write_bytes(outcomes)
    manifest = {
        "schema": "signal.replay_bundle.v3",
        "scenario_set": "fast",
        "outcomes": {
            "file": "outcomes.json",
            "sha256": hashlib.sha256(outcomes).hexdigest(),
            "size": len(outcomes),
        },
        "scenarios": [
            {
                "args": ["--seed", "6060", "--hnn-trace"],
                "file": "scenario-000.jsonl",
                "index": 0,
                "sha256": hashlib.sha256(scenario).hexdigest(),
                "size": len(scenario),
            }
        ],
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )


class CompareLiblecoreReplayTests(unittest.TestCase):
    def make_roots(
        self,
        temp: Path,
        direct: dict[str, object] | None = None,
        radix2: dict[str, object] | None = None,
    ) -> dict[str, Path]:
        roots = {
            "builtin": temp / "builtin",
            "direct": temp / "direct",
            "radix2": temp / "radix2",
        }
        write_bundle(roots["builtin"], replay_row("builtin-radix2"))
        write_bundle(
            roots["direct"],
            direct or replay_row("lecore-direct", score_delta=4.0e-7),
        )
        write_bundle(
            roots["radix2"],
            radix2 or replay_row("lecore-radix2", score_delta=3.0e-7),
        )
        return roots

    def test_accepts_tolerated_scores_and_identical_authority(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            report, failures = compare_bundles(
                self.make_roots(Path(directory))
            )
        self.assertFalse(failures)
        self.assertTrue(report["passed"])
        self.assertEqual(
            report["comparisons"]["direct"]["legal_action_agreement"],
            {"agreed": 1, "total": 1},
        )

    def test_rejects_legal_action_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            direct = replay_row("lecore-direct")
            direct_hnn = direct["hnn"]
            assert isinstance(direct_hnn, dict)
            direct_hnn["top_allowed_action"] = 6
            _, failures = compare_bundles(
                self.make_roots(Path(directory), direct=direct)
            )
        self.assertTrue(any("legal top action differs" in item for item in failures))

    def test_rejects_authority_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            radix2 = replay_row("lecore-radix2")
            facts = copy.deepcopy(radix2["outcome_facts"])
            assert isinstance(facts, dict)
            safety = facts["safety"]
            assert isinstance(safety, dict)
            safety["death_events"] = 1
            radix2["outcome_facts"] = facts
            _, failures = compare_bundles(
                self.make_roots(Path(directory), radix2=radix2)
            )
        self.assertTrue(
            any("changed safety or authority output" in item for item in failures)
        )

    def test_rejects_score_drift_above_tolerance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, failures = compare_bundles(
                self.make_roots(
                    Path(directory),
                    direct=replay_row("lecore-direct", score_delta=2.0e-5),
                )
            )
        self.assertTrue(any("score delta" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
