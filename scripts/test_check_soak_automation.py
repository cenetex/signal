#!/usr/bin/env python3
"""Mutation tests for the RUN_SOAK inventory and automation contract."""

from __future__ import annotations

import unittest
from pathlib import Path

import check_soak_automation as soak


class SoakInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = {
            path: path.read_text(encoding="utf-8")
            for path in sorted((soak.ROOT / "tests" / "c").glob("test_*.c"))
        }
        cls.cmake = soak.CMAKE.read_text(encoding="utf-8")
        cls.test_main = soak.TEST_MAIN.read_text(encoding="utf-8")

    def test_current_inventory_is_compiled_and_reachable(self) -> None:
        registrations, failures = soak.inventory_failures(
            self.sources, self.cmake, self.test_main
        )
        self.assertEqual(failures, [])
        self.assertEqual(
            {registration.inventory_key for registration in registrations},
            soak.EXPECTED_SOAK_REGISTRATIONS,
        )

    def test_new_tag_requires_inventory_update(self) -> None:
        changed = dict(self.sources)
        target = soak.ROOT / "tests" / "c" / "test_navigation.c"
        changed[target] += """

void register_new_soak_tests(void) {
    RUN_SOAK(test_new_functional_soak);
}
"""
        _, failures = soak.inventory_failures(
            changed, self.cmake, self.test_main
        )
        self.assertTrue(any(
            "unreviewed RUN_SOAK registration" in failure
            and "test_new_functional_soak" in failure
            for failure in failures
        ), failures)

    def test_soak_source_must_be_compiled(self) -> None:
        changed_cmake = self.cmake.replace(
            "        tests/c/test_navigation.c\n", ""
        )
        _, failures = soak.inventory_failures(
            self.sources, changed_cmake, self.test_main
        )
        self.assertTrue(any(
            "test_navigation.c" in failure
            and "not a signal_test source" in failure
            for failure in failures
        ), failures)

    def test_soak_registry_must_be_invoked(self) -> None:
        changed_main = self.test_main.replace(
            "    register_econ_sim_invariant_tests();\n", ""
        )
        _, failures = soak.inventory_failures(
            self.sources, self.cmake, changed_main
        )
        self.assertTrue(any(
            "register_econ_sim_invariant_tests" in failure
            and "not invoked" in failure
            for failure in failures
        ), failures)

    def test_tag_outside_registry_is_rejected(self) -> None:
        changed = dict(self.sources)
        target = soak.ROOT / "tests" / "c" / "test_econ_sim.c"
        changed[target] += "\nRUN_SOAK(test_orphan_soak);\n"
        _, failures = soak.inventory_failures(
            changed, self.cmake, self.test_main
        )
        self.assertTrue(any(
            "RUN_SOAK(test_orphan_soak)" in failure
            and "not inside exactly one" in failure
            for failure in failures
        ), failures)


class SoakAutomationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = {
            relative_path: (soak.ROOT / relative_path).read_text(
                encoding="utf-8"
            )
            for relative_path in soak.AUTOMATION_PATHS
            if (soak.ROOT / relative_path).is_file()
        }

    def test_current_automation_references_every_soak_lane(self) -> None:
        self.assertEqual(
            soak.automation_contract_failures(self.sources), []
        )

    def test_pull_request_soak_lane_is_required(self) -> None:
        changed = dict(self.sources)
        path = ".github/workflows/ci.yml"
        changed[path] = changed[path].replace(
            "  soak:\n", "  optional-soak:\n", 1
        )
        failures = soak.automation_contract_failures(changed)
        self.assertTrue(any(
            path in failure and "dedicated soak job" in failure
            for failure in failures
        ), failures)

    def test_soak_aggregate_count_guard_cannot_disappear(self) -> None:
        changed = dict(self.sources)
        path = "Makefile"
        changed[path] = changed[path].replace(
            "test-soak: TEST_EXPECTED_COUNT=$(SOAK_TEST_COUNT)\n",
            "",
            1,
        )
        failures = soak.automation_contract_failures(changed)
        self.assertTrue(any(
            path in failure and "live tag count" in failure
            for failure in failures
        ), failures)

    def test_scheduled_sanitizer_soak_cannot_disappear(self) -> None:
        changed = dict(self.sources)
        path = ".github/workflows/soak.yml"
        changed[path] = changed[path].replace(
            "make test-san-soak", "make test-san"
        )
        failures = soak.automation_contract_failures(changed)
        self.assertTrue(any(
            path in failure and "scheduled workflow runs test-san-soak"
            in failure
            for failure in failures
        ), failures)

    def test_release_soak_gate_cannot_disappear(self) -> None:
        changed = dict(self.sources)
        path = ".github/workflows/release.yml"
        changed[path] = changed[path].replace(
            "          make test-soak\n", "", 1
        )
        failures = soak.automation_contract_failures(changed)
        self.assertTrue(any(
            path in failure and "release verification" in failure
            for failure in failures
        ), failures)


if __name__ == "__main__":
    unittest.main()
