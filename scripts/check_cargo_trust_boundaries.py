#!/usr/bin/env python3
"""Audit provenance-sensitive server callers for trust/append bypasses.

This is intentionally a narrow source-policy check rather than a C parser. It
guards the stable architectural boundary established by issue #641:

* gameplay code may not substitute raw receipt/signature verification for the
  composed station trust evaluator;
* every currently audited mutation module must retain its evaluator calls; and
* required batch/transfer appends may not be invoked as ignored statements.
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections.abc import Mapping
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

LOW_LEVEL_VERIFY_APIS = (
    "cargo_receipt_chain_verify",
    "cargo_receipt_verify_signature",
)
LOW_LEVEL_VERIFY_ALLOWLIST = {
    "server/cargo_receipt_issue.c",
    "server/cargo_receipt_trust.c",
}

# These minima make removal or accidental replacement of an audited mutation
# boundary an explicit policy change. They correspond to station production,
# construction/module delivery, NPC hauling/acceptance, player trade/delivery,
# and receipt presentation/transfer preparation.
COMPOSED_EVALUATOR_MINIMUMS = {
    "server/game_sim.c": 2,
    "server/sim_production.c": 3,
    "server/sim_construction.c": 2,
    "server/sim_ai.c": 5,
    "server/cargo_receipt_issue.c": 2,
}
COMPOSED_EVALUATORS = (
    "cargo_receipt_evaluate_at_station",
    "cargo_receipt_evaluate_physical_origin_at_station",
)

REQUIRED_APPEND_APIS = (
    "chain_log_emit_batch",
    "cargo_receipt_commit_prepared_transfer",
)


def tracked_server_sources() -> dict[str, str]:
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            "server/*.c",
            "server/*.h",
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    sources: dict[str, str] = {}
    for relative in result.stdout.splitlines():
        path = ROOT / relative
        sources[relative] = path.read_text(encoding="utf-8", errors="replace")
    return sources


def strip_c_comments_and_literals(source: str) -> str:
    """Replace comments/string contents with spaces while preserving lines."""
    out: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                out.extend((" ", " "))
                index += 2
                state = "line_comment"
                continue
            if char == "/" and next_char == "*":
                out.extend((" ", " "))
                index += 2
                state = "block_comment"
                continue
            if char in ('"', "'"):
                quote = char
                out.append(" ")
                index += 1
                state = "literal"
                continue
            out.append(char)
            index += 1
            continue
        if state == "line_comment":
            if char == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
            index += 1
            continue
        if state == "block_comment":
            if char == "*" and next_char == "/":
                out.extend((" ", " "))
                index += 2
                state = "code"
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
            continue
        if state == "literal":
            if char == "\\" and next_char:
                out.append(" ")
                out.append("\n" if next_char == "\n" else " ")
                index += 2
                continue
            if char == quote:
                out.append(" ")
                index += 1
                state = "code"
                continue
            out.append("\n" if char == "\n" else " ")
            index += 1
    return "".join(out)


def line_number(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def call_offsets(source: str, api: str) -> list[int]:
    pattern = re.compile(rf"\b{re.escape(api)}\s*\(")
    return [match.start() for match in pattern.finditer(source)]


def is_function_definition(source: str, offset: int, api: str) -> bool:
    opening = source.find("(", offset + len(api))
    if opening < 0:
        return False
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "(":
            depth += 1
        elif source[index] == ")":
            depth -= 1
            if depth == 0:
                suffix = source[index + 1 :].lstrip()
                return suffix.startswith("{")
    return False


def statement_prefix(source: str, offset: int) -> str:
    boundary = max(
        source.rfind(";", 0, offset),
        source.rfind("{", 0, offset),
        source.rfind("}", 0, offset),
    )
    return source[boundary + 1 : offset].strip()


def append_result_is_observed(source: str, offset: int) -> bool:
    prefix = statement_prefix(source, offset)
    if not prefix:
        return False
    if "=" in prefix:
        return True
    if re.search(r"\b(return|if|while|for)\s*(?:\([^)]*)?$", prefix):
        return True
    return False


def audit_sources(sources: Mapping[str, str]) -> list[str]:
    findings: list[str] = []
    stripped = {
        path: strip_c_comments_and_literals(source)
        for path, source in sources.items()
    }

    for path, source in stripped.items():
        if not path.startswith("server/"):
            continue
        if path not in LOW_LEVEL_VERIFY_ALLOWLIST:
            for api in LOW_LEVEL_VERIFY_APIS:
                for offset in call_offsets(source, api):
                    findings.append(
                        f"{path}:{line_number(source, offset)}: "
                        f"raw {api} bypasses the composed station evaluator"
                    )
        if path.endswith(".c"):
            for api in REQUIRED_APPEND_APIS:
                for offset in call_offsets(source, api):
                    if is_function_definition(source, offset, api):
                        continue
                    if not append_result_is_observed(source, offset):
                        findings.append(
                            f"{path}:{line_number(source, offset)}: "
                            f"{api} result is ignored"
                        )

    for path, minimum in COMPOSED_EVALUATOR_MINIMUMS.items():
        source = stripped.get(path)
        if source is None:
            findings.append(f"{path}: audited mutation module is missing")
            continue
        count = sum(
            len(call_offsets(source, evaluator))
            for evaluator in COMPOSED_EVALUATORS
        )
        if count < minimum:
            findings.append(
                f"{path}: composed evaluator call count {count} is below "
                f"the audited minimum {minimum}"
            )

    return findings


def main() -> int:
    try:
        findings = audit_sources(tracked_server_sources())
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"cargo trust caller audit could not inspect sources: {exc}",
              file=sys.stderr)
        return 2
    if findings:
        print("Cargo trust caller audit failed:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("cargo trust caller audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
