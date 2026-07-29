#!/usr/bin/env python3
"""Enforce the pre-merge and pre-publication workflow architecture."""

from __future__ import annotations

import posixpath
import re
import sys
from pathlib import Path

import check_container_workflow_inputs as container_inputs
import classify_ci_paths


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
REQUIRED_JOBS = {
    "classify",
    "policy",
    "native",
    "soak",
    "fuzz",
    "browser",
    "container",
    "cross_platform",
    "replay",
    "tools",
    "programs",
    "deployment",
    "ci-required",
}

EXPECTED_SHIPPING_LANES = {
    "core-runtime": {
        "native",
        "soak",
        "browser",
        "container",
        "cross_platform",
        "replay",
    },
    "vendored-runtime": {
        "native",
        "soak",
        "fuzz",
        "browser",
        "container",
        "cross_platform",
        "replay",
    },
    "browser-runtime": {"browser", "container"},
    "node-package": {"browser", "container"},
    "production-container": {"container"},
    "standalone-tools": {"native", "tools"},
    "deployment-config": {"deployment"},
    "independent-solana-programs": {"programs"},
}


def workflow_sources() -> dict[str, str]:
    return {
        path.name: path.read_text(encoding="utf-8")
        for path in sorted(WORKFLOW_DIR.glob("*.yml"))
    }


def script_sources() -> dict[str, str]:
    return {
        path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8")
        for path in sorted((ROOT / "scripts").rglob("*"))
        if path.is_file() and path.suffix in {".js", ".mjs"}
    }


def static_import_specifiers(source: str) -> list[str]:
    """Return ESM imports evaluated while a module is being loaded."""
    specifiers = []
    for match in re.finditer(
        r"^[ \t]*import[ \t]+(?P<body>.*?);",
        source,
        re.MULTILINE | re.DOTALL,
    ):
        body = match.group("body")
        imported = re.search(
            r"\bfrom\s+(['\"])(?P<specifier>[^'\"]+)\1",
            body,
            re.DOTALL,
        )
        if imported is None:
            imported = re.match(
                r"\s*(['\"])(?P<specifier>[^'\"]+)\1",
                body,
                re.DOTALL,
            )
        if imported is not None:
            specifiers.append(imported.group("specifier"))
    return specifiers


NODE_LEGACY_BUILTINS = {
    "assert",
    "buffer",
    "child_process",
    "crypto",
    "events",
    "fs",
    "http",
    "https",
    "net",
    "os",
    "path",
    "stream",
    "tls",
    "url",
    "util",
}


def policy_node_dependency_failures(
    policy_job: str,
    sources: dict[str, str],
) -> list[str]:
    """Keep always-run Node self-tests usable before package installation."""
    entrypoints = re.findall(
        r"^\s*node(?:\s+--test)?\s+"
        r"(scripts/[A-Za-z0-9_./-]+\.(?:mjs|js))\b",
        policy_job,
        re.MULTILINE,
    )
    failures = []
    visited: set[str] = set()
    pending = list(entrypoints)
    while pending:
        path = pending.pop()
        if path in visited:
            continue
        visited.add(path)
        source = sources.get(path)
        if source is None:
            failures.append(
                f"ci.yml: policy Node self-test source {path!r} is missing"
            )
            continue
        for specifier in static_import_specifiers(source):
            if (specifier.startswith("node:")
                    or specifier in NODE_LEGACY_BUILTINS):
                continue
            if specifier.startswith("."):
                resolved = posixpath.normpath(
                    posixpath.join(posixpath.dirname(path), specifier)
                )
                if resolved in sources:
                    pending.append(resolved)
                continue
            failures.append(
                "ci.yml: always-run policy Node self-test dependency graph "
                f"imports external package {specifier!r} from {path}; "
                "load it only in the executable path or install locked "
                "dependencies before running policy self-tests"
            )
    return failures


