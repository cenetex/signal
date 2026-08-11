#!/usr/bin/env python3
"""Validate deployment packages and entrypoint/configuration coupling.

This checker is intentionally credential-free. Platform-side validation stays
in authenticated deploy workflows, while required PR CI proves that local
entrypoints exist, package dependencies are locked, and configs still point at
the code that the deployment lane loads.
"""

from __future__ import annotations

import json
import re
import sys
import tomllib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LAMBDA_DIR = ROOT / "aws" / "lambda" / "signal-lobby"
LAMBDA_PACKAGE = LAMBDA_DIR / "package.json"
LAMBDA_LOCK = LAMBDA_DIR / "package-lock.json"
LAMBDA_TEMPLATE = LAMBDA_DIR / "template.yaml"
LAMBDA_SOURCE = LAMBDA_DIR / "index.mjs"
WRANGLER_CONFIG = ROOT / "wrangler.toml"
WORKER_SOURCE = ROOT / "workers" / "fly-proxy.js"
WORKER_PACKAGE = ROOT / "workers" / "package.json"


def deployment_failures(
    package: dict[str, Any],
    lock: dict[str, Any],
    template_source: str,
    lambda_source: str,
    wrangler: dict[str, Any],
    worker_source: str,
    worker_package: dict[str, Any],
    *,
    root: Path = ROOT,
) -> list[str]:
    failures: list[str] = []

    if package.get("private") is not True:
        failures.append("Lambda package must remain private")
    if package.get("type") != "module":
        failures.append("Lambda package must use ESM (type=module)")
    dependencies = package.get("dependencies")
    if not isinstance(dependencies, dict) or not dependencies:
        failures.append("Lambda package must declare runtime dependencies")
        dependencies = {}

    if lock.get("lockfileVersion") != 3:
        failures.append("Lambda package-lock.json must use lockfileVersion 3")
    packages = lock.get("packages")
    if not isinstance(packages, dict):
        failures.append("Lambda package-lock.json must contain packages")
        packages = {}
    locked_root = packages.get("")
    if not isinstance(locked_root, dict):
        failures.append("Lambda package-lock.json lacks its root package")
        locked_root = {}
    if locked_root.get("dependencies") != dependencies:
        failures.append(
            "Lambda package-lock root dependencies must exactly match "
            "package.json"
        )
    for dependency in sorted(dependencies):
        entry = packages.get(f"node_modules/{dependency}")
        if not isinstance(entry, dict):
            failures.append(
                f"Lambda dependency {dependency!r} has no locked package"
            )
            continue
        if not isinstance(entry.get("version"), str):
            failures.append(
                f"Lambda dependency {dependency!r} lacks a locked version"
            )
        if not isinstance(entry.get("integrity"), str):
            failures.append(
                f"Lambda dependency {dependency!r} lacks package integrity"
            )

    template_requirements = {
        r"(?m)^\s+Runtime:\s+nodejs24\.x\s*$":
            "SAM Lambda runtime must remain nodejs24.x",
        r"(?m)^\s+Handler:\s+index\.handler\s*$":
            "SAM Lambda handler must remain index.handler",
        r"(?m)^\s+CodeUri:\s+\.\s*$":
            "SAM Lambda CodeUri must remain the package directory",
    }
    for pattern, message in template_requirements.items():
        if re.search(pattern, template_source) is None:
            failures.append(message)
    if re.search(
        r"(?m)^export\s+async\s+function\s+handler\s*\(",
        lambda_source,
    ) is None:
        failures.append("Lambda source must export async handler")

    worker_path = wrangler.get("main")
    if worker_path != "workers/fly-proxy.js":
        failures.append(
            "wrangler.toml main must be workers/fly-proxy.js"
        )
    elif not (root / worker_path).is_file():
        failures.append("wrangler.toml main does not exist")
    compatibility_date = wrangler.get("compatibility_date")
    if not isinstance(compatibility_date, str) or re.fullmatch(
        r"\d{4}-\d{2}-\d{2}", compatibility_date
    ) is None:
        failures.append(
            "wrangler.toml compatibility_date must be YYYY-MM-DD"
        )
    routes = wrangler.get("routes")
    if not isinstance(routes, list) or not routes:
        failures.append("wrangler.toml must declare at least one route")
    elif not all(
        isinstance(route, dict)
        and isinstance(route.get("pattern"), str)
        and route["pattern"]
        and isinstance(route.get("zone_name"), str)
        and route["zone_name"]
        for route in routes
    ):
        failures.append(
            "every Wrangler route must declare pattern and zone_name"
        )
    if re.search(r"(?m)^export\s+default\s+\{", worker_source) is None:
        failures.append("Worker source must export its default handler")
    if re.search(r"\basync\s+fetch\s*\(", worker_source) is None:
        failures.append("Worker default handler must expose async fetch")
    if worker_package.get("private") is not True:
        failures.append("Worker package must remain private")
    if worker_package.get("type") != "module":
        failures.append("Worker package must declare type=module")

    return failures


def main() -> int:
    try:
        package = json.loads(LAMBDA_PACKAGE.read_text(encoding="utf-8"))
        lock = json.loads(LAMBDA_LOCK.read_text(encoding="utf-8"))
        template_source = LAMBDA_TEMPLATE.read_text(encoding="utf-8")
        lambda_source = LAMBDA_SOURCE.read_text(encoding="utf-8")
        wrangler = tomllib.loads(
            WRANGLER_CONFIG.read_text(encoding="utf-8")
        )
        worker_source = WORKER_SOURCE.read_text(encoding="utf-8")
        worker_package = json.loads(
            WORKER_PACKAGE.read_text(encoding="utf-8")
        )
    except (
        OSError,
        json.JSONDecodeError,
        tomllib.TOMLDecodeError,
    ) as exc:
        print(f"deployment inputs could not be loaded: {exc}", file=sys.stderr)
        return 2

    failures = deployment_failures(
        package,
        lock,
        template_source,
        lambda_source,
        wrangler,
        worker_source,
        worker_package,
    )
    if failures:
        for failure in failures:
            print(f"deployment inputs: {failure}", file=sys.stderr)
        return 1
    print("deployment input contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
