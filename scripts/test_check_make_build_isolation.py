#!/usr/bin/env python3
"""Mutation tests for the Makefile server-build isolation contract."""

from __future__ import annotations

import unittest

import check_make_build_isolation as checker


GOOD = """
SERVER_BUILD_DIR ?= build-server
SERVER_BUILD_BIN := $(SERVER_BUILD_DIR)/signal_server
SERVER_COMPAT_BIN := build/signal_server

build-server:
\tcmake -S . -B $(SERVER_BUILD_DIR) \\
\t\t-DBUILD_TESTS_ONLY=OFF -DBUILD_SERVER_ONLY=ON -DBUILD_TOOLS=OFF
\tcmake --build $(SERVER_BUILD_DIR) --target signal_server --parallel
\tcmake -E copy_if_different $(SERVER_BUILD_BIN) $(SERVER_COMPAT_BIN)

next-target:
\ttrue
"""


class BuildIsolationTests(unittest.TestCase):
    def test_isolated_server_cache_passes(self) -> None:
        self.assertEqual(checker.build_isolation_failures(GOOD), [])

    def test_shared_configure_cache_is_rejected(self) -> None:
        mutated = GOOD.replace(
            "-B $(SERVER_BUILD_DIR)", "-B build", 1
        )
        failures = checker.build_isolation_failures(mutated)
        self.assertTrue(any("shared build/" in item for item in failures))

    def test_test_only_override_is_required(self) -> None:
        mutated = GOOD.replace("-DBUILD_TESTS_ONLY=OFF ", "")
        failures = checker.build_isolation_failures(mutated)
        self.assertTrue(any(
            "BUILD_TESTS_ONLY=OFF" in item for item in failures
        ))

    def test_server_target_must_use_the_isolated_cache(self) -> None:
        mutated = GOOD.replace(
            "cmake --build $(SERVER_BUILD_DIR)",
            "cmake --build another-cache",
        )
        failures = checker.build_isolation_failures(mutated)
        self.assertTrue(any(
            "cmake --build $(SERVER_BUILD_DIR)" in item
            for item in failures
        ))

    def test_compatibility_copy_is_required(self) -> None:
        mutated = GOOD.replace(
            "\tcmake -E copy_if_different "
            "$(SERVER_BUILD_BIN) $(SERVER_COMPAT_BIN)\n",
            "",
        )
        failures = checker.build_isolation_failures(mutated)
        self.assertTrue(any("copy_if_different" in item
                            for item in failures))


if __name__ == "__main__":
    unittest.main()
