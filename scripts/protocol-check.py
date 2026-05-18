#!/usr/bin/env python3
"""Validate Signal protocol discovery from a running relay."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


REQUIRED_STREAMS = {
    "STATION_IDENTITY": {"class": "static", "header_size": 1955},
    "STATION_DIAG": {"class": "live", "header_size": 3, "record_size": 1},
    "WORLD_PLAYERS": {"class": "live", "record_size": 77},
    "LATENCY_PING": {"class": "live", "header_size": 9},
    "LATENCY_PONG": {"class": "live", "header_size": 17},
    "CLIENT_METRICS": {"class": "live", "header_size": 21},
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
