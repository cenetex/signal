#!/usr/bin/env python3
"""Mutation coverage for the required-check aggregate."""

import unittest

import check_ci_required


LANES = [
    "policy",
    "native",
    "soak",
    "fuzz",
    "browser",
    "container",
    "cross_platform",
    "replay",
    "tools",
    "programs",
]


class RequiredAggregateTests(unittest.TestCase):
    def valid_inputs(self) -> tuple[dict[str, str], dict[str, str]]:
        selected = {lane: "false" for lane in LANES}
        selected["policy"] = "true"
        results = {lane: "skipped" for lane in LANES}
        results.update({"classify": "success", "policy": "success"})
        return selected, results

    def test_selected_success_and_unselected_skip_pass(self) -> None:
        selected, results = self.valid_inputs()
        selected["browser"] = "true"
        results["browser"] = "success"
        self.assertEqual(
            check_ci_required.aggregate_failures(
                LANES, selected, results
            ),
            [],
        )

    def test_required_lane_cannot_be_skipped(self) -> None:
        selected, results = self.valid_inputs()
        selected["cross_platform"] = "true"
        failures = check_ci_required.aggregate_failures(
            LANES, selected, results
        )
        self.assertTrue(any(
            "cross_platform was required" in failure
            for failure in failures
        ), failures)

    def test_failed_classifier_always_fails_aggregate(self) -> None:
        selected, results = self.valid_inputs()
        results["classify"] = "failure"
        failures = check_ci_required.aggregate_failures(
            LANES, selected, results
        )
        self.assertTrue(any(
            "classify must succeed" in failure for failure in failures
        ), failures)

    def test_missing_classifier_output_fails_closed(self) -> None:
        selected, results = self.valid_inputs()
        selected["fuzz"] = ""
        failures = check_ci_required.aggregate_failures(
            LANES, selected, results
        )
        self.assertTrue(any(
            "fuzz has invalid classifier output" in failure
            for failure in failures
        ), failures)


if __name__ == "__main__":
    unittest.main()
