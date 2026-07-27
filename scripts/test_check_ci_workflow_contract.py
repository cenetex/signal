#!/usr/bin/env python3
"""Mutation coverage for required CI and release workflow contracts."""

from __future__ import annotations

import copy
import unittest

import check_ci_workflow_contract as contract
import classify_ci_paths


class WorkflowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = contract.workflow_sources()
        cls.policy = classify_ci_paths.load_policy()

    def failures(
        self,
        *,
        workflow: str,
        before: str,
        after: str,
    ) -> list[str]:
        changed = dict(self.sources)
        changed[workflow] = changed[workflow].replace(before, after, 1)
        self.assertNotEqual(changed[workflow], self.sources[workflow])
        return contract.contract_failures(changed, self.policy)

    def test_current_contract_passes(self) -> None:
        self.assertEqual(
            contract.contract_failures(self.sources, self.policy), []
        )

    def test_pr_path_filter_cannot_suppress_required_check(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="  pull_request:\n",
            after=(
                "  pull_request:\n"
                "    paths:\n"
                "      - 'server/**'\n"
            ),
        )
        self.assertTrue(any(
            "pull_request must be unfiltered" in failure
            for failure in failures
        ), failures)

    def test_browser_lane_cannot_drop_chromium_smoke(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="        run: npm run smoke\n",
            after="        run: npm run optional-browser-demo\n",
        )
        self.assertTrue(any(
            "Chromium smoke suite" in failure for failure in failures
        ), failures)

    def test_cross_platform_lane_cannot_drop_server_target(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="--target signal signal_server --parallel",
            after="--target signal --parallel",
        )
        self.assertTrue(any(
            "'--target signal signal_server'" in failure
            for failure in failures
        ), failures)

    def test_policy_lane_must_validate_local_fly_contract(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="          python3 scripts/check_fly_config.py\n",
            after="",
        )
        self.assertTrue(any(
            "check_fly_config.py" in failure
            for failure in failures
        ), failures)

    def test_script_self_test_cannot_silently_disappear(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="          node scripts/test-relay-region-broker.mjs\n",
            after="",
        )
        self.assertTrue(any(
            "test-relay-region-broker.mjs" in failure
            for failure in failures
        ), failures)

    def test_aggregate_cannot_drop_selected_lane(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="      - fuzz\n",
            after="",
        )
        self.assertTrue(any(
            "ci-required does not need 'fuzz'" in failure
            for failure in failures
        ), failures)

    def test_program_manifests_must_remain_locked(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="            --locked\n",
            after="",
        )
        self.assertTrue(any(
            "cargo check/test must be locked" in failure
            for failure in failures
        ), failures)

    def test_published_release_trigger_is_rejected(self) -> None:
        failures = self.failures(
            workflow="release.yml",
            before="  push:\n",
            after="  release:\n    types: [published]\n  push:\n",
        )
        self.assertTrue(any(
            "published-event artifact builds are forbidden" in failure
            for failure in failures
        ), failures)

    def test_dispatch_release_ref_drives_wasm_version(self) -> None:
        failures = self.failures(
            workflow="release.yml",
            before=(
                "          RELEASE_REF: "
                "${{ inputs.release_ref || github.ref_name }}\n"
            ),
            after="          RELEASE_REF: ${{ github.ref_name }}\n",
        )
        self.assertTrue(any(
            "RELEASE_REF:" in failure for failure in failures
        ), failures)

    def test_floating_third_party_action_is_rejected(self) -> None:
        failures = self.failures(
            workflow="deploy-fly.yml",
            before=(
                "superfly/flyctl-actions/setup-flyctl@"
                "ed8efb33836e8b2096c7fd3ba1c8afe303ebbff1"
            ),
            after="superfly/flyctl-actions/setup-flyctl@master",
        )
        self.assertTrue(any(
            "floating reference @master" in failure
            for failure in failures
        ), failures)

    def test_playwright_path_mapping_cannot_disappear(self) -> None:
        changed_policy = copy.deepcopy(self.policy)
        for category in changed_policy["categories"]:
            if category["name"] == "browser-test-config":
                category["paths"].remove("playwright.config.ts")
        failures = contract.contract_failures(
            self.sources, changed_policy
        )
        self.assertTrue(any(
            "web/package/Playwright inputs" in failure
            for failure in failures
        ), failures)


if __name__ == "__main__":
    unittest.main()
