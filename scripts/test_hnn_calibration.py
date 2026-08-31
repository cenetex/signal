#!/usr/bin/env python3
"""Unit tests for the deterministic HNN calibration corpus and generator."""

from __future__ import annotations

import unittest

from calibrate_hnn_confidence import accepted, threshold_for
from hnn_null_corpus import canonical_scenario_digest, null_scenarios


class HnnCalibrationTests(unittest.TestCase):
    def test_null_corpus_is_versioned_and_covers_every_control(self) -> None:
        scenarios = null_scenarios()
        counts: dict[str, int] = {}
        for scenario in scenarios:
            counts[scenario.category] = counts.get(scenario.category, 0) + 1
            self.assertIn("--hnn-trace", scenario.args)
            self.assertIn("--hnn-confidence-mode", scenario.args)
        self.assertEqual(
            counts,
            {
                "disrupted-world": 16,
                "overloaded-memory": 8,
                "shuffled-labels": 16,
                "unrelated-trace": 16,
            },
        )
        self.assertEqual(len(scenarios), 56)
        self.assertEqual(
            canonical_scenario_digest(scenarios),
            "a4b14d3a4311e05e1bc97d5b71c294c9d4db714bb6aded535ff6b39c67b075d8",
        )

    def test_threshold_uses_training_only_and_capacity_abstains(self) -> None:
        samples = [
            {
                "allowed_margin": 0.30,
                "capacity_load": 0.5,
                "held_out": False,
                "stored_count": 64,
                "top_allowed_score": 0.72,
            },
            {
                "allowed_margin": 0.80,
                "capacity_load": 0.5,
                "held_out": True,
                "stored_count": 64,
                "top_allowed_score": 0.99,
            },
        ]
        min_score, min_margin = threshold_for(samples)
        self.assertGreater(min_score, 0.72)
        self.assertLess(min_score, 0.73)
        self.assertEqual(min_margin, 0.25)
        overloaded = {
            "allowed_margin": 0.99,
            "capacity_load": 1.01,
            "stored_count": 129,
            "top_allowed_score": 0.99,
        }
        self.assertFalse(accepted(overloaded, min_score, min_margin))


if __name__ == "__main__":
    unittest.main()
