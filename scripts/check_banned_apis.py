#!/usr/bin/env python3
"""Fail on high-risk C library calls in owned source files."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

BANNED = {
    "gets": "use fgets with an explicit buffer size",
    "strcpy": "use memcpy/snprintf with a visible capacity check",
    "strcat": "use a length-tracked append helper",
    "sprintf": "use snprintf and check the return value",
    "vsprintf": "use vsnprintf and check the return value",
    "scanf": "use bounded parsing with strtol/strtof or a project parser",
    "sscanf": "use bounded parsing with strtol/strtof or a project parser",
    "fscanf": "use fgets plus bounded parsing",
    "strncpy": "do not use truncating pseudo-safe copies",
    "strncat": "use a length-tracked append helper",
    "atoi": "use strtol with full-tail validation",
    "atol": "use strtol with full-tail validation",
    "atoll": "use strtoll with full-tail validation",
    "rand": "use shared/rng.h or an explicit deterministic generator",
    "tmpnam": "use mkstemp or a platform secure-temp helper",
    "mktemp": "use mkstemp or a platform secure-temp helper",
}

EXCLUDED_PREFIXES = (
    "vendor/",
)

EXCLUDED_FILES = {
    "client/stb_image.h",
    "client/minimp3.h",
    "client/pl_mpeg.h",
    "server/mongoose.c",
    "server/mongoose.h",
}

CALL_RE = re.compile(r"\b(" + "|".join(map(re.escape, sorted(BANNED))) + r")\s*\(")


def tracked_c_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "--", "*.c", "*.h"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    files: list[Path] = []
    for raw in result.stdout.splitlines():
        if raw.startswith(EXCLUDED_PREFIXES) or raw in EXCLUDED_FILES:
            continue
        files.append(ROOT / raw)
    return files


def strip_line_comments(line: str, in_block: bool) -> tuple[str, bool]:
    out: list[str] = []
    i = 0
    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end == -1:
                return "".join(out), True
            i = end + 2
            in_block = False
            continue
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if line.startswith("//", i):
            break
        out.append(line[i])
        i += 1
    return "".join(out), in_block


def main() -> int:
    findings: list[tuple[str, int, str, str]] = []
    for path in tracked_c_files():
        rel = path.relative_to(ROOT).as_posix()
        in_block_comment = False
        for lineno, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
            line, in_block_comment = strip_line_comments(raw, in_block_comment)
            for match in CALL_RE.finditer(line):
                api = match.group(1)
                findings.append((rel, lineno, api, BANNED[api]))

    if findings:
        print("Banned C APIs found:", file=sys.stderr)
        for rel, lineno, api, replacement in findings:
            print(
                f"  {rel}:{lineno}: {api}() is banned; {replacement}",
                file=sys.stderr,
            )
        return 1

    print("banned API check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
