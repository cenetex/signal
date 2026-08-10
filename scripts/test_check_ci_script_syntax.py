#!/usr/bin/env python3
"""Mutation coverage for dependency-free syntax checks."""

import unittest

import check_ci_script_syntax as syntax


class ScriptSyntaxTests(unittest.TestCase):
    def test_valid_python_json_and_toml_pass(self) -> None:
        self.assertIsNone(syntax.python_failure("ok.py", "value = 1\n"))
        self.assertIsNone(syntax.json_failure("ok.json", '{"value": 1}\n'))
        self.assertIsNone(syntax.toml_failure("ok.toml", 'value = 1\n'))

    def test_python_mutation_is_rejected(self) -> None:
        failure = syntax.python_failure("broken.py", "if:\n")
        self.assertIn("Python syntax error", failure or "")

    def test_json_mutation_is_rejected(self) -> None:
        failure = syntax.json_failure("broken.json", '{"value": }\n')
        self.assertIn("JSON syntax error", failure or "")

    def test_toml_mutation_is_rejected(self) -> None:
        failure = syntax.toml_failure("broken.toml", 'value =\n')
        self.assertIn("TOML syntax error", failure or "")

    def test_shell_checker_follows_bash_and_sh_shebangs(self) -> None:
        self.assertEqual(
            syntax.shell_interpreter("#!/usr/bin/env bash\n[[ -n x ]]\n"),
            "bash",
        )
        self.assertEqual(
            syntax.shell_interpreter("#!/bin/sh\n[ -n x ]\n"),
            "sh",
        )
        self.assertIsNone(syntax.shell_interpreter("echo no-shebang\n"))


if __name__ == "__main__":
    unittest.main()
