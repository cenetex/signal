#!/usr/bin/env python3
"""Enforce the release-WebAssembly initial-memory page budget."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUDGET_HEADER = ROOT / "client" / "client_memory_budget.h"


def read_uleb(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    for _ in range(5):
        if offset >= len(data):
            raise ValueError("truncated unsigned LEB128 value")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
    raise ValueError("unsigned LEB128 value exceeds 32 bits")


def wasm_initial_memory_pages(data: bytes) -> int:
    if len(data) < 8 or data[:4] != b"\0asm" or data[4:8] != b"\x01\0\0\0":
        raise ValueError("not a WebAssembly v1 binary")

    offset = 8
    while offset < len(data):
        section_id = data[offset]
        offset += 1
        section_size, offset = read_uleb(data, offset)
        end = offset + section_size
        if end > len(data):
            raise ValueError("truncated WebAssembly section")
        if section_id != 5:
            offset = end
            continue

        count, cursor = read_uleb(data, offset)
        if count != 1:
            raise ValueError(
                f"expected one defined WebAssembly memory, found {count}"
            )
        flags, cursor = read_uleb(data, cursor)
        if flags & ~0x07:
            raise ValueError(f"unsupported WebAssembly memory flags {flags}")
        initial_pages, cursor = read_uleb(data, cursor)
        if flags & 0x01:
            _, cursor = read_uleb(data, cursor)
        if cursor != end:
            raise ValueError("unexpected trailing bytes in memory section")
        return initial_pages

    raise ValueError("WebAssembly binary has no defined memory section")


def configured_page_budget(header: Path = BUDGET_HEADER) -> int:
    source = header.read_text(encoding="utf-8")
    match = re.search(
        r"^#define\s+SIGNAL_WASM_INITIAL_PAGE_BUDGET\s+([0-9]+)u?\s*$",
        source,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(
            f"{header}: missing SIGNAL_WASM_INITIAL_PAGE_BUDGET"
        )
    return int(match.group(1))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--max-pages", type=int)
    args = parser.parse_args(argv)

    try:
        pages = wasm_initial_memory_pages(args.wasm.read_bytes())
        budget = (
            args.max_pages
            if args.max_pages is not None
            else configured_page_budget()
        )
    except (OSError, ValueError) as exc:
        print(f"client memory budget check failed: {exc}", file=sys.stderr)
        return 2

    mib = pages * 65536 / (1024 * 1024)
    print(
        f"release WASM initial memory: {pages} pages "
        f"({mib:.4f} MiB), budget {budget} pages"
    )
    if pages > budget:
        print(
            f"release WASM initial memory exceeds budget by "
            f"{pages - budget} pages",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
