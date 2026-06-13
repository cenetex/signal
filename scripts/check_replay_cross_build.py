#!/usr/bin/env python3
"""Compare signal_replay output across two independently built binaries.

Either input may be a native executable or an Emscripten-generated .js CLI.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from check_replay_repeatability import ROOT, SCENARIOS


def runner_for(binary: Path) -> list[str]:
    if binary.suffix == ".js":
        return ["node", str(binary)]
    return [str(binary)]


def run_once(binary: Path, args: tuple[str, ...], out: Path) -> bool:
    try:
        subprocess.run(
            [*runner_for(binary), *args, "--out", str(out)],
            cwd=ROOT,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        print(
            f"signal_replay command failed: {' '.join(exc.cmd)}",
            file=sys.stderr,
        )
        if exc.stdout:
            print(exc.stdout, file=sys.stderr)
        if exc.stderr:
            print(exc.stderr, file=sys.stderr)
        return False
    return True


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: check_replay_cross_build.py LEFT_SIGNAL_REPLAY RIGHT_SIGNAL_REPLAY",
            file=sys.stderr,
        )
        return 2

    left_binary = Path(sys.argv[1])
    right_binary = Path(sys.argv[2])
    if not left_binary.exists():
        print(f"left signal_replay binary not found: {left_binary}", file=sys.stderr)
        return 1
    if not right_binary.exists():
        print(f"right signal_replay binary not found: {right_binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="signal-replay-cross-build-") as tmp:
        tmpdir = Path(tmp)
        for i, args in enumerate(SCENARIOS):
            left = tmpdir / f"scenario-{i}-left.jsonl"
            right = tmpdir / f"scenario-{i}-right.jsonl"
            if not run_once(left_binary, args, left):
                print(f"signal_replay scenario {i} failed on left binary", file=sys.stderr)
                print(f"  args: {' '.join(args)}", file=sys.stderr)
                print(f"  left binary: {left_binary}", file=sys.stderr)
                return 1
            if not run_once(right_binary, args, right):
                print(f"signal_replay scenario {i} failed on right binary", file=sys.stderr)
                print(f"  args: {' '.join(args)}", file=sys.stderr)
                print(f"  right binary: {right_binary}", file=sys.stderr)
                return 1
            left_bytes = left.read_bytes()
            right_bytes = right.read_bytes()
            if left_bytes != right_bytes:
                print(
                    f"signal_replay scenario {i} differed across builds",
                    file=sys.stderr,
                )
                print(f"  args: {' '.join(args)}", file=sys.stderr)
                print(f"  left binary: {left_binary}", file=sys.stderr)
                print(f"  right binary: {right_binary}", file=sys.stderr)
                print(f"  left output: {left}", file=sys.stderr)
                print(f"  right output: {right}", file=sys.stderr)
                return 1
            if not left_bytes:
                print(f"signal_replay scenario {i} produced no output", file=sys.stderr)
                return 1

    print(f"signal replay cross-build check passed ({len(SCENARIOS)} scenarios)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
