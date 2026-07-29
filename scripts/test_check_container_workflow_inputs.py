#!/usr/bin/env python3
"""Mutation coverage for production-container workflow input filters."""

import unittest

import check_container_workflow_inputs as checker


class ContainerWorkflowInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.dockerfile = checker.DOCKERFILE.read_text(encoding="utf-8")
        cls.policy = checker.classify_ci_paths.load_policy(
            checker.CI_PATH_POLICY
        )
        cls.workflows = {
            path: path.read_text(encoding="utf-8")
            for path in checker.WORKFLOWS
        }

    def test_ci_classifier_selects_container_for_every_input(self) -> None:
        self.assertEqual(
            checker.container_ci_lane_failures(
                self.dockerfile, self.policy
            ),
            [],
        )

    def test_production_dependency_layer_is_lock_enforced(self) -> None:
        self.assertEqual(
            checker.production_dependency_contract_failures(self.dockerfile),
            [],
        )

    def test_missing_lockfile_copy_is_rejected(self) -> None:
        changed = self.dockerfile.replace(
            "COPY package.json package-lock.json ./",
            "COPY package.json ./",
        )
        failures = checker.production_dependency_contract_failures(changed)
        self.assertTrue(any(
            "package.json and package-lock.json together" in failure
            for failure in failures
        ), failures)

    def test_non_locking_production_install_is_rejected(self) -> None:
        changed = self.dockerfile.replace(
            "RUN npm ci --omit=dev",
            "RUN npm install --omit=dev",
        )
        failures = checker.production_dependency_contract_failures(changed)
        self.assertTrue(any(
            "npm ci --omit=dev" in failure for failure in failures
        ), failures)

    def test_production_install_must_remove_npm_cache(self) -> None:
        changed = self.dockerfile.replace(
            "npm cache clean --force",
            "npm cache verify",
        )
        failures = checker.production_dependency_contract_failures(changed)
        self.assertTrue(any(
            "clean the npm cache" in failure for failure in failures
        ), failures)

    def test_production_install_must_remove_npm_cache_directory(self) -> None:
        changed = self.dockerfile.replace(
            "    && rm -rf /root/.npm",
            "    && true",
        )
        failures = checker.production_dependency_contract_failures(changed)
        self.assertTrue(any(
            "remove /root/.npm" in failure for failure in failures
        ), failures)

    def test_current_workflows_cover_every_container_input(self) -> None:
        for path, source in self.workflows.items():
            self.assertEqual(
                checker.container_workflow_input_failures(
                    self.dockerfile, source, str(path)),
                [],
            )

    def test_missing_asset_filter_is_rejected(self) -> None:
        path = checker.ROOT / ".github" / "workflows" / "deploy-fly.yml"
        changed = self.workflows[path].replace("      - 'assets/**'\n", "")
        failures = checker.container_workflow_input_failures(
            self.dockerfile, changed, str(path))
        self.assertTrue(
            any("'assets/**'" in failure for failure in failures), failures)

    def test_missing_exact_runtime_script_filter_is_rejected(self) -> None:
        path = checker.ROOT / ".github" / "workflows" / "deploy-fly.yml"
        changed = self.workflows[path].replace(
            "      - 'scripts/fly-webrtc-entrypoint.sh'\n", "")
        failures = checker.container_workflow_input_failures(
            self.dockerfile, changed, str(path))
        self.assertTrue(any(
            "'scripts/fly-webrtc-entrypoint.sh'" in failure
            for failure in failures
        ), failures)

    def test_new_copy_source_requires_a_matching_filter(self) -> None:
        path = checker.ROOT / ".github" / "workflows" / "deploy-fly.yml"
        changed_dockerfile = (
            self.dockerfile + "\nCOPY config/runtime.json ./config.json\n")
        failures = checker.container_workflow_input_failures(
            changed_dockerfile, self.workflows[path], str(path))
        self.assertTrue(
            any("'config/runtime.json'" in failure for failure in failures),
            failures,
        )

    def test_classified_non_container_copy_source_is_rejected(self) -> None:
        path = "docs/runtime.json"
        classification = checker.classify_ci_paths.classify_paths(
            [path], self.policy
        )
        self.assertEqual(classification.unknown, ())
        self.assertNotIn("container", classification.lanes)

        changed_dockerfile = (
            self.dockerfile + f"\nCOPY {path} ./runtime.json\n"
        )
        failures = checker.container_ci_lane_failures(
            changed_dockerfile, self.policy
        )
        self.assertTrue(
            any(
                repr(path) in failure and "'container'" in failure
                for failure in failures
            ),
            failures,
        )

    def test_unfiltered_pull_request_needs_no_yaml_path_filter(self) -> None:
        workflow = "on:\n  pull_request:\n  workflow_dispatch:\n"
        changed_dockerfile = (
            self.dockerfile + "\nCOPY future/runtime.json ./runtime.json\n"
        )
        self.assertTrue(checker.pull_request_is_unfiltered(workflow))
        self.assertEqual(
            checker.container_workflow_input_failures(
                changed_dockerfile, workflow, "fixture.yml"
            ),
            [],
        )

    def test_filtered_pull_request_does_not_claim_full_coverage(self) -> None:
        workflow = (
            "on:\n"
            "  pull_request:\n"
            "    paths:\n"
            "      - 'server/**'\n"
        )
        self.assertFalse(checker.pull_request_is_unfiltered(workflow))
        failures = checker.container_workflow_input_failures(
            self.dockerfile, workflow, "fixture.yml"
        )
        self.assertTrue(failures)

    def test_directory_filter_covers_exact_files(self) -> None:
        dockerfile = "FROM example\nCOPY scripts/start.sh ./start.sh\n"
        workflow = "on:\n  push:\n    paths:\n      - 'scripts/**'\n"
        failures = checker.container_workflow_input_failures(
            dockerfile, workflow, "fixture.yml")
        self.assertEqual(failures, [
            "fixture.yml: Docker build input '.dockerignore' is not covered "
            "by an event path filter",
            "fixture.yml: Docker build input 'server/Dockerfile' is not "
            "covered by an event path filter",
        ])


if __name__ == "__main__":
    unittest.main()
