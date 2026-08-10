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
        cls.scripts = contract.script_sources()
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
        return contract.contract_failures(
            changed, self.policy, self.scripts
        )

    def test_current_contract_passes(self) -> None:
        self.assertEqual(
            contract.contract_failures(
                self.sources, self.policy, self.scripts
            ),
            [],
        )

    def test_policy_node_dependency_cannot_become_a_static_import(
        self,
    ) -> None:
        changed_scripts = dict(self.scripts)
        path = "scripts/ws-backpressure-soak.mjs"
        changed_scripts[path] = (
            "import WebSocket from 'ws';\n" + changed_scripts[path]
        )
        failures = contract.contract_failures(
            self.sources, self.policy, changed_scripts
        )
        self.assertTrue(any(
            "imports external package 'ws'" in failure
            for failure in failures
        ), failures)

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

    def test_relay_probe_self_test_cannot_silently_disappear(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "          node --test "
                "scripts/test-relay-traffic-probe.mjs\n"
            ),
            after="",
        )
        self.assertTrue(any(
            "test-relay-traffic-probe.mjs" in failure
            for failure in failures
        ), failures)

    def test_memzero_checker_self_test_cannot_silently_disappear(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "          python3 "
                "scripts/test_check_memzero_codegen.py\n"
            ),
            after="",
        )
        self.assertTrue(any(
            "test_check_memzero_codegen.py" in failure
            for failure in failures
        ), failures)

    def test_memory_budget_self_test_cannot_silently_disappear(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "          python3 "
                "scripts/test_check_client_memory_budget.py\n"
            ),
            after="",
        )
        self.assertTrue(any(
            "test_check_client_memory_budget.py" in failure
            for failure in failures
        ), failures)

    def test_browser_lane_cannot_drop_memory_budget(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "          python3 scripts/check_client_memory_budget.py \\\n"
                "            build-web/signal.wasm\n"
            ),
            after="",
        )
        self.assertTrue(any(
            "release WASM memory budget" in failure
            for failure in failures
        ), failures)

    def test_native_lane_cannot_drop_memzero_codegen_gate(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="          make memzero-codegen\n",
            after="",
        )
        self.assertTrue(any(
            "optimized explicit-wipe codegen gate" in failure
            for failure in failures
        ), failures)

    def test_policy_lane_cannot_drop_cargo_trust_caller_audit(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "          python3 "
                "scripts/check_cargo_trust_boundaries.py\n"
            ),
            after="",
        )
        self.assertTrue(any(
            "check_cargo_trust_boundaries.py" in failure
            for failure in failures
        ), failures)

    def test_scheduled_msan_gate_cannot_disappear(self) -> None:
        failures = self.failures(
            workflow="soak.yml",
            before="        run: make memzero-codegen test-msan\n",
            after="        run: make memzero-codegen\n",
        )
        self.assertTrue(any(
            "MemorySanitizer lifecycle gate" in failure
            for failure in failures
        ), failures)

    def test_scheduled_tsan_gate_cannot_disappear(self) -> None:
        failures = self.failures(
            workflow="soak.yml",
            before="          make test-tsan \\\n",
            after="          make test \\\n",
        )
        self.assertTrue(any(
            "bounded ThreadSanitizer gate" in failure
            for failure in failures
        ), failures)

    def test_container_lane_cannot_drop_gateway_self_test(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="            -ec 'npm run test:rtc-gateway'\n",
            after="            -ec 'npm run optional-gateway-demo'\n",
        )
        self.assertTrue(any(
            "test:rtc-gateway" in failure for failure in failures
        ), failures)

    def test_container_lane_cannot_drop_dependency_tree_evidence(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="            ls --omit=dev --all \\\n",
            after="            ls --depth=0 \\\n",
        )
        self.assertTrue(any(
            "recorded production dependency tree" in failure
            for failure in failures
        ), failures)

    def test_container_lane_cannot_drop_production_audit(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="            audit --omit=dev \\\n",
            after="            fund \\\n",
        )
        self.assertTrue(any(
            "production dependency audit" in failure
            for failure in failures
        ), failures)

    def test_container_lane_cannot_drop_dependency_evidence_upload(
        self,
    ) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "          name: production-container-dependencies-"
                "${{ github.sha }}\n"
            ),
            after="          name: optional-container-log\n",
        )
        self.assertTrue(any(
            "uploaded dependency and audit evidence" in failure
            for failure in failures
        ), failures)

    def test_container_evidence_pipelines_require_pipefail(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="          set -o pipefail\n",
            after="",
        )
        self.assertTrue(any(
            "container evidence pipelines must enable pipefail" in failure
            for failure in failures
        ), failures)

    def test_valgrind_pipeline_requires_pipefail(self) -> None:
        failures = self.failures(
            workflow="valgrind.yml",
            before="          set -o pipefail\n",
            after="",
        )
        self.assertTrue(any(
            "memcheck pipeline must enable pipefail" in failure
            for failure in failures
        ), failures)

    def test_deployment_lane_cannot_drop_locked_lambda_install(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before="          npm ci --ignore-scripts --omit=dev \\\n",
            after="          npm install --omit=dev \\\n",
        )
        self.assertTrue(any(
            "locked Lambda production dependency install" in failure
            for failure in failures
        ), failures)

    def test_deployment_paths_cannot_claim_container_coverage(self) -> None:
        changed_policy = copy.deepcopy(self.policy)
        for category in changed_policy["categories"]:
            if category["name"] == "deployment-config":
                category["lanes"].append("container")
        failures = contract.contract_failures(
            self.sources, changed_policy, self.scripts
        )
        self.assertTrue(any(
            "must not claim production-container coverage" in failure
            for failure in failures
        ), failures)

    def test_every_shipping_category_lane_set_is_exact(self) -> None:
        for name, expected in sorted(
            contract.EXPECTED_SHIPPING_LANES.items()
        ):
            changed_policy = copy.deepcopy(self.policy)
            category = next(
                item
                for item in changed_policy["categories"]
                if item["name"] == name
            )
            removed = sorted(expected)[0]
            category["lanes"].remove(removed)
            with self.subTest(category=name, removed=removed):
                failures = contract.contract_failures(
                    self.sources, changed_policy, self.scripts
                )
                self.assertTrue(any(
                    f"shipping category {name!r} must map exactly" in failure
                    for failure in failures
                ), failures)

    def test_new_shipping_category_requires_an_explicit_lane_contract(
        self,
    ) -> None:
        changed_policy = copy.deepcopy(self.policy)
        changed_policy["categories"].append({
            "name": "new-shipping-surface",
            "scope": "shipping",
            "paths": ["new-runtime/**"],
            "lanes": ["native"],
        })
        failures = contract.contract_failures(
            self.sources, changed_policy, self.scripts
        )
        self.assertTrue(any(
            "has no pinned expected lane set" in failure
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

    def test_every_aggregate_result_mapping_is_exact(self) -> None:
        for lane in sorted(self.policy["lanes"]):
            suffix = lane.upper()
            expected = (
                f"      CI_RESULT_{suffix}: "
                f"${{{{ needs.{lane}.result }}}}\n"
            )
            wrong_source = "classify" if lane == "policy" else "policy"
            replacement = (
                f"      CI_RESULT_{suffix}: "
                f"${{{{ needs.{wrong_source}.result }}}}\n"
            )
            with self.subTest(lane=lane):
                failures = self.failures(
                    workflow="ci.yml",
                    before=expected,
                    after=replacement,
                )
                self.assertTrue(any(
                    f"CI_RESULT_{suffix}" in failure
                    for failure in failures
                ), failures)

    def test_every_aggregate_selection_mapping_is_exact(self) -> None:
        for lane in sorted(self.policy["lanes"]):
            suffix = lane.upper()
            expected = (
                f"      CI_SELECTED_{suffix}: "
                f"${{{{ needs.classify.outputs.{lane} }}}}\n"
            )
            wrong_output = "native" if lane != "native" else "policy"
            replacement = (
                f"      CI_SELECTED_{suffix}: "
                "${{ needs.classify.outputs."
                f"{wrong_output} }}}}\n"
            )
            with self.subTest(lane=lane):
                failures = self.failures(
                    workflow="ci.yml",
                    before=expected,
                    after=replacement,
                )
                self.assertTrue(any(
                    f"CI_SELECTED_{suffix}" in failure
                    for failure in failures
                ), failures)

    def test_classifier_result_mapping_is_exact(self) -> None:
        failures = self.failures(
            workflow="ci.yml",
            before=(
                "      CI_RESULT_CLASSIFY: "
                "${{ needs.classify.result }}\n"
            ),
            after=(
                "      CI_RESULT_CLASSIFY: "
                "${{ needs.policy.result }}\n"
            ),
        )
        self.assertTrue(any(
            "CI_RESULT_CLASSIFY" in failure
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

    def test_release_cannot_drop_cargo_trust_caller_audit(self) -> None:
        failures = self.failures(
            workflow="release.yml",
            before="make cargo-trust-audit banned-apis",
            after="make banned-apis",
        )
        self.assertTrue(any(
            "release.yml" in failure and "cargo trust" in failure
            for failure in failures
        ), failures)

    def test_deploy_cannot_drop_cargo_trust_caller_audit(self) -> None:
        failures = self.failures(
            workflow="deploy-fly.yml",
            before="make cargo-trust-audit banned-apis",
            after="make banned-apis",
        )
        self.assertTrue(any(
            "deploy-fly.yml" in failure and "cargo trust" in failure
            for failure in failures
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
            self.sources, changed_policy, self.scripts
        )
        self.assertTrue(any(
            "web/package/Playwright inputs" in failure
            for failure in failures
        ), failures)


if __name__ == "__main__":
    unittest.main()
