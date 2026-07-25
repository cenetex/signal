#!/usr/bin/env python3
"""Focused mutation coverage for the checked cell-damage table."""

import unittest

import check_doc_freshness as freshness


class CellStressFreshnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = freshness.CELL_STRESS.read_text(encoding="utf-8")
        cls.doc = freshness.CELL_DAMAGE_DOC.read_text(encoding="utf-8")

    def test_current_table_matches_header(self) -> None:
        self.assertEqual(
            freshness.cell_stress_failures(self.header, self.doc), [])

    def test_code_threshold_change_requires_doc_update(self) -> None:
        changed = self.header.replace(
            "CELL_STRESS_TRIANGLE_FAILURE = 80",
            "CELL_STRESS_TRIANGLE_FAILURE = 81",
        )
        failures = freshness.cell_stress_failures(changed, self.doc)
        self.assertTrue(any(
            "Directional triangle mount" in failure
            and "triangle is 81" in failure
            for failure in failures
        ), failures)

    def test_doc_threshold_change_requires_code_update(self) -> None:
        changed = self.doc.replace(
            "| Standard complete-edge weld "
            "| standard cell/component can detach | 120 |",
            "| Standard complete-edge weld "
            "| standard cell/component can detach | 121 |",
        )
        failures = freshness.cell_stress_failures(self.header, changed)
        self.assertTrue(any(
            "Standard complete-edge weld" in failure
            and "'121'" in failure
            for failure in failures
        ), failures)

    def test_hub_stage_and_failure_are_checked(self) -> None:
        changed = self.doc.replace(
            "visible stages at 120 and 240, fails at 360 | 360",
            "visible stages at 120 and 241, fails at 361 | 361",
        )
        failures = freshness.cell_stress_failures(self.header, changed)
        self.assertTrue(any("hub stages" in failure for failure in failures))
        self.assertTrue(any(
            "hub failure threshold" in failure for failure in failures))

    def test_historical_prose_remains_unrestricted(self) -> None:
        changed = self.doc + (
            "\nHistorical tuning used thresholds 79, 119, and 359.\n"
        )
        self.assertEqual(
            freshness.cell_stress_failures(self.header, changed), [])


if __name__ == "__main__":
    unittest.main()
