#!/usr/bin/env python3
"""Validate Signal protocol discovery from a running relay."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


REQUIRED_STREAMS = {
    "STATION_IDENTITY": {"class": "static", "header_size": 2003},
    "STATION_IDENTITY_Q": {"class": "static", "header_size": 3},
    "STATION_DIAG": {"class": "live", "header_size": 3, "record_size": 1},
    "WORLD_PLAYERS": {"class": "live", "record_size": 77},
    "WORLD_PLAYER_MOTION_Q": {"class": "live", "header_size": 2, "record_size": 10},
    "WORLD_PLAYER_MOTIOND_Q": {"class": "live", "header_size": 2, "record_size": 6},
    "WORLD_PLAYER_POSED_Q": {"class": "live", "header_size": 2, "record_size": 4},
    "WORLD_PLAYER_MOTIONM_Q": {"class": "live", "header_size": 2, "record_size": 0},
    "WORLD_PLAYER_DOCK_Q": {"class": "live", "header_size": 2, "record_size": 2},
    "WORLD_ASTEROIDS_Q": {"class": "live", "header_size": 3, "record_size": 19},
    "WORLD_ASTEROIDS8_Q": {"class": "live", "header_size": 2, "record_size": 18},
    "WORLD_ASTEROID_STATE_Q": {"class": "live", "header_size": 3, "record_size": 18},
    "WORLD_ASTEROID_REMOVE": {"class": "live", "header_size": 3, "record_size": 2},
    "WORLD_ASTEROID_POS8_Q": {"class": "live", "header_size": 2, "record_size": 5},
    "WORLD_ASTEROID_POSD_Q": {"class": "live", "header_size": 3, "record_size": 4},
    "WORLD_ASTEROID_POSD8_Q": {"class": "live", "header_size": 2, "record_size": 3},
    "WORLD_STATIONS_Q": {"class": "econ", "header_size": 2},
    "WORLD_NPC_MOTION_Q": {"class": "live", "header_size": 2, "record_size": 12},
    "WORLD_NPC_MOTION8_Q": {"class": "live", "header_size": 2, "record_size": 9},
    "WORLD_NPC_POS_Q": {"class": "live", "header_size": 2, "record_size": 5},
    "WORLD_NPC_POSE_Q": {"class": "live", "header_size": 2, "record_size": 7},
    "WORLD_NPC_LINEAR_Q": {"class": "live", "header_size": 2, "record_size": 9},
    "WORLD_NPC_STATUS": {"class": "live", "header_size": 2, "record_size": 6},
    "WORLD_NPC_STATUS8_Q": {"class": "live", "header_size": 2, "record_size": 4},
    "WORLD_SCAFFOLDS": {"class": "live", "header_size": 2, "record_size": 28},
    "WORLD_SCAFFOLD_REMOVE": {"class": "live", "header_size": 2, "record_size": 1},
    "WORLD_SCAFFOLD_MOTION_Q": {"class": "live", "header_size": 2, "record_size": 9},
    "WORLD_CARGO_POD_MOTION_Q": {"class": "live", "header_size": 2, "record_size": 11},
    "WORLD_CARGO_POD_LINEAR_Q": {"class": "live", "header_size": 2, "record_size": 9},
    "WORLD_CARGO_POD_REMOVE": {"class": "live", "header_size": 2, "record_size": 1},
    "WORLD_CARGO_PODS_Q": {"class": "live", "header_size": 2, "record_size": 28},
    "WORLD_INTERACTIONS": {"class": "live", "header_size": 2, "record_size": 38},
    "WORLD_INTERACTIONS_Q": {"class": "live", "header_size": 2, "record_size": 25},
    "WORLD_INTERACTION_DRIFT": {"class": "live", "header_size": 2, "record_size": 12},
    "LATENCY_PING": {"class": "live", "header_size": 9},
    "LATENCY_PONG": {"class": "live", "header_size": 21},
    "CLIENT_METRICS": {"class": "live", "header_size": 21},
    "CONTRACTS_Q": {"class": "econ", "header_size": 2},
    "CARGO_RECEIPT_BUNDLE": {"class": "auth", "record_size": 208},
    "PRESENT_RECEIPT_CHAIN": {"class": "auth", "record_size": 208},
}


def fetch_json(url: str) -> dict:
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        body = response.read()
    return json.loads(body.decode("utf-8"))


def check_protocol(info: dict, *, expect_version: int | None) -> list[str]:
    errors: list[str] = []
    if expect_version is not None and info.get("version") != expect_version:
        errors.append(f"version: got {info.get('version')!r}, want {expect_version}")

    streams = info.get("streams")
    if not isinstance(streams, list):
        return ["streams: missing or not an array"]

    by_name = {stream.get("name"): stream for stream in streams if isinstance(stream, dict)}
    for name, expected in REQUIRED_STREAMS.items():
        stream = by_name.get(name)
        if stream is None:
            errors.append(f"{name}: missing")
            continue
        for key, want in expected.items():
            got = stream.get(key)
            if got != want:
                errors.append(f"{name}.{key}: got {got!r}, want {want!r}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:9091/api/protocol",
        help="protocol endpoint URL",
    )
    parser.add_argument(
        "--expect-version",
        type=int,
        default=1,
        help="expected protocol discovery version; set to 0 to skip",
    )
    parser.add_argument("--json", action="store_true", help="print raw protocol JSON")
    args = parser.parse_args()

    try:
        info = fetch_json(args.url)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        print(f"protocol-check: fetch failed: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(info, indent=2, sort_keys=True))

    expect_version = None if args.expect_version == 0 else args.expect_version
    errors = check_protocol(info, expect_version=expect_version)
    if errors:
        for error in errors:
            print(f"protocol-check: {error}", file=sys.stderr)
        return 1

    print(
        "protocol-check: ok "
        f"version={info.get('version')} streams={info.get('stream_count')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
