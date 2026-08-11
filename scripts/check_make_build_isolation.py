#!/usr/bin/env python3
"""Fail if the headless-server Make target can reuse a non-server CMake cache."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MAKEFILE = ROOT / "Makefile"


def recipe(text: str, target: str) -> str | None:
    lines = text.splitlines()
    marker = f"{target}:"
    for index, line in enumerate(lines):
        if line == marker or line.startswith(marker + " "):
            body: list[str] = []
            for following in lines[index + 1 :]:
                if following.startswith("\t"):
                    body.append(following)
                    continue
                if not following.strip() or following.lstrip().startswith("#"):
                    continue
                break
            return "\n".join(body)
    return None


def build_isolation_failures(text: str) -> list[str]:
    failures: list[str] = []
    required_globals = (
        "SERVER_BUILD_DIR ?= build-server",
        "SERVER_BUILD_BIN := $(SERVER_BUILD_DIR)/signal_server",
        "SERVER_COMPAT_BIN := build/signal_server",
    )
    for required in required_globals:
        if required not in text:
            failures.append(f"Makefile is missing `{required}`")

    body = recipe(text, "build-server")
    if body is None:
        return failures + ["Makefile is missing the build-server target"]

    required_recipe_parts = (
        "-B $(SERVER_BUILD_DIR)",
        "-DBUILD_TESTS_ONLY=OFF",
        "-DBUILD_SERVER_ONLY=ON",
        "-DBUILD_TOOLS=OFF",
        "cmake --build $(SERVER_BUILD_DIR) --target signal_server",
        "cmake -E copy_if_different $(SERVER_BUILD_BIN) $(SERVER_COMPAT_BIN)",
    )
    for required in required_recipe_parts:
        if required not in body:
            failures.append(
                f"build-server recipe is missing `{required}`"
            )

    if "-B build " in body or "cmake --build build " in body:
        failures.append(
            "build-server reuses the shared build/ CMake cache"
        )
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "makefile",
        nargs="?",
        type=Path,
        default=DEFAULT_MAKEFILE,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        text = args.makefile.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"build-isolation check could not read {args.makefile}: {exc}",
              file=sys.stderr)
        return 2

    failures = build_isolation_failures(text)
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("Makefile build-mode isolation check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
