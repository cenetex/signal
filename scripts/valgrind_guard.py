#!/usr/bin/env python3
"""Reuse only recent, complete Valgrind coverage of identical inputs.

History comes from GitHub's public, unauthenticated read API. Rate limiting,
private repositories, incomplete history, or unknown environments all run the
full suite. No cache writes, secrets, or additional token permissions are used.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ".github/workflows/valgrind.yml"
WEEKLY_SCHEDULE = "17 10 * * 0"
DAILY_SCHEDULE = "17 10 * * 1-6"
SHARDS = 16
PROOF_PREFIX = "Valgrind coverage fingerprint "
REUSE_STEP = "Reuse previous complete Valgrind run"
MAX_AGE = timedelta(days=7)


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def command(*args: str) -> str:
    return subprocess.check_output(args, cwd=ROOT, text=True, timeout=30).strip()


def fingerprint(env: dict[str, str]) -> str:
    """Include the entire revision plus the installed, post-apt environment."""
    required = ("GITHUB_SHA", "GITHUB_WORKFLOW_SHA", "GITHUB_WORKFLOW_REF",
                "ImageOS", "ImageVersion", "RUNNER_OS", "RUNNER_ARCH")
    if any(not env.get(key) for key in required):
        raise ValueError("runner image or workflow identity is unavailable")
    if command("git", "rev-parse", "HEAD") != env["GITHUB_SHA"]:
        raise ValueError("checkout does not match the workflow revision")
    if command("git", "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("tracked build inputs are modified")
    values = {key: env[key] for key in required}
    # The whole commit covers CMake, vendored dependencies, test registration,
    # launcher, guard, and all flags in the workflow, not just source paths.
    values["schema"] = "valgrind-coverage-v1"
    values["workflow"] = digest((ROOT / WORKFLOW).read_bytes())
    values["os-release"] = Path("/etc/os-release").read_text()
    values["kernel"] = command("uname", "-srmo")
    values["packages"] = command(
        "dpkg-query", "-W", "-f=${binary:Package}=${Version} ${Architecture}\\n")
    for name in ("cc", "c++", "cmake", "make", "ld", "valgrind"):
        executable = shutil.which(name)
        if not executable:
            raise ValueError(f"missing tool: {name}")
        values[name] = {
            "version": command(name, "--version"),
            "binary": digest(Path(executable).resolve().read_bytes()),
        }
    values["compiler-target"] = command("cc", "-dumpmachine")
    values["compiler-specs"] = command("cc", "-dumpspecs")
    for key in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
                "CMAKE_GENERATOR", "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH",
                "LD_LIBRARY_PATH", "LIBRARY_PATH", "CPATH"):
        values[key] = env.get(key, "")
    return digest(json.dumps(values, sort_keys=True).encode())


def api(path: str) -> dict:
    request = urllib.request.Request(
        "https://api.github.com" + path,
        headers={"Accept": "application/vnd.github+json",
                 "X-GitHub-Api-Version": "2022-11-28",
                 "User-Agent": "signal-valgrind-guard"},
    )
    with urllib.request.urlopen(request, timeout=15) as response:
        data = json.load(response)
    if not isinstance(data, dict):
        raise ValueError("unexpected GitHub response")
    return data


def timestamp(value: str) -> datetime:
    result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if result.tzinfo is None:
        raise ValueError("history timestamp has no timezone")
    return result


def complete_coverage(jobs: list[dict], expected: str, sha: str) -> bool:
    expected_names = {f"Valgrind shard {shard}/{SHARDS}" for shard in range(SHARDS)}
    shards = [job for job in jobs if job.get("name", "").startswith("Valgrind shard ")]
    if len(shards) != SHARDS or {job["name"] for job in shards} != expected_names:
        return False
    return all(
        job.get("head_sha") == sha
        and job.get("status") == "completed"
        and job.get("conclusion") == "success"
        and any(step.get("name") == PROOF_PREFIX + expected
                and step.get("status") == "completed"
                and step.get("conclusion") == "success"
                for step in job.get("steps", []))
        for job in shards
    )


def deliberate_reuse(jobs: list[dict], sha: str) -> bool:
    # A skipped workflow is not itself a fresh success. Walk past it to the
    # original full run, without extending the seven-day validity window.
    return any(job.get("name") == "Check previous Valgrind coverage"
               and job.get("head_sha") == sha
               and job.get("status") == "completed"
               and job.get("conclusion") == "success"
               and any(step.get("name") == REUSE_STEP
                       and step.get("status") == "completed"
                       and step.get("conclusion") == "success"
                       for step in job.get("steps", []))
               for job in jobs)


def decide(env: dict[str, str], expected: str, get=api,
           now: datetime | None = None) -> tuple[bool, str]:
    now = now or datetime.now(timezone.utc)
    if env.get("GITHUB_EVENT_NAME") != "schedule":
        return True, "manual/non-scheduled runs always retain full coverage"
    if env.get("VALGRIND_SCHEDULE") != DAILY_SCHEDULE:
        return True, "weekly or unknown schedule requires full coverage"
    if env.get("GITHUB_RUN_ATTEMPT") != "1":
        return True, "explicit reruns always retain full coverage"
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        return True, "environment fingerprint is unavailable"
    repo = env["GITHUB_REPOSITORY"]
    branch = env["GITHUB_REF_NAME"]
    sha = env["GITHUB_SHA"]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("invalid repository identity")
    if (env.get("GITHUB_REF") != "refs/heads/" + branch
            or env.get("GITHUB_WORKFLOW_REF") != f"{repo}/{WORKFLOW}@refs/heads/{branch}"
            or env.get("GITHUB_WORKFLOW_SHA") != sha):
        return True, "workflow and checkout identities differ"
    base = f"/repos/{repo}/actions"
    workflow = get(f"{base}/workflows/valgrind.yml")
    if workflow.get("path") != WORKFLOW or not isinstance(workflow.get("id"), int):
        return True, "workflow identity is unavailable"
    query = urllib.parse.urlencode({"branch": branch, "head_sha": sha,
                                    "per_page": 100})
    data = get(f"{base}/workflows/{workflow['id']}/runs?{query}")
    runs = data["workflow_runs"]
    # Do not omit failures with a status=success query, or skip through a
    # truncated page which might hide a later failed rerun.
    if data["total_count"] != len(runs):
        return True, "workflow history is incomplete"
    runs = sorted(runs, key=lambda run: timestamp(run["updated_at"]), reverse=True)
    for run in runs:
        if str(run["id"]) == env["GITHUB_RUN_ID"]:
            continue
        if (run.get("workflow_id") != workflow["id"]
                or run.get("path") != WORKFLOW or run.get("head_sha") != sha
                or run.get("head_branch") != branch
                or run.get("head_repository", {}).get("full_name") != repo
                or run.get("event") not in {"schedule", "workflow_dispatch"}):
            return True, "unexpected workflow, revision, branch, or event in history"
        if (run.get("status") != "completed" or run.get("conclusion") != "success"):
            return True, "a newer run failed, was cancelled, or is incomplete"
        age = now - timestamp(run["created_at"])
        if not timedelta(0) <= age < MAX_AGE:
            return True, "last full coverage is outside the seven-day window"
        run_id, attempt = run["id"], run["run_attempt"]
        if not isinstance(run_id, int) or not isinstance(attempt, int) or attempt < 1:
            raise ValueError("invalid run attempt")
        data = get(f"{base}/runs/{run_id}/attempts/{attempt}/jobs?per_page=100")
        jobs = data["jobs"]
        if data["total_count"] != len(jobs) or any(
                job.get("run_id") != run_id or job.get("run_attempt") != attempt
                for job in jobs):
            return True, "job history is incomplete or belongs to another attempt"
        if complete_coverage(jobs, expected, sha):
            return False, f"all {SHARDS} matching shards passed in run {run_id}"
        if not deliberate_reuse(jobs, sha):
            return True, "previous run lacks complete matching environment coverage"
    return True, "no reusable full Valgrind run found"


def write_output(values: dict[str, str]) -> None:
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("fingerprint", "decide"))
    args = parser.parse_args()
    if args.mode == "fingerprint":
        value = ""
        try:
            value = fingerprint(dict(os.environ))
        except Exception as exc:
            # Missing evidence must cost extra testing, never suppress it.
            print(f"Fingerprint unavailable; full coverage required: {exc}")
        write_output({"fingerprint": value})
    else:
        run, reason = True, "guard did not establish reusable coverage"
        try:
            run, reason = decide(dict(os.environ), os.environ.get("VALGRIND_FINGERPRINT", ""))
        except Exception as exc:
            reason = f"history unavailable; full coverage required: {exc}"
        print(reason)
        write_output({"run": str(run).lower()})
        if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(summary, "a", encoding="utf-8") as output:
                output.write(f"Valgrind: {'run all 16 shards' if run else 'reuse coverage'}. {reason}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
