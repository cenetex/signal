#!/usr/bin/env python3
"""Validate the unconditional ``ci-required`` aggregate job.

Selections and job conclusions arrive through ``CI_SELECTED_<LANE>`` and
``CI_RESULT_<LANE>``. The classifier and policy jobs are always mandatory.
"""

from __future__ import annotations

import os
import sys

import classify_ci_paths


def aggregate_failures(
    lanes: list[str],
    selected: dict[str, str],
    results: dict[str, str],
) -> list[str]:
    failures: list[str] = []
    for required in ("classify", "policy"):
        if results.get(required) != "success":
            failures.append(
                f"{required} must succeed (got {results.get(required)!r})"
            )

    for lane in lanes:
        if lane == "policy":
            continue
        selection = selected.get(lane)
        result = results.get(lane)
        if selection not in {"true", "false"}:
            failures.append(
                f"{lane} has invalid classifier output {selection!r}"
            )
        elif selection == "true" and result != "success":
            failures.append(
                f"{lane} was required but concluded {result!r}"
            )
        elif selection == "false" and result not in {"skipped", "success"}:
            failures.append(
                f"{lane} was not selected but concluded {result!r}"
            )
    return failures


def main() -> int:
    try:
        policy = classify_ci_paths.load_policy()
    except (OSError, ValueError) as exc:
        print(f"cannot load CI path policy: {exc}", file=sys.stderr)
        return 2

    lanes = sorted(policy["lanes"])
    selected = {
        lane: os.environ.get(f"CI_SELECTED_{lane.upper()}", "")
        for lane in lanes
    }
    results = {
        name: os.environ.get(f"CI_RESULT_{name.upper()}", "")
        for name in [*lanes, "classify"]
    }
    failures = aggregate_failures(lanes, selected, results)
    if failures:
        for failure in failures:
            print(f"ci-required: {failure}", file=sys.stderr)
        return 1
    print("ci-required: every selected pre-merge lane succeeded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
