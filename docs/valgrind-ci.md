# Nightly Valgrind coverage reuse

`.github/workflows/valgrind.yml` keeps the full 16-shard, non-soak memcheck
suite off the deploy path. A small planning job can avoid repeating a passing
suite when both code and the measured environment are unchanged.

## When all 16 shards run

- Every Sunday at 10:17 UTC, including a delayed delivery of that schedule
- Every manual `workflow_dispatch` and **Re-run all jobs**
- Any new commit or workflow revision
- A changed runner image, kernel, installed package version, compiler, linker,
  CMake, Make, Valgrind, or relevant build environment variable
- No complete matching success in the preceding seven days
- A newer failed, cancelled, timed-out, or unfinished run
- Missing or unfamiliar environment/history evidence, API errors, rate limits,
  truncated history, or a failed planning job

The original matrix, test launcher, memcheck flags, one-hour shard limit, and
failure diagnostic artifacts are retained. Pre-merge required CI is unchanged
apart from adding the guard's regression tests to the policy job.

## What qualifies as reusable coverage

The guard queries the exact `valgrind.yml` workflow using GitHub's public
read-only API. It validates workflow ID/path, repository, branch, source SHA,
workflow SHA/ref, event, run attempt, and age. A complete success must contain
all 16 distinct successful shard jobs. Each shard records a successful final
step containing its independently calculated environment fingerprint, after
memcheck passes. All fingerprints must match the current planning job.

The fingerprint includes the entire commit (covering workflow arguments,
CMake inputs, vendored dependencies, test registration and launcher), workflow
bytes, runner image identity, OS/kernel, installed Debian package versions,
actual tool binary hashes/versions, GCC target/specs, and relevant compiler,
linker and library environment variables. Unknown fingerprint inputs prevent
reuse. Image rollouts that produce different environments across shards also
prevent reuse until a full matching run exists.

Skipped runs are never counted as new test successes: the guard follows their
explicit reuse marker back to the original complete run, whose seven-day
expiry does not move. It inspects the latest attempt and does not filter
history to successful runs, so a subsequent failure cannot be hidden behind
an older pass. Existing runs from before these proof steps were added do not
qualify; the first run after adoption is always full.

## Operations and limitations

No new secret, cache, artifact, or token permission is required. Public API
reads are unauthenticated; private repositories or exhausted API rate limits
fall back to full testing. More than 100 same-revision workflow runs also
fall back to full testing rather than trusting a truncated history page.
The planning job still installs Valgrind to measure the actual post-install
dependency versions. The optimization therefore saves the 16 builds and
memchecks, not every runner minute on an unchanged night.

Use **Run workflow** in the Nightly Valgrind Actions page for an unconditional
full check. The planning job summary states whether coverage was reused and
identifies the original successful run. Each full shard shows its environment
fingerprint in its final successful step. GitHub's **Re-run failed jobs** or a
single-job rerun can run only a subset of shards; a partial attempt never
qualifies as reusable full coverage.

Run the dependency-free guard tests with:

```sh
python3 scripts/test_valgrind_guard.py
```
