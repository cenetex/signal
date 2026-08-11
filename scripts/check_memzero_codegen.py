#!/usr/bin/env python3
"""Verify that the optimized C11 memzero fallback keeps observable stores."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


FUNCTION = "signal_memzero_explicit"
VOLATILE_ZERO_STORE = re.compile(r"\bstore\s+volatile\s+i8\s+0(?:\s|,)")
PLAIN_MEMSET_CALL = re.compile(
    r"\bcall\b[^\n]*@(?:llvm\.memset(?:\.[^(]+)?|memset)\s*\("
)


def function_body(ir: str, function: str = FUNCTION) -> str | None:
    """Return one LLVM function definition, without trusting line numbers."""
    definition = re.search(
        rf"^\s*define\b[^\n]*@{re.escape(function)}\s*\(",
        ir,
        re.MULTILINE,
    )
    if definition is None:
        return None

    opening_brace = ir.find("{", definition.end())
    if opening_brace == -1:
        return None
    closing_brace = re.search(r"^\s*}\s*(?:;.*)?$", ir[opening_brace + 1 :],
                              re.MULTILINE)
    if closing_brace is None:
        return None
    return ir[opening_brace + 1 : opening_brace + 1 + closing_brace.start()]


def codegen_failures(ir: str) -> list[str]:
    body = function_body(ir)
    if body is None:
        return [f"optimized IR does not define {FUNCTION}"]

    failures: list[str] = []
    if VOLATILE_ZERO_STORE.search(body) is None:
        failures.append(
            "explicit wipe fallback lost its volatile zero-byte store"
        )
    if PLAIN_MEMSET_CALL.search(body):
        failures.append(
            "explicit wipe fallback was lowered to an elidable memset call"
        )
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("ir", type=Path, help="optimized LLVM IR to inspect")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        ir = args.ir.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"memzero codegen check could not read {args.ir}: {exc}",
              file=sys.stderr)
        return 2

    failures = codegen_failures(ir)
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("memzero optimized-code check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
