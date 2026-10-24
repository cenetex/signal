#!/usr/bin/env python3
"""Regression coverage for fail-open Valgrind reuse and workflow wiring."""

import copy
from datetime import datetime, timedelta, timezone
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import valgrind_guard as guard


NOW = datetime(2026, 10, 2, 12, tzinfo=timezone.utc)
SHA = "a" * 40
FINGERPRINT = "b" * 64


def iso(age=1):
    return (NOW - timedelta(days=age)).isoformat()


def environment():
    return {
        "GITHUB_EVENT_NAME": "schedule", "VALGRIND_SCHEDULE": guard.DAILY_SCHEDULE,
        "GITHUB_RUN_ATTEMPT": "1", "GITHUB_RUN_ID": "99",
        "GITHUB_SHA": SHA, "GITHUB_WORKFLOW_SHA": SHA,
        "GITHUB_REPOSITORY": "cenetex/signal", "GITHUB_REF_NAME": "main",
        "GITHUB_REF": "refs/heads/main",
        "GITHUB_WORKFLOW_REF": "cenetex/signal/.github/workflows/valgrind.yml@refs/heads/main",
        "ImageOS": "ubuntu24", "ImageVersion": "20261001.1",
        "RUNNER_OS": "Linux", "RUNNER_ARCH": "X64",
    }


def run(run_id=50, age=1):
    return {
        "id": run_id, "run_attempt": 1, "workflow_id": 123,
        "path": guard.WORKFLOW, "head_sha": SHA, "head_branch": "main",
        "head_repository": {"full_name": "cenetex/signal"},
        "event": "schedule", "status": "completed", "conclusion": "success",
        "created_at": iso(age), "updated_at": iso(age),
    }


def jobs(run_id=50):
    return [{
        "name": f"Valgrind shard {i}/16", "run_id": run_id, "run_attempt": 1,
        "head_sha": SHA, "status": "completed", "conclusion": "success",
        "steps": [{"name": guard.PROOF_PREFIX + FINGERPRINT,
                   "status": "completed", "conclusion": "success"}],
    } for i in range(16)]


