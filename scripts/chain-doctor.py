#!/usr/bin/env python3
"""Operator-facing chain health diagnostic for a running Signal server.

The server remains the source of truth: this tool only reads /health and
prints the chain health, blocked stations, verifier messages, and repair hints.
It never edits world.sav or chain logs.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


DEFAULT_URL = "http://127.0.0.1:9091/health"


def load_health(url: str, timeout: float) -> dict:
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        if resp.status != 200:
            raise RuntimeError(f"{url}: HTTP {resp.status}")
        return json.loads(resp.read().decode("utf-8"))


def fallback_hint(health: str, blocked: bool) -> str:
    if health == "failed":
        return (
            "Preserve the damaged log, run signal_verify, then restore a "
            "matching save+chain backup or quarantine the bad log before "
            "starting a new branch."
        )
    if health == "mismatch":
        return (
            "Restore the matching world.sav and chain directory, or back up "
            "both and reset/re-anchor them together; do not append from the "
            "saved head."
        )
    if blocked:
        return "Verify the station log before repair; appends are blocked."
    return "No repair needed."


def station_label(st: dict) -> str:
    idx = st.get("index", "?")
    name = st.get("name") or "unknown station"
    return f"station {idx} ({name})"


def print_text(doc: dict) -> None:
    chain = doc.get("chain", {})
    chain_status = chain.get("status", "unknown")
    version = doc.get("version", "unknown")
    players = doc.get("players", "?")
    chain_dir = chain.get("chain_dir", "chain")

    print(f"Signal chain doctor: version {version}, players {players}")
    print(f"chain status: {chain_status}")
    print(f"chain dir:    {chain_dir}")
    print(
        "stations:     "
        f"{chain.get('stations', 0)} total, "
        f"{chain.get('blocked_stations', 0)} blocked, "
        f"{chain.get('warning_stations', 0)} warning"
    )
    print()

    stations = chain.get("station_status") or []
    if not stations:
        print("No station chain status found in /health.")
        return

    for st in stations:
        health = st.get("health", "unknown")
        blocked = bool(st.get("append_blocked"))
        badge = "BLOCKED" if blocked else ("WARN" if health in {"unknown", "adopted"} else "OK")
        print(f"[{badge}] {station_label(st)}")
        print(f"  health:     {health}")
        print(f"  event head: {st.get('event_count', 0)} saved / {st.get('verified_event_count', 0)} verified")
        msg = st.get("message") or ""
        if msg:
            print(f"  verifier:   {msg}")
        hint = st.get("repair_hint") or fallback_hint(health, blocked)
        print(f"  next step:  {hint}")
        print()

    if chain_status == "blocked":
        print("No automatic repair was applied. Preserve world.sav and chain/ before any manual repair.")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Read Signal /health and print operator-visible chain repair guidance."
    )
    ap.add_argument("--url", default=DEFAULT_URL, help=f"health endpoint (default: {DEFAULT_URL})")
    ap.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout in seconds")
    ap.add_argument("--json", action="store_true", help="print raw health JSON")
    ap.add_argument(
        "--fail-on-blocked",
        action="store_true",
        help="exit 1 when any station has append_blocked=true",
    )
    args = ap.parse_args(argv)

    try:
        doc = load_health(args.url, args.timeout)
    except (OSError, urllib.error.URLError, json.JSONDecodeError, RuntimeError) as exc:
        print(f"chain-doctor: failed to read {args.url}: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(doc, indent=2, sort_keys=True))
    else:
        print_text(doc)

    if args.fail_on_blocked:
        chain = doc.get("chain", {})
        if any(st.get("append_blocked") for st in chain.get("station_status") or []):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
