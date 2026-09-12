#!/usr/bin/env python3
"""Checks for paired summaries and invalid evidence."""
import unittest
from compare_connectome_strategy import METRICS, summarize, validate


def row(seed=2037, strategy=False):
    return {**dict.fromkeys(METRICS, 0), "schema": 1, "seed": seed, "ticks": 300,
            "strategy": strategy, "initial_rng": 42, "chain_verified": True,
            "event_capacity_ticks": 0, "strategy_changes": int(strategy)}


class ComparisonTests(unittest.TestCase):
    def test_paired_differences(self):
        off, on = row(), row(strategy=True)
        off["delivered_units"], on["delivered_units"] = 3, 8
        result = summarize([on, off], [2037])
        self.assertEqual(result["totals"]["delivered_units"]["median_delta"], 5)
        self.assertEqual(result["totals"]["delivered_units"]["higher"], 1)

    def test_missing_duplicate_or_different_initial_world(self):
        for rows in ([row()], [row(), row(), row(strategy=True)],
                     [row(), {**row(strategy=True), "initial_rng": 43}]):
            with self.assertRaises(ValueError):
                summarize(rows, [2037])

    def test_rejects_unverified_wrong_mode_and_overflow(self):
        for change in ({"chain_verified": False}, {"strategy": True},
                       {"event_capacity_ticks": 1}, {"delivered_units": float("nan")}):
            with self.assertRaises(ValueError):
                validate({**row(), **change}, 2037, 300, False)

    def test_zero_outcomes_are_valid(self):
        validate(row(), 2037, 300, False)


if __name__ == "__main__":
    unittest.main()
