#!/usr/bin/env python3
"""Run dependency-free syntax checks over owned scripts and CI data files."""

from __future__ import annotations

import ast
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def repository_paths() -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return [
        ROOT / relative
        for relative in result.stdout.splitlines()
        if (ROOT / relative).is_file()
    ]


def python_failure(path: str, source: str) -> str | None:
    try:
        ast.parse(source, filename=path)
    except SyntaxError as exc:
        return f"{path}:{exc.lineno}: Python syntax error: {exc.msg}"
    return None


def json_failure(path: str, source: str) -> str | None:
    try:
        json.loads(source)
    except json.JSONDecodeError as exc:
        return f"{path}:{exc.lineno}: JSON syntax error: {exc.msg}"
    return None


def toml_failure(path: str, source: str) -> str | None:
    try:
        tomllib.loads(source)
    except tomllib.TOMLDecodeError as exc:
        return f"{path}: TOML syntax error: {exc}"
    return None


def shell_interpreter(source: str) -> str | None:
    first_line = source.splitlines()[0] if source.splitlines() else ""
    if re.match(r"^#!(?:/usr/bin/env\s+|.*/)(?:-\S+\s+)*bash(?:\s|$)", first_line):
        return "bash"
    if re.match(r"^#!(?:/usr/bin/env\s+|.*/)(?:-\S+\s+)*sh(?:\s|$)", first_line):
        return "sh"
    return None


def command_failure(command: list[str], display: str) -> str | None:
    result = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode == 0:
        return None
    detail = (result.stderr or result.stdout).strip()
    return f"{display}: syntax check failed: {detail}"


def syntax_failures(paths: list[Path]) -> list[str]:
    failures: list[str] = []
    yaml_paths: list[str] = []

    for path in paths:
        relative = path.relative_to(ROOT).as_posix()
        suffix = path.suffix.lower()
        source = path.read_text(encoding="utf-8", errors="replace")
        failure: str | None = None
        if suffix == ".py":
            failure = python_failure(relative, source)
        elif suffix == ".json":
            failure = json_failure(relative, source)
        elif suffix == ".toml":
            failure = toml_failure(relative, source)
        elif suffix in {".yml", ".yaml"}:
            yaml_paths.append(str(path))
        elif suffix in {".js", ".mjs"}:
            failure = command_failure(
                ["node", "--check", str(path)], relative
            )
        elif suffix == ".sh" or relative.startswith("scripts/git-hooks/"):
            interpreter = shell_interpreter(source)
            if interpreter is None:
                failure = (
                    f"{relative}: shell script lacks a supported sh/bash "
                    "shebang"
                )
            else:
                failure = command_failure(
                    [interpreter, "-n", str(path)], relative
                )
        if failure:
            failures.append(failure)

    if yaml_paths:
        failure = command_failure(
            [
                "ruby",
                "-e",
                (
                    "require 'yaml'; "
                    "ARGV.each { |path| Psych.parse_file(path) }"
                ),
                *yaml_paths,
            ],
            "YAML files",
        )
        if failure:
            failures.append(failure)
    return failures


def main() -> int:
    try:
        failures = syntax_failures(repository_paths())
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"script syntax check could not run: {exc}", file=sys.stderr)
        return 2
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print(
        "script syntax check passed "
        "(Python, JSON, TOML, YAML, JS, bash, and sh)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
