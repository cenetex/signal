#!/usr/bin/env python3
"""Run the reproducible Signal x liblecore replay and performance trial."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from compare_liblecore_replay import ComparisonError, compare_bundles


ROOT = Path(__file__).resolve().parents[1]
BACKENDS = ("builtin", "direct", "radix2")
REPORT_SCHEMA = "signal.liblecore_pilot_evidence.v1"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run fast and long replay evidence for all HNN backends."
    )
    parser.add_argument("--benchmark", required=True, type=Path)
    parser.add_argument("--builtin", required=True, type=Path)
    parser.add_argument("--direct", required=True, type=Path)
    parser.add_argument("--radix2", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--fast-runs", type=int, default=5)
    parser.add_argument("--long-runs", type=int, default=3)
    parser.add_argument("--samples", type=int, default=256)
    parser.add_argument("--tolerance", type=float, default=1.0e-5)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    for path in (args.benchmark, args.builtin, args.direct, args.radix2):
        if not path.resolve().is_file():
            raise ComparisonError(f"binary not found: {path}")
    if args.fast_runs < 2 or args.long_runs < 2:
        raise ComparisonError("fast-runs and long-runs must both be at least 2")
    if args.samples < 32:
        raise ComparisonError("samples must be at least 32")
    if not math.isfinite(args.tolerance) or args.tolerance < 0.0:
        raise ComparisonError("tolerance must be finite and non-negative")
    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise ComparisonError(f"output directory must be empty: {args.output_dir}")


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def timing_summary(values: list[float]) -> dict[str, float | int]:
    return {
        "runs": len(values),
        "median_seconds": statistics.median(values),
        "p95_seconds": percentile(values, 0.95),
        "minimum_seconds": min(values),
        "maximum_seconds": max(values),
    }


def run_benchmark(binary: Path, samples: int) -> dict[str, Any]:
    completed = subprocess.run(
        [str(binary.resolve()), "--samples", str(samples)],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return json.loads(completed.stdout)


def run_bundle(binary: Path, output: Path, scenario_set: str) -> float:
    start = time.perf_counter()
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "build_replay_bundle.py"),
            str(binary.resolve()),
            str(output),
            "--scenario-set",
            scenario_set,
        ],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return time.perf_counter() - start


def git_revision() -> str:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout.strip()


def main() -> int:
    args = parse_args()
    try:
        validate_args(args)
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        binaries = {
            "builtin": args.builtin,
            "direct": args.direct,
            "radix2": args.radix2,
        }
        benchmark = run_benchmark(args.benchmark, args.samples)
        timings: dict[str, dict[str, list[float]]] = {
            corpus: {backend: [] for backend in BACKENDS}
            for corpus in ("fast", "long")
        }
        digests: dict[str, dict[str, list[str]]] = {
            corpus: {backend: [] for backend in BACKENDS}
            for corpus in ("fast", "long")
        }
        first_roots: dict[str, dict[str, Path]] = {"fast": {}, "long": {}}
        for corpus, runs in (("fast", args.fast_runs), ("long", args.long_runs)):
            for run_index in range(runs):
                order = BACKENDS[run_index % len(BACKENDS):] + \
                        BACKENDS[:run_index % len(BACKENDS)]
                for backend in order:
                    bundle = output_dir / "bundles" / corpus / backend / f"run-{run_index:02d}"
                    elapsed = run_bundle(binaries[backend], bundle, corpus)
                    timings[corpus][backend].append(elapsed)
                    digests[corpus][backend].append(tree_digest(bundle))
                    first_roots[corpus].setdefault(backend, bundle)
        repeatability = {
            corpus: {
                backend: len(set(digests[corpus][backend])) == 1
                for backend in BACKENDS
            }
            for corpus in ("fast", "long")
        }
        fast_comparison, fast_failures = compare_bundles(
            first_roots["fast"], args.tolerance, require_hnn=True
        )
        long_comparison, long_failures = compare_bundles(
            first_roots["long"], args.tolerance, require_hnn=False
        )
        performance = {
            corpus: {
                backend: timing_summary(timings[corpus][backend])
                for backend in BACKENDS
            }
            for corpus in ("fast", "long")
        }
        for corpus in ("fast", "long"):
            baseline = performance[corpus]["builtin"]["p95_seconds"]
            assert isinstance(baseline, float)
            for backend in ("direct", "radix2"):
                candidate = performance[corpus][backend]["p95_seconds"]
                assert isinstance(candidate, float)
                performance[corpus][backend]["p95_ratio_to_builtin"] = (
                    candidate / baseline if baseline > 0.0 else None
                )
        failures = fast_failures + long_failures
        for corpus in repeatability:
            for backend, repeated in repeatability[corpus].items():
                if not repeated:
                    failures.append(f"{backend} {corpus} replay was not repeatable")
        radix_within_budget = all(
            float(performance[corpus]["radix2"]["p95_ratio_to_builtin"]) <= 1.10
            for corpus in ("fast", "long")
        )
        report = {
            "schema": REPORT_SCHEMA,
            "signal_revision": git_revision(),
            "environment": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "python": platform.python_version(),
            },
            "tolerance": args.tolerance,
            "primitive_benchmark": benchmark,
            "replay": {
                "fast": fast_comparison,
                "long": long_comparison,
                "repeatability": repeatability,
                "timings": performance,
            },
            "adoption_gates": {
                "correctness": not fast_failures and not long_failures,
                "repeatability": all(
                    repeated
                    for corpus in repeatability.values()
                    for repeated in corpus.values()
                ),
                "radix2_replay_p95_within_10_percent": radix_within_budget,
            },
            "recommendation": {
                "decision": "hold",
                "preferred_pilot_backend": "lecore-radix2",
                "reasons": [
                    "Keep the built-in backend as default while liblecore is ABI-0.",
                    "Land calibrated abstention and teacher fallback before activation.",
                    "Keep direct mode as the slow correctness oracle, not a runtime candidate.",
                ],
            },
            "failures": failures,
            "passed": not failures,
        }
        report_path = output_dir / "report.json"
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"liblecore pilot evidence written: {report_path}")
        return 0 if not failures else 1
    except (
        ComparisonError,
        OSError,
        ValueError,
        json.JSONDecodeError,
        subprocess.CalledProcessError,
    ) as exc:
        print(f"liblecore pilot failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
