#!/usr/bin/env python3
"""Compare signal_replay output across two independently built binaries.

Either input may be a native executable or an Emscripten-generated .js CLI.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from collections.abc import Mapping
from pathlib import Path

from ai_episode_outcomes import validate_outcome_output
from ai_eval_corpus import (
    evaluation_world,
    validate_evaluation_output,
    validate_permutation_pair,
)
from check_replay_repeatability import (
    ROOT,
    scenarios_for_name,
    validate_active_worker_output,
)


def runner_for(binary: Path) -> list[str]:
    if binary.suffix == ".js":
        return ["node", str(binary)]
    return [str(binary)]


def run_once(
    binary: Path,
    args: tuple[str, ...],
    out: Path,
    *,
    env: Mapping[str, str] | None = None,
) -> bool:
    cmd = [*runner_for(binary), *args, "--out", str(out)]
    result = subprocess.run(
        cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    if result.returncode != 0 or not out.exists():
        print(
            f"signal_replay command failed: {' '.join(cmd)}",
            file=sys.stderr,
        )
        if result.returncode == 0 and not out.exists():
            print(f"signal_replay did not create output file: {out}", file=sys.stderr)
        if result.stdout:
            print(result.stdout, file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        return False
    return True


def first_diff(left: Path, right: Path) -> str:
    left_lines = left.read_text(errors="replace").splitlines()
    right_lines = right.read_text(errors="replace").splitlines()
    max_lines = max(len(left_lines), len(right_lines))
    for i in range(max_lines):
        left_line = left_lines[i] if i < len(left_lines) else "<missing>"
        right_line = right_lines[i] if i < len(right_lines) else "<missing>"
        if left_line == right_line:
            continue
        return (
            f"  first differing row: {i + 1}\n"
            f"  left:  {left_line[:500]}\n"
            f"  right: {right_line[:500]}"
        )
    return "  outputs differ but no differing text row was found"


def main() -> int:
    scenario_set_name = "fast"
    args = sys.argv[1:]
    if "--scenario-set" in args:
        idx = args.index("--scenario-set")
        if idx + 1 >= len(args):
            print("--scenario-set requires a value", file=sys.stderr)
            return 2
        scenario_set_name = args[idx + 1]
        del args[idx:idx + 2]

    if len(args) != 2:
        print(
            "usage: check_replay_cross_build.py LEFT_SIGNAL_REPLAY RIGHT_SIGNAL_REPLAY "
            "[--scenario-set fast|long|all|ai-eval-fast|ai-eval-long|"
            "ai-outcomes-fast]",
            file=sys.stderr,
        )
        return 2

    scenarios = scenarios_for_name(scenario_set_name)
    if scenarios is None:
        print(f"unknown scenario set: {scenario_set_name}", file=sys.stderr)
        return 2

    left_binary = Path(args[0])
    right_binary = Path(args[1])
    if not left_binary.exists():
        print(f"left signal_replay binary not found: {left_binary}", file=sys.stderr)
        return 1
    if not right_binary.exists():
        print(f"right signal_replay binary not found: {right_binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="signal-replay-cross-build-") as tmp:
        tmpdir = Path(tmp)
        evaluation_outputs: dict[str, Path] = {}
        for i, scenario_args in enumerate(scenarios):
            left = tmpdir / f"scenario-{i}-left.jsonl"
            right = tmpdir / f"scenario-{i}-right.jsonl"
            if not run_once(left_binary, scenario_args, left):
                print(f"signal_replay scenario {i} failed on left binary", file=sys.stderr)
                print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                print(f"  left binary: {left_binary}", file=sys.stderr)
                return 1
            if not run_once(right_binary, scenario_args, right):
                print(f"signal_replay scenario {i} failed on right binary", file=sys.stderr)
                print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                print(f"  right binary: {right_binary}", file=sys.stderr)
                return 1
            left_bytes = left.read_bytes()
            right_bytes = right.read_bytes()
            if left_bytes != right_bytes:
                print(
                    f"signal_replay scenario {i} differed across builds",
                    file=sys.stderr,
                )
                print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                print(f"  left binary: {left_binary}", file=sys.stderr)
                print(f"  right binary: {right_binary}", file=sys.stderr)
                print(f"  left output: {left}", file=sys.stderr)
                print(f"  right output: {right}", file=sys.stderr)
                print(first_diff(left, right), file=sys.stderr)
                return 1
            if not left_bytes:
                print(f"signal_replay scenario {i} produced no output", file=sys.stderr)
                return 1
            outcome_failure = validate_outcome_output(left)
            if outcome_failure is not None:
                print(
                    f"signal_replay scenario {i} failed outcome facts: "
                    f"{outcome_failure}",
                    file=sys.stderr,
                )
                return 1
            evaluation_failure = validate_evaluation_output(left, scenario_args)
            if evaluation_failure is not None:
                print(
                    f"signal_replay scenario {i} failed evaluation coverage: "
                    f"{evaluation_failure}",
                    file=sys.stderr,
                )
                print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                print(f"  output: {left}", file=sys.stderr)
                return 1
            world = evaluation_world(scenario_args)
            if world != "none":
                evaluation_outputs[world] = left
            if "--active-workers" in scenario_args:
                failure = validate_active_worker_output(
                    left,
                    require_worker_tow="worker-tow-hnn" in scenario_args,
                    require_worker_repair="worker-repair-hnn" in scenario_args,
                    require_worker_delivery_proof=(
                        "worker-delivery-proof-hnn" in scenario_args
                    ),
                )
                if failure is not None:
                    print(f"signal_replay scenario {i} failed AI coverage: {failure}", file=sys.stderr)
                    print(f"  scenario set: {scenario_set_name}", file=sys.stderr)
                    print(f"  args: {' '.join(scenario_args)}", file=sys.stderr)
                    print(f"  left output: {left}", file=sys.stderr)
                    return 1
        permutation_failure = validate_permutation_pair(evaluation_outputs)
        if permutation_failure is not None:
            print(
                f"signal replay station-permutation gate failed: "
                f"{permutation_failure}",
                file=sys.stderr,
            )
            return 1

    print(
        f"signal replay cross-build check passed "
        f"({len(scenarios)} {scenario_set_name} scenarios)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