def reuse_jobs(run_id=51):
    return [{"name": "Check previous Valgrind coverage", "run_id": run_id,
             "run_attempt": 1, "head_sha": SHA, "status": "completed",
             "conclusion": "success", "steps": [
                 {"name": guard.REUSE_STEP, "status": "completed", "conclusion": "success"}]}]


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.env = environment()
        self.runs = [run()]
        self.jobs = {50: jobs()}
        self.calls = []
        self.total = None

    def api(self, path):
        self.calls.append(path)
        if path.endswith("/workflows/valgrind.yml"):
            return {"path": guard.WORKFLOW, "id": 123}
        if "/workflows/123/runs?" in path:
            self.assertIn("head_sha=" + SHA, path)
            self.assertIn("branch=main", path)
            self.assertNotIn("status=success", path)
            return {"total_count": self.total or len(self.runs), "workflow_runs": self.runs}
        run_id = int(path.split("/runs/")[1].split("/")[0])
        attempt = next(run["run_attempt"] for run in self.runs if run["id"] == run_id)
        self.assertIn(f"/attempts/{attempt}/jobs?", path)
        result = self.jobs[run_id]
        return {"total_count": len(result), "jobs": result}

    def decision(self, fingerprint=FINGERPRINT):
        return guard.decide(self.env, fingerprint, self.api, NOW)[0]

    def test_identical_success_skips(self):
        self.assertFalse(self.decision())

    def test_manual_weekly_unknown_schedule_and_reruns_force_full_coverage(self):
        for key, value in (("GITHUB_EVENT_NAME", "workflow_dispatch"),
                           ("VALGRIND_SCHEDULE", guard.WEEKLY_SCHEDULE),
                           ("VALGRIND_SCHEDULE", ""), ("GITHUB_RUN_ATTEMPT", "2")):
            with self.subTest(key=key, value=value):
                self.env = environment()
                self.env[key] = value
                self.assertTrue(self.decision())
                self.assertEqual(self.calls, [])

    def test_missing_fingerprint_forces_full_coverage(self):
        self.assertTrue(self.decision(""))
        self.assertEqual(self.calls, [])

    def test_missing_empty_or_truncated_history_runs(self):
        self.runs = []
        self.assertTrue(self.decision())
        self.runs = [run()]
        self.total = 101
        self.assertTrue(self.decision())

    def test_wrong_workflow_revision_repository_branch_or_event_runs(self):
        mutations = {"workflow_id": 456, "path": ".github/workflows/ci.yml",
                     "head_sha": "c" * 40, "head_branch": "other",
                     "head_repository": {"full_name": "someone/signal"},
                     "event": "pull_request"}
        for key, value in mutations.items():
            with self.subTest(key=key):
                self.runs = [run()]
                self.runs[0][key] = value
                self.assertTrue(self.decision())

    def test_workflow_checkout_identity_mismatch_runs(self):
        for key in ("GITHUB_WORKFLOW_SHA", "GITHUB_WORKFLOW_REF", "GITHUB_REF"):
            with self.subTest(key=key):
                self.env = environment()
                self.env[key] = "wrong"
                self.assertTrue(self.decision())

    def test_failed_cancelled_in_progress_newer_attempt_invalidates_success(self):
        for conclusion in ("failure", "cancelled", "timed_out", None):
            with self.subTest(conclusion=conclusion):
                latest = run(51, 0.5)
                latest["conclusion"] = conclusion
                # Reordered API responses cannot hide a more recent failure.
                self.runs = [run(), latest]
                self.assertTrue(self.decision())
        latest["conclusion"] = "success"
        latest["status"] = "in_progress"
        self.assertTrue(self.decision())

    def test_old_run_rerun_failure_is_not_hidden_by_creation_order(self):
        failed = run(49, 5)
        failed["updated_at"] = iso(0.1)
        failed["conclusion"] = "failure"
        self.runs = [run(), failed]
        self.assertTrue(self.decision())

    def test_seven_day_limit_and_future_timestamp_run(self):
        for age in (7, 8, -1):
            with self.subTest(age=age):
                self.runs = [run(age=age)]
                self.assertTrue(self.decision())

    def test_missing_duplicate_failed_skipped_or_different_environment_shards_run(self):
        for mode in ("missing", "duplicate", "failed", "skipped", "fingerprint", "proof", "sha", "attempt"):
            with self.subTest(mode=mode):
                shards = jobs()
                if mode == "missing":
                    shards.pop()
                elif mode == "duplicate":
                    shards[15] = copy.deepcopy(shards[0])
                elif mode in {"failed", "skipped"}:
                    shards[0]["conclusion"] = mode
                elif mode == "fingerprint":
                    shards[0]["steps"][0]["name"] = guard.PROOF_PREFIX + "c" * 64
                elif mode == "proof":
                    shards[0]["steps"][0]["conclusion"] = "skipped"
                elif mode == "sha":
                    shards[0]["head_sha"] = "c" * 40
                else:
                    shards[0]["run_attempt"] = 2
                self.jobs[50] = shards
                self.assertTrue(self.decision())

    def test_reuse_never_refreshes_coverage_age(self):
        self.runs = [run(51, 0.5), run(50, 6)]
        self.jobs[51] = reuse_jobs()
        self.assertFalse(self.decision())
        self.runs[1] = run(50, 7)
        self.assertTrue(self.decision())

    def test_skip_without_explicit_reuse_proof_is_not_success(self):
        self.runs = [run(51, 0.5), run()]
        self.jobs[51] = []
        self.assertTrue(self.decision())

    def test_current_run_is_ignored(self):
        current = run(99, 0)
        current["status"] = "in_progress"
        self.runs.insert(0, current)
        self.assertFalse(self.decision())

    def test_partial_latest_attempt_cannot_borrow_earlier_shards(self):
        self.runs[0]["run_attempt"] = 2
        self.jobs[50] = jobs()[:1]
        self.jobs[50][0]["run_attempt"] = 2
        self.assertTrue(self.decision())
        self.assertTrue(any("/attempts/2/jobs?" in path for path in self.calls))

    def test_checkout_mismatch_and_modified_inputs_disable_fingerprint(self):
        with patch.object(guard, "command", return_value="different revision"):
            with self.assertRaises(ValueError):
                guard.fingerprint(environment())
        with patch.object(guard, "command", side_effect=[SHA, " M CMakeLists.txt"]):
            with self.assertRaises(ValueError):
                guard.fingerprint(environment())

    def test_manual_success_can_supply_full_proof(self):
        self.runs[0]["event"] = "workflow_dispatch"
        self.assertFalse(self.decision())

    def test_api_failure_emits_run_true(self):
        for error in (OSError("rate limit"), TimeoutError(), KeyError("malformed")):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "output"
                with patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}, clear=True), \
                        patch("sys.argv", ["guard", "decide"]), \
                        patch.object(guard, "decide", side_effect=error), \
                        patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(guard.main(), 0)
                self.assertEqual(output.read_text(), "run=true\n")

    def test_incomplete_fingerprint_never_becomes_a_proof(self):
        for key in ("ImageOS", "ImageVersion", "GITHUB_WORKFLOW_SHA", "RUNNER_ARCH"):
            with self.subTest(key=key):
                env = environment()
                del env[key]
                with self.assertRaises(ValueError):
                    guard.fingerprint(env)

    def test_fingerprint_invalidates_environment_and_input_changes(self):
        def command(*args):
            if args == ("git", "rev-parse", "HEAD"):
                return SHA
            if args[0] == "git":
                return ""
            return json.dumps(args)
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "tool"
            executable.write_bytes(b"tool version 1")
            with patch.object(guard, "command", side_effect=command), \
                    patch.object(guard.shutil, "which", return_value=str(executable)):
                original = guard.fingerprint(environment())
                for key in ("ImageVersion", "GITHUB_WORKFLOW_SHA", "CC", "CFLAGS", "RUNNER_ARCH"):
                    env = environment()
                    env[key] = "changed"
                    self.assertNotEqual(original, guard.fingerprint(env))
                executable.write_bytes(b"tool version 2")
                self.assertNotEqual(original, guard.fingerprint(environment()))
                executable.write_bytes(b"tool version 1")
                def changed_packages(*args):
                    return "updated-libc" if args[0] == "dpkg-query" else command(*args)
                with patch.object(guard, "command", side_effect=changed_packages):
                    self.assertNotEqual(original, guard.fingerprint(environment()))


