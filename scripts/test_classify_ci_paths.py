#!/usr/bin/env python3
"""Mutation coverage for the required-check path classifier."""

from __future__ import annotations

import copy
import subprocess
import tempfile
import unittest
from pathlib import Path

import classify_ci_paths as classifier


class CiPathPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policy = classifier.load_policy()

    def lanes_for(self, path: str) -> set[str]:
        result = classifier.classify_paths([path], self.policy)
        self.assertEqual(result.unknown, ())
        return set(result.lanes)

    def test_every_repository_path_is_intentionally_classified(self) -> None:
        result = classifier.classify_paths(
            classifier.tracked_paths(), self.policy
        )
        self.assertEqual(result.unknown, ())

    def test_web_package_and_playwright_paths_run_browser_smoke(self) -> None:
        for path in (
            "web/play.html",
            "package.json",
            "package-lock.json",
            "playwright.config.ts",
            "tests/browser-smoke.spec.ts",
        ):
            self.assertIn("browser", self.lanes_for(path), path)

    def test_vendor_crypto_runs_native_sanitizer_and_fuzz_lane(self) -> None:
        lanes = self.lanes_for("vendor/tweetnacl/tweetnacl.c")
        self.assertTrue({"native", "fuzz"}.issubset(lanes), lanes)

    def test_memzero_codegen_gate_runs_native_lane(self) -> None:
        for path in (
            "scripts/check_memzero_codegen.py",
            "scripts/test_check_memzero_codegen.py",
        ):
            self.assertIn("native", self.lanes_for(path), path)

    def test_build_mode_guard_runs_policy_lane(self) -> None:
        for path in (
            "scripts/check_make_build_isolation.py",
            "scripts/test_check_make_build_isolation.py",
        ):
            self.assertIn("policy", self.lanes_for(path), path)

    def test_memzero_runtime_runs_native_and_fuzz_lanes(self) -> None:
        for path in (
            "shared/signal_memzero.c",
            "shared/signal_memzero.h",
        ):
            lanes = self.lanes_for(path)
            self.assertTrue({"native", "fuzz"}.issubset(lanes), (path, lanes))

    def test_container_inputs_run_clean_image_lane(self) -> None:
        for path in (
            ".dockerignore",
            "server/Dockerfile",
            "package-lock.json",
            "scripts/webrtc-gateway.mjs",
        ):
            self.assertIn("container", self.lanes_for(path), path)

    def test_deployment_inputs_run_honest_deployment_lane(self) -> None:
        for path in (
            "aws/lambda/signal-lobby/package-lock.json",
            "deploy/lightsail-user-data.sh",
            "docker/entrypoint.sh",
            "fly.toml",
            "workers/fly-proxy.js",
            "wrangler.toml",
        ):
            lanes = self.lanes_for(path)
            self.assertIn("deployment", lanes, path)
            self.assertNotIn("container", lanes, path)

    def test_core_change_compiles_all_supported_platforms(self) -> None:
        self.assertIn(
            "cross_platform", self.lanes_for("client/main.c")
        )

    def test_scripts_and_workflows_always_run_policy_lane(self) -> None:
        for path in (
            "scripts/check_doc_freshness.py",
            ".github/workflows/valgrind.yml",
        ):
            self.assertIn("policy", self.lanes_for(path), path)

    def test_non_shipping_docs_still_receive_required_policy_check(self) -> None:
        lanes = self.lanes_for("docs/c_safety_policy.md")
        self.assertEqual(lanes, {"policy"})

    def test_unknown_top_level_path_fails_closed(self) -> None:
        result = classifier.classify_paths(
            ["new-release-input.toml"], self.policy
        )
        self.assertEqual(result.unknown, ("new-release-input.toml",))

    def test_playwright_mapping_mutation_is_detected(self) -> None:
        changed = copy.deepcopy(self.policy)
        for category in changed["categories"]:
            if category["name"] == "browser-test-config":
                category["paths"].remove("playwright.config.ts")
        result = classifier.classify_paths(["playwright.config.ts"], changed)
        self.assertEqual(result.unknown, ("playwright.config.ts",))

    def test_unsupported_broad_glob_is_rejected(self) -> None:
        changed = copy.deepcopy(self.policy)
        changed["categories"][0]["paths"].append("*.toml")
        failures = classifier.policy_failures(changed)
        self.assertTrue(any(
            "only directory/** wildcards" in failure
            for failure in failures
        ), failures)

    def test_deleted_runtime_path_still_selects_shipping_lanes(self) -> None:
        changed = self._changed_paths_fixture("delete")
        self.assertEqual(changed, ["server/deleted.c"])
        lanes = set(classifier.classify_paths(changed, self.policy).lanes)
        self.assertTrue({
            "native", "container", "cross_platform"
        }.issubset(lanes), lanes)

    def test_rename_reports_old_and_new_paths_without_rename_folding(
        self,
    ) -> None:
        changed = self._changed_paths_fixture("rename")
        self.assertEqual(
            changed,
            ["docs/renamed.md", "server/renamed.c"],
        )
        lanes = set(classifier.classify_paths(changed, self.policy).lanes)
        self.assertTrue({
            "native", "container", "cross_platform"
        }.issubset(lanes), lanes)

    @staticmethod
    def _changed_paths_fixture(operation: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(
                ["git", "config", "user.email", "ci@example.invalid"],
                cwd=root,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "CI Test"],
                cwd=root,
                check=True,
            )
            source = root / "server" / (
                "deleted.c" if operation == "delete" else "renamed.c"
            )
            source.parent.mkdir()
            source.write_text("int fixture;\n", encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "base"], cwd=root, check=True
            )
            base = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=root,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.strip()
            if operation == "delete":
                source.unlink()
            else:
                destination = root / "docs" / "renamed.md"
                destination.parent.mkdir()
                source.rename(destination)
            subprocess.run(["git", "add", "-A"], cwd=root, check=True)
            subprocess.run(
                ["git", "commit", "-qm", operation], cwd=root, check=True
            )
            head = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=root,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.strip()
            return classifier.changed_paths(base, head, root=root)


if __name__ == "__main__":
    unittest.main()
