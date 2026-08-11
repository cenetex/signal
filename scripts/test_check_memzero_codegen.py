#!/usr/bin/env python3
"""Mutation coverage for the optimized explicit-wipe codegen gate."""

import unittest

import check_memzero_codegen as codegen


GOOD_IR = """
define void @signal_memzero_explicit(ptr %ptr, i64 %len) {
entry:
  br label %loop
loop:
  store volatile i8 0, ptr %ptr, align 1
  br i1 true, label %done, label %loop
done:
  ret void
}
"""


class MemzeroCodegenTests(unittest.TestCase):
    def test_volatile_zero_store_passes(self) -> None:
        self.assertEqual(codegen.codegen_failures(GOOD_IR), [])

    def test_missing_function_is_rejected(self) -> None:
        self.assertTrue(any(
            "does not define" in failure
            for failure in codegen.codegen_failures(
                "define void @other() { ret void }\n"
            )
        ))

    def test_nonvolatile_store_is_rejected(self) -> None:
        mutated = GOOD_IR.replace("store volatile i8 0", "store i8 0")
        self.assertTrue(any(
            "volatile zero-byte store" in failure
            for failure in codegen.codegen_failures(mutated)
        ))

    def test_plain_memset_lowering_is_rejected(self) -> None:
        mutated = GOOD_IR.replace(
            "  ret void",
            "  call void @llvm.memset.p0.i64("
            "ptr %ptr, i8 0, i64 %len, i1 false)\n"
            "  ret void",
        )
        self.assertTrue(any(
            "elidable memset" in failure
            for failure in codegen.codegen_failures(mutated)
        ))


if __name__ == "__main__":
    unittest.main()
