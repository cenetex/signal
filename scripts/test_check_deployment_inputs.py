#!/usr/bin/env python3
"""Mutation coverage for deployment package and config contracts."""

from __future__ import annotations

import copy
import json
import tomllib
import unittest

import check_deployment_inputs as deployment


class DeploymentInputContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.package = json.loads(
            deployment.LAMBDA_PACKAGE.read_text(encoding="utf-8")
        )
        cls.lock = json.loads(
            deployment.LAMBDA_LOCK.read_text(encoding="utf-8")
        )
        cls.template_source = deployment.LAMBDA_TEMPLATE.read_text(
            encoding="utf-8"
        )
        cls.lambda_source = deployment.LAMBDA_SOURCE.read_text(
            encoding="utf-8"
        )
        cls.wrangler = tomllib.loads(
            deployment.WRANGLER_CONFIG.read_text(encoding="utf-8")
        )
        cls.worker_source = deployment.WORKER_SOURCE.read_text(
            encoding="utf-8"
        )
        cls.worker_package = json.loads(
            deployment.WORKER_PACKAGE.read_text(encoding="utf-8")
        )

    def failures(
        self,
        *,
        package=None,
        lock=None,
        template_source=None,
        lambda_source=None,
        wrangler=None,
        worker_source=None,
        worker_package=None,
    ) -> list[str]:
        return deployment.deployment_failures(
            self.package if package is None else package,
            self.lock if lock is None else lock,
            (
                self.template_source
                if template_source is None
                else template_source
            ),
            self.lambda_source if lambda_source is None else lambda_source,
            self.wrangler if wrangler is None else wrangler,
            self.worker_source if worker_source is None else worker_source,
            (
                self.worker_package
                if worker_package is None
                else worker_package
            ),
        )

    def test_current_deployment_inputs_pass(self) -> None:
        self.assertEqual(self.failures(), [])

    def test_lambda_dependencies_must_match_lock_root(self) -> None:
        changed = copy.deepcopy(self.package)
        changed["dependencies"]["new-runtime-package"] = "1.0.0"
        failures = self.failures(package=changed)
        self.assertTrue(any(
            "exactly match" in failure for failure in failures
        ), failures)

    def test_direct_lambda_dependency_must_be_locked(self) -> None:
        changed = copy.deepcopy(self.lock)
        del changed["packages"][
            "node_modules/@aws-sdk/client-dynamodb"
        ]
        failures = self.failures(lock=changed)
        self.assertTrue(any(
            "client-dynamodb" in failure for failure in failures
        ), failures)

    def test_sam_handler_cannot_drift_from_module_export(self) -> None:
        changed = self.template_source.replace(
            "Handler: index.handler",
            "Handler: index.main",
        )
        failures = self.failures(template_source=changed)
        self.assertTrue(any(
            "index.handler" in failure for failure in failures
        ), failures)

    def test_deprecated_sam_runtime_cannot_return(self) -> None:
        changed = self.template_source.replace(
            "Runtime: nodejs24.x",
            "Runtime: nodejs20.x",
        )
        failures = self.failures(template_source=changed)
        self.assertTrue(any(
            "nodejs24.x" in failure for failure in failures
        ), failures)

    def test_lambda_handler_export_cannot_disappear(self) -> None:
        changed = self.lambda_source.replace(
            "export async function handler(event)",
            "async function handler(event)",
        )
        failures = self.failures(lambda_source=changed)
        self.assertTrue(any(
            "export async handler" in failure for failure in failures
        ), failures)

    def test_wrangler_main_must_name_tracked_worker(self) -> None:
        changed = copy.deepcopy(self.wrangler)
        changed["main"] = "workers/missing.js"
        failures = self.failures(wrangler=changed)
        self.assertTrue(any(
            "workers/fly-proxy.js" in failure for failure in failures
        ), failures)

    def test_worker_fetch_handler_cannot_disappear(self) -> None:
        changed = self.worker_source.replace("async fetch(", "async proxy(")
        failures = self.failures(worker_source=changed)
        self.assertTrue(any(
            "async fetch" in failure for failure in failures
        ), failures)

    def test_worker_must_remain_explicit_esm(self) -> None:
        changed = copy.deepcopy(self.worker_package)
        changed["type"] = "commonjs"
        failures = self.failures(worker_package=changed)
        self.assertTrue(any(
            "type=module" in failure for failure in failures
        ), failures)


if __name__ == "__main__":
    unittest.main()
