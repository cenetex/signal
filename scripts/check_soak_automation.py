#!/usr/bin/env python3
"""Keep functional soak registrations and their automation synchronized.

RUN_SOAK is intentionally a source-level tag: `--soak-only` discovers every
tag at runtime. This check makes that implicit coverage auditable by requiring
an exact inventory, a compiled source, a registry invoked by test_main, and
native/sanitized automation that exercises the generic soak target.
"""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path
from typing import Mapping, NamedTuple


ROOT = Path(__file__).resolve().parents[1]
TEST_MAIN = ROOT / "tests" / "c" / "test_main.c"
CMAKE = ROOT / "CMakeLists.txt"

# Updating a RUN_SOAK registration requires an intentional update here. The
# check still derives and reports the live source count; this inventory makes
# additions, removals, moves, and renames visible in review instead of letting
# the suite size drift silently.
EXPECTED_SOAK_REGISTRATIONS = frozenset({
    "tests/c/test_bug_regression.c:"
    "register_bug_regression_batch3_tests:"
    "test_bug22_hauler_stuck_at_empty_station",
    "tests/c/test_econ_sim.c:"
    "register_econ_sim_sim_tests:"
    "test_econ_sim_npc_only_5min",
    "tests/c/test_econ_sim.c:"
    "register_econ_sim_sim_tests:"
    "test_grade_aware_sell_pays_per_unit_grade",
    "tests/c/test_econ_sim.c:"
    "register_econ_sim_sim_tests:"
    "test_e2e_kit_chain_converges",
    "tests/c/test_econ_sim.c:"
    "register_econ_sim_sim_tests:"
    "test_e2e_kit_import_contract_lifecycle",
    "tests/c/test_econ_sim.c:"
    "register_econ_sim_invariant_tests:"
    "test_econ_invariant_npc_only_conservation",
    "tests/c/test_navigation.c:"
    "register_navigation_autopilot_stress_tests:"
    "test_autopilot_completes_mining_cycle",
    "tests/c/test_navigation.c:"
    "register_navigation_autopilot_stress_tests:"
    "test_autopilot_does_not_leave_signal",
    "tests/c/test_navigation.c:"
    "register_navigation_autopilot_stress_tests:"
    "test_autopilot_multiple_players",
})

RUN_SOAK_RE = re.compile(
    r"^\s*RUN_SOAK\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;",
    re.MULTILINE,
)
REGISTRY_RE = re.compile(
    r"^void\s+(register_[A-Za-z0-9_]+_tests)\s*\(void\)\s*\{"
    r"(?P<body>.*?)^\}",
    re.MULTILINE | re.DOTALL,
)
CMAKE_TEST_SOURCE_RE = re.compile(r"tests/c/test_[A-Za-z0-9_]+\.c")
SIGNAL_TEST_TARGET_RE = re.compile(
    r"add_executable\(\s*signal_test(?P<body>.*?)^\s*\)",
    re.MULTILINE | re.DOTALL,
)


class SoakRegistration(NamedTuple):
    path: str
    registry: str
    test: str

    @property
    def inventory_key(self) -> str:
        return f"{self.path}:{self.registry}:{self.test}"


class AutomationRequirement(NamedTuple):
    path: str
    description: str
    pattern: re.Pattern[str]