def job_block(source: str, job: str) -> str:
    match = re.search(
        rf"^  {re.escape(job)}:\s*$\n"
        r"(?P<body>(?:^ {4,}.*$\n|^\s*$\n)*)",
        source,
        re.MULTILINE,
    )
    return match.group("body") if match else ""


def exact_aggregate_env_failures(
    aggregate: str,
    policy_lanes: set[str],
) -> list[str]:
    """Require each aggregate input to come from its matching job/output."""
    expected = {
        "CI_RESULT_CLASSIFY": "${{ needs.classify.result }}",
    }
    for lane in policy_lanes:
        suffix = lane.upper()
        expected[f"CI_RESULT_{suffix}"] = (
            f"${{{{ needs.{lane}.result }}}}"
        )
        expected[f"CI_SELECTED_{suffix}"] = (
            f"${{{{ needs.classify.outputs.{lane} }}}}"
        )

    assignments: dict[str, list[str]] = {}
    for match in re.finditer(
        r"^\s+(?P<name>CI_(?:RESULT|SELECTED)_[A-Z0-9_]+):"
        r"\s*(?P<value>.*?)\s*$",
        aggregate,
        re.MULTILINE,
    ):
        assignments.setdefault(match.group("name"), []).append(
            match.group("value")
        )

    failures = []
    for name, value in sorted(expected.items()):
        actual = assignments.get(name, [])
        if actual != [value]:
            failures.append(
                f"ci.yml: ci-required must map {name} exactly to {value!r} "
                f"(got {actual!r})"
            )
    for name in sorted(set(assignments) - set(expected)):
        failures.append(
            f"ci.yml: ci-required has unexpected aggregate input {name}"
        )
    return failures


def exact_shipping_lane_failures(policy: dict) -> list[str]:
    """Require an explicit, complete lane contract for shipping surfaces."""
    shipping = {
        category.get("name"): category
        for category in policy.get("categories", [])
        if category.get("scope") == "shipping"
    }
    failures = []
    for name, expected in EXPECTED_SHIPPING_LANES.items():
        category = shipping.get(name)
        if category is None:
            failures.append(
                f"ci-paths.json: missing shipping category {name!r}"
            )
            continue
        actual = set(category.get("lanes", []))
        if actual != expected:
            failures.append(
                f"ci-paths.json: shipping category {name!r} must map "
                f"exactly to {sorted(expected)!r} (got {sorted(actual)!r})"
            )
    for name in sorted(set(shipping) - set(EXPECTED_SHIPPING_LANES)):
        failures.append(
            f"ci-paths.json: shipping category {name!r} has no pinned "
            "expected lane set"
        )
    return failures


def action_pin_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name, source in sources.items():
        for lineno, line in enumerate(source.splitlines(), start=1):
            match = re.search(r"\buses:\s*([^@\s]+)@([^\s#]+)", line)
            if not match:
                continue
            action, ref = match.groups()
            if action.startswith("./"):
                continue
            if ref.lower() in {"main", "master", "head", "latest"}:
                failures.append(
                    f"{name}:{lineno}: action {action} uses floating "
                    f"reference @{ref}"
                )
            owner = action.split("/", maxsplit=1)[0]
            if owner != "actions" and not re.fullmatch(r"[0-9a-f]{40}", ref):
                failures.append(
                    f"{name}:{lineno}: third-party action {action} must use "
                    "an immutable 40-character commit SHA"
                )
    return failures


