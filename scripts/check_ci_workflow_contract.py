#!/usr/bin/env python3
"""Enforce the pre-merge and pre-publication workflow architecture."""

from __future__ import annotations

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
    "ci-required",
}


def workflow_sources() -> dict[str, str]:
    return {
        path.name: path.read_text(encoding="utf-8")
        for path in sorted(WORKFLOW_DIR.glob("*.yml"))
    }


def job_block(source: str, job: str) -> str:
    match = re.search(
        rf"^  {re.escape(job)}:\s*$\n"
        r"(?P<body>(?:^ {4,}.*$\n|^\s*$\n)*)",
        source,
        re.MULTILINE,
    )
    return match.group("body") if match else ""


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
) -> list[str]:
    failures: list[str] = []
    ci = sources.get("ci.yml", "")
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
    for marker in (
        "python3 scripts/check_banned_apis.py",
        "python3 scripts/check_deterministic_libm.py",
        "node scripts/test-rati-anchor-batch.mjs",
        "node scripts/test-rati-anchor-stamp.mjs",
        "node scripts/test-relay-region-broker.mjs",
        "node scripts/test-ws-backpressure-soak.mjs",
        "node scripts/test-ws-latency-proxy.mjs",
    ):
        if marker not in policy_job:
            failures.append(
                f"ci.yml: policy job lacks script check {marker!r}"
            )

    browser = job_block(ci, "browser")
    browser_requirements = {
        "emscripten/emsdk:4.0.15": "pinned Emscripten 4.0.15",
        "-DCMAKE_BUILD_TYPE=Release": "release WASM configuration",
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
    for marker in ("docker build --no-cache",):
        if marker not in container:
            failures.append(
                f"ci.yml: container job lacks {marker!r}"
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
        failures = contract_failures(workflow_sources(), policy)
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
