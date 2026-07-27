#!/usr/bin/env python3
"""Classify PR paths from the reviewed CI path inventory.

The policy intentionally supports only exact paths and ``directory/**``
prefixes. That small grammar keeps coverage reviewable and prevents a broad
wildcard from silently swallowing a new top-level release input.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_POLICY = ROOT / ".github" / "ci-paths.json"
ALLOWED_SCOPES = {
    "shipping",
    "test",
    "gate",
    "separate-release",
    "non-shipping",
}


@dataclass(frozen=True)
class Classification:
    paths: tuple[str, ...]
    categories: tuple[str, ...]
    lanes: tuple[str, ...]
    unknown: tuple[str, ...]


def load_policy(path: Path = DEFAULT_POLICY) -> dict:
    policy = json.loads(path.read_text(encoding="utf-8"))
    failures = policy_failures(policy)
    if failures:
        raise ValueError("\n".join(failures))
    return policy


def path_matches(path: str, pattern: str) -> bool:
    if pattern.endswith("/**"):
        prefix = pattern[:-3]
        return path == prefix or path.startswith(f"{prefix}/")
    return path == pattern


def policy_failures(policy: dict) -> list[str]:
    failures: list[str] = []
    if policy.get("version") != 1:
        failures.append("policy version must be 1")

    lanes = policy.get("lanes")
    if not isinstance(lanes, dict) or not lanes:
        failures.append("lanes must be a non-empty object")
        lanes = {}
    lane_names = set(lanes)

    always = policy.get("always_lanes")
    if not isinstance(always, list):
        failures.append("always_lanes must be a list")
        always = []
    for lane in always:
        if lane not in lane_names:
            failures.append(f"unknown always lane {lane!r}")

    categories = policy.get("categories")
    if not isinstance(categories, list) or not categories:
        failures.append("categories must be a non-empty list")
        return failures

    seen_names: set[str] = set()
    seen_patterns: set[tuple[str, str]] = set()
    for index, category in enumerate(categories):
        label = f"categories[{index}]"
        if not isinstance(category, dict):
            failures.append(f"{label} must be an object")
            continue
        name = category.get("name")
        if not isinstance(name, str) or not name:
            failures.append(f"{label}.name must be a non-empty string")
            name = label
        elif name in seen_names:
            failures.append(f"duplicate category name {name!r}")
        seen_names.add(name)

        scope = category.get("scope")
        if scope not in ALLOWED_SCOPES:
            failures.append(f"{name}: invalid scope {scope!r}")
        if scope in {"non-shipping", "separate-release"}:
            if not category.get("reason"):
                failures.append(f"{name}: {scope} categories require a reason")

        patterns = category.get("paths")
        if not isinstance(patterns, list) or not patterns:
            failures.append(f"{name}: paths must be a non-empty list")
            patterns = []
        for pattern in patterns:
            if not isinstance(pattern, str) or not pattern:
                failures.append(f"{name}: every path must be a string")
                continue
            if pattern.startswith(("/", "./")) or ".." in pattern.split("/"):
                failures.append(f"{name}: unsafe path pattern {pattern!r}")
            wildcard = "*" in pattern
            if wildcard and not pattern.endswith("/**"):
                failures.append(
                    f"{name}: only directory/** wildcards are supported: "
                    f"{pattern!r}"
                )
            key = (name, pattern)
            if key in seen_patterns:
                failures.append(f"{name}: duplicate path {pattern!r}")
            seen_patterns.add(key)

        category_lanes = category.get("lanes")
        if not isinstance(category_lanes, list):
            failures.append(f"{name}: lanes must be a list")
            category_lanes = []
        for lane in category_lanes:
            if lane not in lane_names:
                failures.append(f"{name}: unknown lane {lane!r}")

    return failures


def classify_paths(paths: Iterable[str], policy: dict) -> Classification:
    normalized = tuple(sorted({
        path.strip().replace("\\", "/").removeprefix("./")
        for path in paths
        if path.strip()
    }))
    categories: set[str] = set()
    lanes = set(policy["always_lanes"])
    unknown: list[str] = []

    for path in normalized:
        matched = False
        for category in policy["categories"]:
            if any(path_matches(path, pattern) for pattern in category["paths"]):
                matched = True
                categories.add(category["name"])
                lanes.update(category["lanes"])
        if not matched:
            unknown.append(path)

    return Classification(
        normalized,
        tuple(sorted(categories)),
        tuple(sorted(lanes)),
        tuple(unknown),
    )


def tracked_paths() -> list[str]:
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.splitlines()


def changed_paths(
    base: str,
    head: str,
    *,
    root: Path = ROOT,
) -> list[str]:
    result = subprocess.run(
        [
            "git",
            "diff",
            "--name-only",
            "--no-renames",
            f"{base}...{head}",
        ],
        cwd=root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.splitlines()


def write_github_output(
    output_path: Path,
    policy: dict,
    classification: Classification,
    *,
    select_all: bool,
) -> None:
    selected = set(policy["lanes"]) if select_all else set(classification.lanes)
    with output_path.open("a", encoding="utf-8") as output:
        for lane in sorted(policy["lanes"]):
            output.write(f"{lane}={'true' if lane in selected else 'false'}\n")
        output.write(f"changed_count={len(classification.paths)}\n")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--base")
    parser.add_argument("--head")
    parser.add_argument("--path", action="append", default=[])
    parser.add_argument(
        "--all",
        action="store_true",
        help="select every lane (for workflow_dispatch)",
    )
    parser.add_argument(
        "--check-tracked",
        action="store_true",
        help="also reject any unclassified tracked or untracked file",
    )
    parser.add_argument("--github-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        policy = load_policy(args.policy)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"CI path policy is invalid: {exc}", file=sys.stderr)
        return 2

    if bool(args.base) != bool(args.head):
        print("--base and --head must be provided together", file=sys.stderr)
        return 2
    if args.base:
        try:
            paths = changed_paths(args.base, args.head)
        except subprocess.CalledProcessError as exc:
            print(exc.stderr, file=sys.stderr)
            return 2
    else:
        paths = args.path

    classification = classify_paths(paths, policy)
    unknown = list(classification.unknown)
    if args.check_tracked:
        try:
            inventory = classify_paths(tracked_paths(), policy)
        except subprocess.CalledProcessError as exc:
            print(exc.stderr, file=sys.stderr)
            return 2
        unknown.extend(inventory.unknown)

    unknown = sorted(set(unknown))
    if unknown:
        print("Unclassified repository paths:", file=sys.stderr)
        for path in unknown:
            print(f"  {path}", file=sys.stderr)
        print(
            "Update .github/ci-paths.json with an intentional category and "
            "lane selection.",
            file=sys.stderr,
        )
        return 1

    if args.github_output:
        write_github_output(
            args.github_output,
            policy,
            classification,
            select_all=args.all,
        )

    selected = set(policy["lanes"]) if args.all else set(classification.lanes)
    print(
        "CI path classification passed: "
        f"{len(classification.paths)} changed path(s), "
        f"lanes={','.join(sorted(selected)) or '(none)'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