class WorkflowTests(unittest.TestCase):
    def test_workflow_preserves_all_shards_gates_and_fail_open_path(self):
        source = (guard.ROOT / guard.WORKFLOW).read_text()
        self.assertIn(f'- cron: "{guard.WEEKLY_SCHEDULE}"', source)
        self.assertIn(f'- cron: "{guard.DAILY_SCHEDULE}"', source)
        self.assertIn("workflow_dispatch:", source)
        self.assertIn("needs.plan.result != 'success' || needs.plan.outputs.run != 'false'", source)
        self.assertIn("shard: [" + ", ".join(map(str, range(16))) + "]", source)
        self.assertIn("--shard=${{ matrix.shard }}/16", source)
        self.assertIn("--quiet --no-soak", source)
        self.assertIn("scripts/run_signal_test.sh valgrind --error-exitcode=1", source)
        self.assertIn("permissions:\n  contents: read\n", source)
        self.assertNotIn("actions: write", source)
        self.assertNotIn("secrets.", source)
        self.assertIn("if: success() && steps.environment.outputs.fingerprint != ''", source)
        self.assertGreater(source.index(guard.PROOF_PREFIX), source.index('exit "$status"'))
        self.assertIn("python3 scripts/test_valgrind_guard.py", (guard.ROOT / ".github/workflows/ci.yml").read_text())


if __name__ == "__main__":
    unittest.main()
