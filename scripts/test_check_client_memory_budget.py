#!/usr/bin/env python3
"""Unit tests for the release-WebAssembly memory budget parser."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import check_client_memory_budget as budget

ROOT = Path(__file__).resolve().parents[1]


def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        out.append(byte)
        if not value:
            return bytes(out)


def wasm_with_memory(initial_pages: int, maximum_pages: int = 2048) -> bytes:
    payload = (
        uleb(1) + uleb(1) + uleb(initial_pages) + uleb(maximum_pages)
    )
    return b"\0asm\x01\0\0\0" + bytes([5]) + uleb(len(payload)) + payload


class ClientMemoryBudgetTests(unittest.TestCase):
    def test_reads_initial_pages(self) -> None:
        self.assertEqual(
            budget.wasm_initial_memory_pages(wasm_with_memory(887)),
            887,
        )

    def test_rejects_truncated_memory_section(self) -> None:
        with self.assertRaisesRegex(ValueError, "truncated"):
            budget.wasm_initial_memory_pages(wasm_with_memory(887)[:-1])

    def test_cli_rejects_over_budget_binary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wasm = Path(tmp) / "signal.wasm"
            wasm.write_bytes(wasm_with_memory(921))
            self.assertEqual(
                budget.main([str(wasm), "--max-pages", "920"]),
                1,
            )

    def test_startup_does_not_restore_full_state_zeroing_sweeps(self) -> None:
        source = (ROOT / "client" / "main.c").read_text(encoding="utf-8")
        self.assertNotIn("memset(&g, 0, sizeof(g))", source)
        self.assertEqual(source.count("world_cleanup(&g.world);"), 1)
        self.assertNotIn(
            "memset(&g.world, 0, sizeof(g.world));",
            source,
        )


if __name__ == "__main__":
    unittest.main()
