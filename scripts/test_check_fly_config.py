#!/usr/bin/env python3
"""Mutation coverage for the credential-free Fly config contract."""

from __future__ import annotations

import copy
import tomllib
import unittest

import check_fly_config as fly


class FlyConfigContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = tomllib.loads(
            fly.FLY_TOML.read_text(encoding="utf-8")
        )

    def failures_after(self, mutate) -> list[str]:
        changed = copy.deepcopy(self.config)
        mutate(changed)
        return fly.fly_config_failures(changed)

    def test_current_config_matches_runtime_contract(self) -> None:
        self.assertEqual(fly.fly_config_failures(self.config), [])

    def test_dockerfile_drift_is_rejected(self) -> None:
        failures = self.failures_after(
            lambda config: config["build"].update(
                dockerfile="Dockerfile"
            )
        )
        self.assertTrue(any(
            "server/Dockerfile" in failure for failure in failures
        ), failures)

    def test_http_port_must_match_runtime_env(self) -> None:
        failures = self.failures_after(
            lambda config: config["http_service"].update(
                internal_port=8081
            )
        )
        self.assertTrue(any(
            "internal_port" in failure for failure in failures
        ), failures)

    def test_persistent_mount_must_match_data_directory(self) -> None:
        failures = self.failures_after(
            lambda config: config["mounts"].update(
                destination="/tmp/data"
            )
        )
        self.assertTrue(any(
            "SIGNAL_DATA_DIR" in failure for failure in failures
        ), failures)

    def test_health_check_cannot_disappear(self) -> None:
        failures = self.failures_after(
            lambda config: config["http_service"].update(checks=[])
        )
        self.assertTrue(any("GET /health" in failure for failure in failures))

    def test_udp_ice_mux_must_remain_exposed(self) -> None:
        failures = self.failures_after(
            lambda config: config["services"][0].update(protocol="tcp")
        )
        self.assertTrue(any(
            "over UDP" in failure for failure in failures
        ), failures)


if __name__ == "__main__":
    unittest.main()
