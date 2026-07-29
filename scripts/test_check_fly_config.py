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
        cls.worker_source = fly.WORKER_SOURCE.read_text(encoding="utf-8")

    def failures_after(self, mutate, *, worker_source=None) -> list[str]:
        changed = copy.deepcopy(self.config)
        mutate(changed)
        return fly.fly_config_failures(
            changed,
            self.worker_source if worker_source is None else worker_source,
        )

    def test_current_config_matches_runtime_contract(self) -> None:
        self.assertEqual(
            fly.fly_config_failures(self.config, self.worker_source),
            [],
        )

    def test_runtime_critical_environment_cannot_drift(self) -> None:
        expected = {
            "SIGNAL_ALLOWED_ORIGIN": "https://signal.ratimics.com",
            "SIGNAL_REQUIRE_STATION_AUTH_SECRET": "1",
            "SIGNAL_TRUST_PROXY_HEADERS": "1",
            "RTC_GATEWAY_PREFIX": "/rtc",
            "RTC_GATEWAY_ICE_BIND": "fly-global-services",
            "RTC_GATEWAY_ICE_UDP_MUX": "1",
        }
        for key, value in expected.items():
            with self.subTest(key=key):
                failures = self.failures_after(
                    lambda config, key=key: config["env"].update(
                        {key: "mutated"}
                    )
                )
                self.assertIn(
                    f"[env].{key} must be {value!r}",
                    failures,
                )

    def test_worker_origin_must_match_fly_app(self) -> None:
        changed_worker = self.worker_source.replace(
            "https://signal-relay-kind-pond-4338.fly.dev",
            "https://wrong-origin.fly.dev",
        )
        failures = self.failures_after(
            lambda config: None,
            worker_source=changed_worker,
        )
        self.assertIn(
            "workers/fly-proxy.js ORIGIN must be "
            "'https://signal-relay-kind-pond-4338.fly.dev'",
            failures,
        )

    def test_fly_app_must_match_worker_origin(self) -> None:
        failures = self.failures_after(
            lambda config: config.update(app="signal-other-app")
        )
        self.assertIn(
            "workers/fly-proxy.js ORIGIN must be "
            "'https://signal-other-app.fly.dev'",
            failures,
        )

    def test_worker_origin_must_remain_a_single_literal(self) -> None:
        changed_worker = self.worker_source.replace(
            "const ORIGIN =",
            "const UPSTREAM_ORIGIN =",
        )
        failures = self.failures_after(
            lambda config: None,
            worker_source=changed_worker,
        )
        self.assertIn(
            "workers/fly-proxy.js must declare exactly one literal ORIGIN",
            failures,
        )

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
