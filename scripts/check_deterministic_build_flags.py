#!/usr/bin/env python3
"""Fail when production server/shared C files build without strict FP flags."""

from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILE_COMMANDS = ROOT / "compile_commands.json"
SCAN_ROOTS = ("server", "shared")
REQUIRED_FLAGS = ("-ffp-contract=off", "-fno-fast-math")


def is_production_source(path: Path) -> bool:
    try:
        rel = path.resolve().relative_to(ROOT)
    except ValueError:
        return False
    return (
        rel.suffix == ".c"
        and len(rel.parts) >= 2
        and rel.parts[0] in SCAN_ROOTS
    )


def command_flags(entry: dict[str, object]) -> list[str]:
    args = entry.get("arguments")
    if isinstance(args, list):
        return [str(arg) for arg in args]
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    return []


def main() -> int:
    compile_commands = (
        Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_COMPILE_COMMANDS
    )
    if not compile_commands.is_absolute():
        compile_commands = ROOT / compile_commands

    if not compile_commands.exists():
        print(
            f"{compile_commands} not found; run a CMake build before "
            "checking deterministic build flags",
            file=sys.stderr,
        )
        return 1

    try:
        entries = json.loads(compile_commands.read_text())
    except json.JSONDecodeError as exc:
        print(f"invalid {compile_commands}: {exc}", file=sys.stderr)
        return 1

    findings: list[tuple[str, str]] = []
    checked = 0
    for entry in entries:
        file_value = entry.get("file")
        if not isinstance(file_value, str):
            continue
        source = Path(file_value)
        if not source.is_absolute():
            directory = Path(str(entry.get("directory", ROOT)))
            source = directory / source
        if not is_production_source(source):
            continue

        checked += 1
        flags = set(command_flags(entry))
        rel = str(source.resolve().relative_to(ROOT))
        for flag in REQUIRED_FLAGS:
            if flag not in flags:
                findings.append((rel, flag))

    if checked == 0:
        print("no production server/shared compile commands found", file=sys.stderr)
        return 1

    if findings:
        print("Production C files missing deterministic FP flags:", file=sys.stderr)
        for rel, flag in findings:
            print(f"  {rel}: missing {flag}", file=sys.stderr)
        return 1

    try:
        rel_db = compile_commands.relative_to(ROOT)
    except ValueError:
        rel_db = compile_commands
    print(f"deterministic build flag check passed ({checked} compile commands in {rel_db})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