def contract_failures(
    sources: dict[str, str],
    policy: dict,
    scripts: dict[str, str],
) -> list[str]:
    failures: list[str] = []
    ci = sources.get("ci.yml", "")
    failures.extend(exact_shipping_lane_failures(policy))
    replay = sources.get("replay.yml", "")
    release = sources.get("release.yml", "")

    if not container_inputs.pull_request_is_unfiltered(ci):
        failures.append(
            "ci.yml: pull_request must be unfiltered; ci-paths.json owns "
            "path selection"
        )

    jobs = set(re.findall(r"^  ([A-Za-z0-9_-]+):\s*$", ci, re.MULTILINE))
    missing_jobs = sorted(REQUIRED_JOBS - jobs)
    if missing_jobs:
        failures.append(
            f"ci.yml: missing required jobs: {', '.join(missing_jobs)}"
        )

    policy_lanes = set(policy["lanes"])
    expected_lanes = REQUIRED_JOBS - {"classify", "ci-required"}
    if policy_lanes != expected_lanes:
        failures.append(
            "ci-paths.json lanes must exactly match required CI lane jobs "
            f"(policy={sorted(policy_lanes)}, jobs={sorted(expected_lanes)})"
        )

    classify = job_block(ci, "classify")
    aggregate = job_block(ci, "ci-required")
    for lane in sorted(policy_lanes):
        output = (
            f"{lane}: "
            f"${{{{ steps.paths.outputs.{lane} }}}}"
        )
        if output not in classify:
            failures.append(
                f"ci.yml: classify job does not export {lane!r}"
            )
        if lane != "policy" and f"      - {lane}\n" not in aggregate:
            failures.append(
                f"ci.yml: ci-required does not need {lane!r}"
            )
    for required in ("classify", "policy"):
        if f"      - {required}\n" not in aggregate:
            failures.append(
                f"ci.yml: ci-required does not need {required!r}"
            )
    failures.extend(
        exact_aggregate_env_failures(aggregate, policy_lanes)
    )
    if "if: always()" not in aggregate:
        failures.append("ci.yml: ci-required must run with if: always()")
    if "python3 scripts/check_ci_required.py" not in aggregate:
        failures.append(
            "ci.yml: ci-required must execute its checked aggregate"
        )
    if "github.com/rhysd/actionlint/cmd/actionlint@v1.7.12" not in ci:
        failures.append(
            "ci.yml: policy job must run pinned actionlint v1.7.12"
        )
    policy_job = job_block(ci, "policy")
    failures.extend(policy_node_dependency_failures(policy_job, scripts))
    for marker in (
        "python3 scripts/check_banned_apis.py",
        "python3 scripts/check_cargo_trust_boundaries.py",
        "python3 scripts/test_check_cargo_trust_boundaries.py",
        "python3 scripts/check_deterministic_libm.py",
        "python3 scripts/check_deployment_inputs.py",
        "python3 scripts/test_check_deployment_inputs.py",
        "python3 scripts/test_check_client_memory_budget.py",
        "python3 scripts/test_check_memzero_codegen.py",
        "node scripts/test-rati-anchor-batch.mjs",
        "node scripts/test-rati-anchor-stamp.mjs",
        "node scripts/test-relay-region-broker.mjs",
        "node --test scripts/test-relay-traffic-probe.mjs",
        "node scripts/test-ws-backpressure-soak.mjs",
        "node scripts/test-ws-latency-proxy.mjs",
    ):
        if marker not in policy_job:
            failures.append(
                f"ci.yml: policy job lacks script check {marker!r}"
            )

    native = job_block(ci, "native")
    if "make memzero-codegen" not in native:
        failures.append(
            "ci.yml: native job lacks optimized explicit-wipe codegen gate"
        )

    scheduled_sanitizers = sources.get("soak.yml", "")
    for marker, description in (
        ("make memzero-codegen test-msan",
         "scoped MemorySanitizer lifecycle gate"),
        ("make test-tsan", "bounded ThreadSanitizer gate"),
    ):
        if marker not in scheduled_sanitizers:
            failures.append(
                f"soak.yml: scheduled workflow lacks {description}"
            )

    browser = job_block(ci, "browser")
    browser_requirements = {
        "emscripten/emsdk:4.0.15": "pinned Emscripten 4.0.15",
        "-DCMAKE_BUILD_TYPE=Release": "release WASM configuration",
        "python3 scripts/check_client_memory_budget.py":
            "release WASM memory budget",
        "npm ci": "locked npm install",
        "npx --no-install playwright install --with-deps chromium":
            "locked Chromium installation",
        "npm run smoke": "Chromium smoke suite",
    }
    for marker, description in browser_requirements.items():
        if marker not in browser:
            failures.append(f"ci.yml: browser job lacks {description}")

    cross = job_block(ci, "cross_platform")
    for marker in (
        "ubuntu-24.04",
        "macos-15",
        "windows-2025-vs2026",
        "macOS ARM Metal",
        "--target signal signal_server",
    ):
        if marker not in cross:
            failures.append(
                f"ci.yml: cross_platform job lacks {marker!r}"
            )

    container = job_block(ci, "container")
    container_requirements = {
        "docker build --no-cache":
            "clean production-image build",
        "ls --omit=dev --all":
            "recorded production dependency tree",
        "audit --omit=dev":
            "production dependency audit",
        "test ! -e /root/.npm":
            "npm-cache absence check",
        "test ! -e node_modules/@playwright/test":
            "development-dependency absence check",
        "production-container-dependencies-${{ github.sha }}":
            "uploaded dependency and audit evidence",
        "npm run test:rtc-gateway":
            "packaged WebRTC gateway test",
    }
    for marker, description in container_requirements.items():
        if marker not in container:
            failures.append(
                f"ci.yml: container job lacks {description} "
                f"({marker!r})"
            )
    if container.count("set -o pipefail") < 2:
        failures.append(
            "ci.yml: container evidence pipelines must enable pipefail"
        )
    if "FLY_API_TOKEN" in container or "setup-flyctl" in container:
        failures.append(
            "ci.yml: required container lane must not depend on Fly secrets"
        )
    for marker in (
        "python3 scripts/check_fly_config.py",
        "python3 scripts/test_check_fly_config.py",
    ):
        if marker not in job_block(ci, "policy"):
            failures.append(
                f"ci.yml: policy job lacks local Fly contract {marker!r}"
            )

    deployment = job_block(ci, "deployment")
    deployment_requirements = {
        "node-version: '24'": "SAM-compatible Node 24 runtime",
        "cache-dependency-path: "
        "aws/lambda/signal-lobby/package-lock.json":
            "nested Lambda dependency lock",
        "python3 scripts/check_deployment_inputs.py":
            "deployment input contract",
        "python3 scripts/check_fly_config.py":
            "local Fly contract",
        "bash -n deploy/lightsail-user-data.sh":
            "Lightsail bootstrap syntax check",
        "sh -n docker/entrypoint.sh":
            "local container entrypoint syntax check",
        "npm ci --ignore-scripts --omit=dev":
            "locked Lambda production dependency install",
        "npm ls --omit=dev --all":
            "Lambda production dependency tree validation",
        "import('./aws/lambda/signal-lobby/index.mjs')":
            "Lambda dependency-resolving module smoke",
        "import('./workers/fly-proxy.js')":
            "Worker module smoke",
    }
    for marker, description in deployment_requirements.items():
        if marker not in deployment:
            failures.append(
                f"ci.yml: deployment job lacks {description} "
                f"({marker!r})"
            )

    if "set -o pipefail" not in sources.get("valgrind.yml", ""):
        failures.append(
            "valgrind.yml: truncated memcheck pipeline must enable pipefail"
        )

    programs = job_block(ci, "programs")
    for manifest in (
        "programs/burn-to-mint/native/Cargo.toml",
        "programs/burn-to-mint/onchain/Cargo.toml",
    ):
        if programs.count(manifest) < 2:
            failures.append(
                f"ci.yml: programs job must check and test {manifest}"
            )
    if programs.count("--locked") < 4:
        failures.append(
            "ci.yml: every burn-to-mint cargo check/test must be locked"
        )
    for marker in (
        "RUST_TOOLCHAIN: '1.85.0'",
        'rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal',
        'rustup run "$RUST_TOOLCHAIN" cargo',
    ):
        if marker not in programs:
            failures.append(
                f"ci.yml: programs job lacks pinned toolchain marker {marker!r}"
            )

    if "workflow_call:" not in replay:
        failures.append("replay.yml: must be callable from required CI")
    if "uses: ./.github/workflows/replay.yml" not in job_block(ci, "replay"):
        failures.append("ci.yml: replay lane must call replay.yml")

    release_trigger = re.search(
        r"^on:\s*$\n(?P<body>(?:^[ \t].*$\n|^\s*$\n)*)",
        release,
        re.MULTILINE,
    )
    trigger_body = release_trigger.group("body") if release_trigger else ""
    if re.search(r"^\s+release:\s*$", trigger_body, re.MULTILINE):
        failures.append(
            "release.yml: release events occur too late for pre-publication"
        )
    if "types: [published]" in release:
        failures.append(
            "release.yml: published-event artifact builds are forbidden"
        )
    for marker in (
        "push:",
        "tags:",
        "stage-draft-release:",
        "RELEASE_REF: ${{ inputs.release_ref || github.ref_name }}",
        '-e GIT_HASH="$RELEASE_REF"',
        "--draft",
        "--verify-tag",
        "--json isDraft",
        "Release $RELEASE_TAG is already published",
        "python3 scripts/check_client_memory_budget.py",
    ):
        if marker not in release:
            failures.append(
                f"release.yml: missing pre-publication marker {marker!r}"
            )
    stage = job_block(release, "stage-draft-release")
    for build_job in ("build-native", "build-server", "build-web"):
        if build_job not in stage:
            failures.append(
                "release.yml: draft staging must depend on "
                f"{build_job}"
            )

    deploy = sources.get("deploy-fly.yml", "")
    if "python3 scripts/check_client_memory_budget.py" not in deploy:
        failures.append(
            "deploy-fly.yml: browser build lacks release WASM memory budget"
        )

    expected_browser = classify_ci_paths.classify_paths(
        [
            "web/play.html",
            "package.json",
            "package-lock.json",
            "playwright.config.ts",
        ],
        policy,
    )
    if expected_browser.unknown or "browser" not in expected_browser.lanes:
        failures.append(
            "ci-paths.json: web/package/Playwright inputs must select browser"
        )

    expected_vendor = classify_ci_paths.classify_paths(
        ["vendor/tweetnacl/tweetnacl.c"], policy
    )
    if not {"native", "fuzz"}.issubset(expected_vendor.lanes):
        failures.append(
            "ci-paths.json: vendored crypto must select native and fuzz"
        )

    for path in (
        "aws/lambda/signal-lobby/package-lock.json",
        "deploy/lightsail-user-data.sh",
        "docker/entrypoint.sh",
        "fly.toml",
        "workers/fly-proxy.js",
        "wrangler.toml",
    ):
        classified = classify_ci_paths.classify_paths([path], policy)
        if classified.unknown or "deployment" not in classified.lanes:
            failures.append(
                f"ci-paths.json: {path} must select deployment"
            )
        if "container" in classified.lanes:
            failures.append(
                f"ci-paths.json: {path} must not claim production-container "
                "coverage"
            )

    failures.extend(action_pin_failures(sources))
    for name, source in sources.items():
        if re.search(r"\bnpx\s+(?!.*--no-install)", source):
            failures.append(
                f"{name}: npx commands must use --no-install"
            )
        if "emscripten/emsdk:latest" in source:
            failures.append(f"{name}: Emscripten image must be version-pinned")
    return failures


def main() -> int:
    try:
        policy = classify_ci_paths.load_policy()
        failures = contract_failures(
            workflow_sources(), policy, script_sources()
        )
    except (OSError, ValueError) as exc:
        print(f"workflow contract check could not run: {exc}", file=sys.stderr)
        return 2
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("CI workflow contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
