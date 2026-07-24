#!/usr/bin/env python3
"""Compare deterministic replay bundles exported on different runners."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from check_replay_cross_build import first_diff


BUNDLE_SCHEMA = "signal.replay_bundle.v1"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare two or more exported Signal replay bundles."
    )
    parser.add_argument("bundles", nargs="+", type=Path)
    return parser.parse_args()


def load_bundle(path: Path) -> tuple[dict[str, object], dict[str, bytes]] | None:
    manifest_path = path / "manifest.json"
    if not manifest_path.is_file():
        print(f"replay bundle manifest not found: {manifest_path}", file=sys.stderr)
        return None
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"invalid replay bundle manifest {manifest_path}: {exc}", file=sys.stderr)
        return None
    if not isinstance(manifest, dict) or manifest.get("schema") != BUNDLE_SCHEMA:
        print(f"unsupported replay bundle schema in {manifest_path}", file=sys.stderr)
        return None

    scenarios = manifest.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        print(f"replay bundle has no scenarios: {manifest_path}", file=sys.stderr)
        return None

    outputs: dict[str, bytes] = {}
    for entry in scenarios:
        if not isinstance(entry, dict):
            print(f"invalid scenario entry in {manifest_path}", file=sys.stderr)
            return None
        filename = entry.get("file")
        expected_hash = entry.get("sha256")
        expected_size = entry.get("size")
        if (
            not isinstance(filename, str)
            or Path(filename).name != filename
            or not filename.endswith(".jsonl")
        ):
            print(
                f"unsafe scenario filename in {manifest_path}: {filename}",
                file=sys.stderr,
            )
            return None
        output_path = path / filename
        if not output_path.is_file():
            print(f"replay scenario output not found: {output_path}", file=sys.stderr)
            return None
        output_bytes = output_path.read_bytes()
        actual_hash = hashlib.sha256(output_bytes).hexdigest()
        if actual_hash != expected_hash or len(output_bytes) != expected_size:
            print(f"replay scenario digest mismatch: {output_path}", file=sys.stderr)
            return None
        outputs[filename] = output_bytes
    return manifest, outputs


def main() -> int:
    args = parse_args()
    if len(args.bundles) < 2:
        print("at least two replay bundles are required", file=sys.stderr)
        return 2

    loaded: list[tuple[Path, dict[str, object], dict[str, bytes]]] = []
    for bundle_path in args.bundles:
        resolved = bundle_path.resolve()
        bundle = load_bundle(resolved)
        if bundle is None:
            return 1
        loaded.append((resolved, bundle[0], bundle[1]))

    baseline_path, baseline_manifest, baseline_outputs = loaded[0]
    for candidate_path, candidate_manifest, candidate_outputs in loaded[1:]:
        if candidate_manifest != baseline_manifest:
            print("replay bundle manifests differ", file=sys.stderr)
            print(f"  baseline:  {baseline_path}", file=sys.stderr)
            print(f"  candidate: {candidate_path}", file=sys.stderr)
            return 1
        for filename, baseline_bytes in baseline_outputs.items():
            candidate_bytes = candidate_outputs.get(filename)
            if candidate_bytes == baseline_bytes:
                continue
            print("replay scenario differed across runner bundles", file=sys.stderr)
            print(f"  scenario:  {filename}", file=sys.stderr)
            print(f"  baseline:  {baseline_path}", file=sys.stderr)
            print(f"  candidate: {candidate_path}", file=sys.stderr)
            if candidate_bytes is not None:
                print(
                    first_diff(
                        baseline_path / filename,
                        candidate_path / filename,
                    ),
                    file=sys.stderr,
                )
            return 1

    scenario_count = len(baseline_outputs)
    print(
        f"signal replay bundle check passed "
        f"({len(loaded)} runners, {scenario_count} scenarios)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
