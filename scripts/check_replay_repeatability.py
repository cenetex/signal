#!/usr/bin/env python3
"""Run signal_replay twice per scenario and require byte-identical output."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "signal_replay"

SCENARIOS = (
    (
        "--seed", "2037",
        "--history", "W,W,WA,D",
        "--horizon-ticks", "36",
        "--candidates", "NONE,W,A,D,S,WA,WD,SA,SD",
    ),
    (
        "--seed", "4242",
        "--station", "1",
        "--history", "W,WD,WD,S,A",
        "--horizon-ticks", "48",
        "--candidates", "NONE,W,WD,SD",
    ),
    (
        "--seed", "9001",
        "--spawn", "1200,-1800",
        "--velocity", "12.5,-7.25",
        "--angle", "0.75",
        "--goal", "2600,-900",
        "--history", "A,W,WA,W,D",
        "--horizon-ticks", "24",
        "--candidates", "A,D,WA,WD",
    ),
)


def run_once(binary: Path, args: tuple[str, ...], out: Path) -> None:
    subprocess.run(
        [str(binary), *args, "--out", str(out)],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def main() -> int:
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BINARY
    if not binary.exists():
        print(f"signal_replay binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="signal-replay-repeat-") as tmp:
        tmpdir = Path(tmp)
        for i, args in enumerate(SCENARIOS):
            left = tmpdir / f"scenario-{i}-a.jsonl"
            right = tmpdir / f"scenario-{i}-b.jsonl"
            run_once(binary, args, left)
            run_once(binary, args, right)
            left_bytes = left.read_bytes()
            right_bytes = right.read_bytes()
            if left_bytes != right_bytes:
                print(f"signal_replay scenario {i} was not repeatable", file=sys.stderr)
                print(f"  args: {' '.join(args)}", file=sys.stderr)
                print(f"  left: {left}", file=sys.stderr)
                print(f"  right: {right}", file=sys.stderr)
                return 1
            if not left_bytes:
                print(f"signal_replay scenario {i} produced no output", file=sys.stderr)
                return 1

    print(f"signal replay repeatability check passed ({len(SCENARIOS)} scenarios)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