AUTOMATION_REQUIREMENTS = (
    AutomationRequirement(
        "Makefile",
        "native test-soak target selects every RUN_SOAK registration",
        re.compile(
            r"^test-soak: build-test\s*\n"
            r"\t\$\(call RUN_PARALLEL_TESTS,--soak-only\)$",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "native soak aggregate labels its RUN_SOAK summary",
        re.compile(
            r"^test-soak: TEST_SUITE_LABEL=.*RUN_SOAK",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "soak count is derived from live RUN_SOAK registrations",
        re.compile(
            r"^SOAK_TEST_COUNT := \$\(shell .*RUN_SOAK",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "native soak validates its aggregate against the live tag count",
        re.compile(
            r"^test-soak: TEST_EXPECTED_COUNT="
            r"\$\(SOAK_TEST_COUNT\)$",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "sanitizer soak validates its aggregate against the live tag count",
        re.compile(
            r"^test-san-soak: TEST_EXPECTED_COUNT="
            r"\$\(SOAK_TEST_COUNT\)$",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "sharded automation rejects a missing or malformed test summary",
        re.compile(r"valid test summaries \(expected 1\)"),
    ),
    AutomationRequirement(
        "Makefile",
        "sharded automation rejects a reduced aggregate count",
        re.compile(
            r"discovered \$\(TEST_EXPECTED_COUNT\) tagged tests "
            r"but ran \$\$total_run"
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "sanitizer soak flags select every RUN_SOAK registration",
        re.compile(
            r"^SAN_SOAK_TEST_FLAGS \?=.*--soak-only",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        "Makefile",
        "test-san-soak uses the sharded sanitizer runner",
        re.compile(
            r"^test-san-soak: build-san\s*\n"
            r"\t\$\(call RUN_PARALLEL_TESTS,"
            r"\$\(SAN_SOAK_TEST_FLAGS\)\)$",
            re.MULTILINE,
        ),
    ),
    AutomationRequirement(
        ".github/workflows/ci.yml",
        "pull-request CI has a dedicated soak job",
        re.compile(r"^  soak:\s*$", re.MULTILINE),
    ),
    AutomationRequirement(
        ".github/workflows/ci.yml",
        "pull-request CI runs test-soak with retained failure logs",
        re.compile(
            r"make test-soak "
            r"TEST_FAILURE_LOG_DIR=test-results/soak"
        ),
    ),
    AutomationRequirement(
        ".github/workflows/ci.yml",
        "pull-request CI executes the soak freshness guard",
        re.compile(r"\bmake [^\n]*\bsoak-automation\b"),
    ),
    AutomationRequirement(
        ".github/workflows/ci.yml",
        "pull-request CI uploads soak failure artifacts",
        re.compile(r"path:\s*test-results/soak/"),
    ),
    AutomationRequirement(
        ".github/workflows/release.yml",
        "release verification runs test-soak on the release revision",
        re.compile(r"^\s+make test-soak\s*$", re.MULTILINE),
    ),
    AutomationRequirement(
        ".github/workflows/deploy-fly.yml",
        "deploy verification runs test-soak before deployment",
        re.compile(r"^\s+make test-soak\s*$", re.MULTILINE),
    ),
    AutomationRequirement(
        ".github/workflows/soak.yml",
        "sanitizer soak workflow has a scheduled cadence",
        re.compile(r"^\s+schedule:\s*$", re.MULTILINE),
    ),
    AutomationRequirement(
        ".github/workflows/soak.yml",
        "scheduled workflow runs test-san-soak with failure logs",
        re.compile(
            r"make test-san-soak "
            r"TEST_FAILURE_LOG_DIR=test-results/soak-san"
        ),
    ),
    AutomationRequirement(
        ".github/workflows/soak.yml",
        "scheduled sanitizer soak uploads failure artifacts",
        re.compile(r"path:\s*test-results/soak-san/"),
    ),
)

AUTOMATION_PATHS = sorted({
    requirement.path for requirement in AUTOMATION_REQUIREMENTS
})


def _display_path(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def discover_soak_registrations(
    test_sources: Mapping[Path, str],
) -> tuple[list[SoakRegistration], list[str]]:
    """Return every RUN_SOAK and structural errors around its registry."""
    registrations = []
    failures = []

    for path, source in sorted(
        test_sources.items(), key=lambda item: item[0].as_posix()
    ):
        display_path = _display_path(path)
        registries = [
            (match.start("body"), match.end("body"), match.group(1))
            for match in REGISTRY_RE.finditer(source)
        ]
        for match in RUN_SOAK_RE.finditer(source):
            owners = [
                registry
                for start, end, registry in registries
                if start <= match.start() < end
            ]
            test = match.group(1)
            if len(owners) != 1:
                lineno = source.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{display_path}:{lineno}: RUN_SOAK({test}) is not "
                    "inside exactly one register_*_tests function"
                )
                continue
            registrations.append(
                SoakRegistration(display_path, owners[0], test)
            )

    return registrations, failures


def inventory_failures(
    test_sources: Mapping[Path, str],
    cmake_source: str,
    test_main_source: str,
    expected: frozenset[str] = EXPECTED_SOAK_REGISTRATIONS,
) -> tuple[list[SoakRegistration], list[str]]:
    """Validate exact inventory, build inclusion, and runtime reachability."""
    registrations, failures = discover_soak_registrations(test_sources)
    actual = {registration.inventory_key for registration in registrations}

    for key in sorted(expected - actual):
        failures.append(f"missing expected RUN_SOAK registration: {key}")
    for key in sorted(actual - expected):
        failures.append(
            "unreviewed RUN_SOAK registration (update the expected "
            f"inventory): {key}"
        )

    duplicate_tests = [
        test
        for test, count in Counter(
            registration.test for registration in registrations
        ).items()
        if count > 1
    ]
    for test in sorted(duplicate_tests):
        failures.append(f"RUN_SOAK test name is registered more than once: {test}")

    signal_test_target = SIGNAL_TEST_TARGET_RE.search(cmake_source)
    if signal_test_target is None:
        failures.append(
            "CMakeLists.txt: could not find the signal_test source list"
        )
        cmake_test_sources = set()
    else:
        cmake_test_sources = set(
            CMAKE_TEST_SOURCE_RE.findall(signal_test_target.group("body"))
        )
    for path in sorted({registration.path for registration in registrations}):
        if path not in cmake_test_sources:
            failures.append(
                f"{path}: contains RUN_SOAK but is not a signal_test "
                "source in CMakeLists.txt"
            )

    for registry in sorted({
        registration.registry for registration in registrations
    }):
        invocation = re.compile(
            rf"^\s*{re.escape(registry)}\(\);\s*$",
            re.MULTILINE,
        )
        if not invocation.search(test_main_source):
            failures.append(
                f"{registry} contains RUN_SOAK but is not invoked by "
                "tests/c/test_main.c"
            )

    return registrations, failures


def automation_contract_failures(
    automation_sources: Mapping[str, str],
) -> list[str]:
    """Return missing Make/workflow contracts required by soak automation."""
    failures = []
    for requirement in AUTOMATION_REQUIREMENTS:
        source = automation_sources.get(requirement.path)
        if source is None:
            failures.append(
                f"{requirement.path}: missing automation file "
                f"({requirement.description})"
            )
        elif not requirement.pattern.search(source):
            failures.append(
                f"{requirement.path}: missing contract: "
                f"{requirement.description}"
            )
    return failures


def main() -> int:
    test_sources = {
        path: path.read_text(encoding="utf-8")
        for path in sorted((ROOT / "tests" / "c").glob("test_*.c"))
    }
    registrations, failures = inventory_failures(
        test_sources,
        CMAKE.read_text(encoding="utf-8"),
        TEST_MAIN.read_text(encoding="utf-8"),
    )

    automation_sources = {}
    for relative_path in AUTOMATION_PATHS:
        path = ROOT / relative_path
        if path.is_file():
            automation_sources[relative_path] = path.read_text(
                encoding="utf-8"
            )
    failures.extend(automation_contract_failures(automation_sources))

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        print(
            "soak automation check FAILED "
            f"({len(registrations)} RUN_SOAK tests discovered)",
            file=sys.stderr,
        )
        return 1

    print(
        f"soak automation check passed "
        f"({len(registrations)} RUN_SOAK tests)"
    )
    for registration in registrations:
        print(
            f"  {registration.test} "
            f"[{registration.path} -> {registration.registry}]"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
