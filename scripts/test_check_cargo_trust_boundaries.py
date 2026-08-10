#!/usr/bin/env python3
"""Mutation tests for the provenance-sensitive caller audit."""

from __future__ import annotations

import unittest

import check_cargo_trust_boundaries as audit


def baseline_sources() -> dict[str, str]:
    sources: dict[str, str] = {}
    for path, minimum in audit.COMPOSED_EVALUATOR_MINIMUMS.items():
        calls = "\n".join(
            "void f(void) { cargo_receipt_evaluate_at_station(0, 0, 0, 0); }"
            for _ in range(minimum)
        )
        sources[path] = calls
    sources["server/cargo_receipt_issue.c"] += """
void low_level(void) {
    result = cargo_receipt_chain_verify(0, 0, 0);
    ok = cargo_receipt_verify_signature(0);
}
"""
    sources["server/chain_log.c"] = """
result_t wrapper(void) {
    return chain_log_emit_batch(0, 0, 0, 0);
}
"""
    return sources


class CargoTrustBoundaryTests(unittest.TestCase):
    def test_audited_baseline_passes(self) -> None:
        self.assertEqual(audit.audit_sources(baseline_sources()), [])

    def test_raw_gameplay_verifier_is_rejected(self) -> None:
        sources = baseline_sources()
        sources["server/game_sim.c"] += """
void bypass(void) {
    verdict = cargo_receipt_chain_verify(chain, count, pub);
}
"""
        failures = audit.audit_sources(sources)
        self.assertTrue(any(
            "raw cargo_receipt_chain_verify" in failure
            for failure in failures
        ), failures)

    def test_comments_and_literals_do_not_create_false_calls(self) -> None:
        sources = baseline_sources()
        sources["server/game_sim.c"] += r'''
/* cargo_receipt_chain_verify(chain, count, pub); */
const char *example = "cargo_receipt_verify_signature(receipt)";
'''
        self.assertEqual(audit.audit_sources(sources), [])

    def test_removed_composed_boundary_is_rejected(self) -> None:
        sources = baseline_sources()
        sources["server/sim_construction.c"] = (
            sources["server/sim_construction.c"].replace(
                "cargo_receipt_evaluate_at_station(0, 0, 0, 0);",
                "legacy_accept();",
                1,
            )
        )
        failures = audit.audit_sources(sources)
        self.assertTrue(any(
            "server/sim_construction.c" in failure
            and "below the audited minimum" in failure
            for failure in failures
        ), failures)

    def test_physical_origin_evaluator_is_a_composed_boundary(self) -> None:
        sources = baseline_sources()
        sources["server/sim_construction.c"] = (
            sources["server/sim_construction.c"].replace(
                "cargo_receipt_evaluate_at_station(0, 0, 0, 0);",
                (
                    "cargo_receipt_evaluate_physical_origin_at_station("
                    "0, 0, 0, 0);"
                ),
                1,
            )
        )
        self.assertEqual(audit.audit_sources(sources), [])

    def test_ignored_required_append_is_rejected(self) -> None:
        sources = baseline_sources()
        sources["server/sim_production.c"] += """
void fail_open(void) {
    chain_log_emit_batch(world, station, events, count);
}
"""
        failures = audit.audit_sources(sources)
        self.assertTrue(any(
            "chain_log_emit_batch result is ignored" in failure
            for failure in failures
        ), failures)

    def test_assigned_and_conditional_append_results_are_observed(self) -> None:
        sources = baseline_sources()
        sources["server/sim_ai.c"] += """
void checked(void) {
    append_result_t result =
        cargo_receipt_commit_prepared_transfer(world, prepared);
    if (chain_log_emit_batch(world, station, events, count).ok) {
        accepted();
    }
}
"""
        self.assertEqual(audit.audit_sources(sources), [])

    def test_required_append_function_definition_is_not_a_call(self) -> None:
        sources = baseline_sources()
        sources["server/chain_log.c"] += """
append_result_t chain_log_emit_batch(
    world_t *world, station_t *station, event_t *events, size_t count) {
    return committed();
}
"""
        self.assertEqual(audit.audit_sources(sources), [])


if __name__ == "__main__":
    unittest.main()
