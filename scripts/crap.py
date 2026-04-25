#!/usr/bin/env python3
"""Compute CRAP per function by joining lizard complexity + gcovr coverage.

CRAP(m) = ccn(m)^2 * (1 - cov(m))^3 + ccn(m)

A function scoring > threshold (default 30) is "crappy": complex AND
not well covered by tests. Lowering complexity OR raising coverage
lowers the score.
"""
import argparse
import json
import os
import subprocess
import sys


def _unq(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        s = s[1:-1]
    return s


def run_lizard(paths):
    out = subprocess.check_output(["lizard", "--csv", *paths], text=True)
    rows = []
    for line in out.splitlines():
        parts = line.split(",")
        if len(parts) < 11:
            continue
        try:
            nloc = int(parts[0])
            ccn = int(parts[1])
            start = int(parts[9])
            end = int(parts[10])
        except ValueError:
            continue
        rows.append({
            "file": _unq(parts[6]),
            "func": _unq(parts[7]),
            "nloc": nloc,
            "ccn": ccn,
            "start": start,
            "end": end,
        })
    return rows


def load_gcovr(path):
    """Return dict keyed by BOTH absolute and repo-relative normalized paths."""
    with open(path) as f:
        j = json.load(f)
    root = os.path.normpath(j.get("root", os.getcwd()))
    cov = {}
    for fe in j.get("files", []):
        lines = {}
        for ln in fe.get("lines", []):
            if ln.get("gcovr/noncode"):
                continue
            lines[ln["line_number"]] = ln["count"]
        abs_path = os.path.normpath(os.path.join(root, fe["file"]))
        cov[abs_path] = lines
        # Also index by suffix (relative path) for loose matching.
        cov[os.path.normpath(fe["file"])] = lines
    return cov


def coverage_for(func, cov):
    key = os.path.normpath(func["file"])
    lines = cov.get(key)
    if lines is None:
        abs_key = os.path.normpath(os.path.abspath(key))
        lines = cov.get(abs_key)
    if lines is None:
        for k, v in cov.items():
            if k.endswith(os.sep + key) or key.endswith(os.sep + k):
                lines = v
                break
    if not lines:
        return None
    hits = [h for ln, h in lines.items() if func["start"] <= ln <= func["end"]]
    if not hits:
        return None
    covered = sum(1 for h in hits if h > 0)
    return covered / len(hits)


def crap(ccn, cov):
    c = 0.0 if cov is None else cov
    return ccn * ccn * (1 - c) ** 3 + ccn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coverage", required=True, help="gcovr --json output file")
    ap.add_argument("--threshold", type=float, default=30.0)
    ap.add_argument("--top", type=int, default=50)
    ap.add_argument("--paths", nargs="+", default=["server", "src", "shared"])
    ap.add_argument("--exclude", nargs="*", default=[
        "server/mongoose.c", "server/mongoose.h",
        "src/stb_image.h", "src/pl_mpeg.h", "src/minimp3.h",
    ], help="file paths to drop from the report (vendored code by default)")
    ap.add_argument("--fail-on-exceed", action="store_true",
                    help="exit 1 if any function exceeds threshold")
    ap.add_argument("--json-out", help="write full results as JSON")
    args = ap.parse_args()

    cov = load_gcovr(args.coverage)
    funcs = run_lizard(args.paths)
    excluded = {os.path.normpath(p) for p in args.exclude}
    funcs = [f for f in funcs if os.path.normpath(f["file"]) not in excluded]

    rows = []
    for f in funcs:
        c = coverage_for(f, cov)
        rows.append({
            "crap": crap(f["ccn"], c),
            "ccn": f["ccn"],
            "cov": c,
            "file": f["file"],
            "func": f["func"],
            "line": f["start"],
        })
    rows.sort(key=lambda r: r["crap"], reverse=True)
    bad = [r for r in rows if r["crap"] > args.threshold]

    print(f"{'CRAP':>8}  {'CCN':>4}  {'COV':>5}  LOCATION")
    print("-" * 72)
    for r in rows[:args.top]:
        cs = f"{r['cov']*100:4.0f}%" if r["cov"] is not None else "  n/a"
        print(f"{r['crap']:8.1f}  {r['ccn']:4d}  {cs}  {r['file']}:{r['line']} {r['func']}")
    print()
    print(f"{len(bad)}/{len(rows)} functions exceed CRAP threshold {args.threshold}")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"threshold": args.threshold, "functions": rows}, f, indent=2)

    if args.fail_on_exceed and bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
