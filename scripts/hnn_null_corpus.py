#!/usr/bin/env python3
"""Materialize the deterministic offline null corpus for the HNN gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CORPUS_NAME = "signal-hnn-null-replay"
CORPUS_VERSION = 1
GENERATOR_VERSION = 1


@dataclass(frozen=True)
class Scenario:
    category: str
    args: tuple[str, ...]


BASE_HISTORIES = (
    "W,WA,W,WD,A,D",
    "W,W,WA,D,S,WD",
    "A,W,WA,W,D,WD",
    "S,SA,A,W,WA,W,D,WD",
    "W,WD,WD,S,A,W,WA",
    "D,W,WD,W,A,WA,S",
    "WA,W,D,WD,W,A",
    "W,A,W,D,W,SA,SD",
)


def null_scenarios() -> tuple[Scenario, ...]:
    scenarios: list[Scenario] = []
    for index in range(16):
        qx = -7200 + index * 113
        qy = 6800 - index * 97
        scenarios.append(
            Scenario(
                "unrelated-trace",
                (
                    "--seed", str(7300 + index),
                    "--station", str(index % 3),
                    "--history", BASE_HISTORIES[index % len(BASE_HISTORIES)],
                    "--horizon-ticks", "1",
                    "--candidates", "NONE",
                    "--hnn-trace",
                    "--hnn-confidence-mode", "mixed",
                    "--hnn-query-goal", f"{qx},{qy}",
                ),
            )
        )
    for index in range(16):
        scenarios.append(
            Scenario(
                "shuffled-labels",
                (
                    "--seed", str(7400 + index),
                    "--station", str(index % 3),
                    "--history", BASE_HISTORIES[(index + 3) % len(BASE_HISTORIES)],
                    "--horizon-ticks", "1",
                    "--candidates", "NONE",
                    "--hnn-trace",
                    "--hnn-confidence-mode", "mixed",
                    "--hnn-label-shift", str(1 + index % 8),
                ),
            )
        )
    overloaded_history = ",".join(["NONE"] * 129)
    for index in range(8):
        scenarios.append(
            Scenario(
                "overloaded-memory",
                (
                    "--seed", str(7500 + index),
                    "--station", str(index % 3),
                    "--history", overloaded_history,
                    "--horizon-ticks", "1",
                    "--candidates", "NONE",
                    "--hnn-trace",
                    "--hnn-confidence-mode", "mixed",
                ),
            )
        )
    worlds = ("weak-signal", "route-disrupted", "seeded-sparse", "scarcity")
    for index in range(16):
        extra = (
            ("--hnn-query-goal", f"{-6100 + index * 71},{5700 - index * 53}")
            if index % 2 == 0
            else ("--hnn-label-shift", str(1 + index % 8))
        )
        scenarios.append(
            Scenario(
                "disrupted-world",
                (
                    "--seed", str(7600 + index),
                    "--station", str(index % 3),
                    "--history", BASE_HISTORIES[(index + 5) % len(BASE_HISTORIES)],
                    "--horizon-ticks", "1",
                    "--candidates", "NONE",
                    "--hnn-trace",
                    "--hnn-confidence-mode", "mixed",
                    "--evaluation-world", worlds[index % len(worlds)],
                    *extra,
                ),
            )
        )
    return tuple(scenarios)


def positive_scenarios() -> tuple[Scenario, ...]:
    definitions = (
        (7700, "W", 16),
        (7701, "WA", 12),
        (7702, "WD", 12),
        (7703, "W", 24),
        (7704, "WA", 16),
        (7705, "WD", 16),
    )
    return tuple(
        Scenario(
            "matched-trace",
            (
                "--seed", str(seed),
                "--station", str(index % 3),
                "--history", ",".join([action] * count),
                "--horizon-ticks", "1",
                "--candidates", action,
                "--hnn-trace",
                "--hnn-confidence-mode", "mixed",
            ),
        )
        for index, (seed, action, count) in enumerate(definitions)
    )


def canonical_scenario_digest(scenarios: tuple[Scenario, ...]) -> str:
    payload = [
        {"args": list(scenario.args), "category": scenario.category}
        for scenario in scenarios
    ]
    encoded = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode()
    return hashlib.sha256(encoded).hexdigest()


def run_scenario(binary: Path, scenario: Scenario) -> dict[str, Any]:
    completed = subprocess.run(
        [str(binary.resolve()), *scenario.args],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    rows = [json.loads(line) for line in completed.stdout.splitlines() if line]
    if len(rows) != 1 or not isinstance(rows[0].get("hnn"), dict):
        raise RuntimeError(
            f"expected one HNN replay row for {' '.join(scenario.args)}"
        )
    return rows[0]


def materialize(binary: Path, scenarios: tuple[Scenario, ...]) -> dict[str, Any]:
    if not binary.is_file():
        raise RuntimeError(f"signal_replay binary not found: {binary}")
    samples: list[dict[str, Any]] = []
    backend_name = ""
    for index, scenario in enumerate(scenarios):
        row = run_scenario(binary, scenario)
        backend = row["hnn_backend"]["active_backend"]
        if backend_name and backend != backend_name:
            raise RuntimeError("HNN backend changed while materializing corpus")
        backend_name = backend
        hnn = row["hnn"]
        contract = hnn["contract"]
        samples.append(
            {
                "allowed_margin": hnn["allowed_margin"],
                "args": list(scenario.args),
                "backend": backend,
                "capacity_load": contract["capacity_load"],
                "category": scenario.category,
                "confidence": hnn["confidence"],
                "contract": {
                    "action_vocabulary_hash": contract["action_vocabulary_hash"],
                    "dim": contract["dim"],
                    "encoder_version": contract["encoder_version"],
                    "keygen_version": contract["keygen_version"],
                    "seed": contract["seed"],
                    "trace_format_version": contract["trace_format_version"],
                },
                "held_out": index % 5 == 0,
                "index": index,
                "stored_count": contract["stored_count"],
                "top_allowed_action": hnn["top_allowed_action"],
                "top_allowed_score": hnn["top_allowed_score"],
            }
        )
    stable_measurements = [
        {
            "accepted": sample["confidence"]["accepted"],
            "category": sample["category"],
            "held_out": sample["held_out"],
            "index": sample["index"],
            "reason": sample["confidence"]["reason"],
            "stored_count": sample["stored_count"],
            "top_allowed_action": sample["top_allowed_action"],
        }
        for sample in samples
    ]
    stable_samples = json.dumps(
        stable_measurements, separators=(",", ":"), sort_keys=True
    ).encode()
    return {
        "backend": backend_name,
        "category_counts": dict(sorted(Counter(s.category for s in scenarios).items())),
        "corpus": CORPUS_NAME,
        "corpus_version": CORPUS_VERSION,
        "generator_version": GENERATOR_VERSION,
        "sample_digest": hashlib.sha256(stable_samples).hexdigest(),
        "scenario_digest": canonical_scenario_digest(scenarios),
        "samples": samples,
        "schema": "signal.hnn_null_replay.v1",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        report = materialize(args.binary.resolve(), null_scenarios())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(
            f"HNN null corpus written ({len(report['samples'])} samples): "
            f"{args.output}"
        )
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"HNN null corpus failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
