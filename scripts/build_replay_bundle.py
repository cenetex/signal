#!/usr/bin/env python3
"""Export deterministic Signal replay scenarios as a portable CI artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from check_replay_cross_build import run_once
from check_replay_repeatability import (
    scenarios_for_name,
    validate_active_worker_output,
)


BUNDLE_SCHEMA = "signal.replay_bundle.v1"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one signal_replay binary and export a stable replay bundle."
    )
    parser.add_argument("binary", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument(
        "--scenario-set",
        default="fast",
        choices=("fast", "long", "all"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    if not binary.exists():
        print(f"signal_replay binary not found: {binary}", file=sys.stderr)
        return 1

    scenarios = scenarios_for_name(args.scenario_set)
    if scenarios is None:
        print(f"unknown scenario set: {args.scenario_set}", file=sys.stderr)
        return 2

    output_dir = args.output_dir.resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        print(
            f"replay bundle output directory must be empty: {output_dir}",
            file=sys.stderr,
        )
        return 1
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_scenarios: list[dict[str, object]] = []

    for index, scenario_args in enumerate(scenarios):
        filename = f"scenario-{index:03d}.jsonl"
        output = output_dir / filename
        if not run_once(binary, scenario_args, output):
            print(
                f"signal_replay scenario {index} failed while building bundle",
                file=sys.stderr,
            )
            print(f"  scenario set: {args.scenario_set}", file=sys.stderr)
            print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
            return 1

        output_bytes = output.read_bytes()
        if not output_bytes:
            print(f"signal_replay scenario {index} produced no output", file=sys.stderr)
            return 1

        if "--active-workers" in scenario_args:
            failure = validate_active_worker_output(
                output,
                require_worker_tow="worker-tow-hnn" in scenario_args,
                require_worker_repair="worker-repair-hnn" in scenario_args,
                require_worker_delivery_proof=(
                    "worker-delivery-proof-hnn" in scenario_args
                ),
            )
            if failure is not None:
                print(
                    f"signal_replay scenario {index} failed AI coverage: {failure}",
                    file=sys.stderr,
                )
                return 1

        manifest_scenarios.append(
            {
                "args": list(scenario_args),
                "file": filename,
                "index": index,
                "sha256": hashlib.sha256(output_bytes).hexdigest(),
                "size": len(output_bytes),
            }
        )

    manifest = {
        "scenario_set": args.scenario_set,
        "scenarios": manifest_scenarios,
        "schema": BUNDLE_SCHEMA,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"signal replay bundle exported "
        f"({len(manifest_scenarios)} {args.scenario_set} scenarios): "
        f"{output_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
