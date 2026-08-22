#!/usr/bin/env python3
"""Validate the local Fly deployment contract without credentials.

``flyctl config validate`` requires an authenticated session even for a local
file. Required PR CI therefore checks the fields coupled to this repository
locally, while the authenticated deploy workflow retains Fly's platform-side
validation.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
FLY_TOML = ROOT / "fly.toml"
WORKER_SOURCE = ROOT / "workers" / "fly-proxy.js"


def _port(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.isdigit():
        return int(value)
    return None


def fly_config_failures(
    config: dict[str, Any],
    worker_source: str,
    *,
    root: Path = ROOT,
) -> list[str]:
    failures: list[str] = []

    app = config.get("app")
    app_is_valid = isinstance(app, str) and re.fullmatch(
        r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", app
    ) is not None
    if not app_is_valid:
        failures.append("app must be a non-empty Fly-compatible name")
    worker_origins = [
        match.group("value")
        for match in re.finditer(
            r"(?m)^\s*const\s+ORIGIN\s*=\s*"
            r"(?P<quote>['\"])(?P<value>[^'\"]+)(?P=quote)\s*;\s*$",
            worker_source,
        )
    ]
    if len(worker_origins) != 1:
        failures.append(
            "workers/fly-proxy.js must declare exactly one literal ORIGIN"
        )
    elif app_is_valid:
        expected_origin = f"https://{app}.fly.dev"
        if worker_origins[0] != expected_origin:
            failures.append(
                "workers/fly-proxy.js ORIGIN must be "
                f"{expected_origin!r}"
            )
    region = config.get("primary_region")
    if not isinstance(region, str) or not re.fullmatch(
        r"[a-z0-9-]+", region
    ):
        failures.append("primary_region must be a non-empty region slug")

    build = config.get("build")
    if not isinstance(build, dict):
        failures.append("[build] table is required")
    else:
        dockerfile = build.get("dockerfile")
        if dockerfile != "server/Dockerfile":
            failures.append(
                "[build].dockerfile must be server/Dockerfile"
            )
        elif not (root / dockerfile).is_file():
            failures.append(
                "[build].dockerfile does not exist in the repository"
            )
        context = build.get("context")
        if context not in {None, "."}:
            failures.append(
                "[build].context must be repository root when present"
            )

    env = config.get("env")
    if not isinstance(env, dict):
        failures.append("[env] table is required")
        env = {}
    expected_env = {
        "PORT": "8080",
        "SIGNAL_SERVER_PORT": "9091",
        "SIGNAL_DATA_DIR": "/app/data",
        "SIGNAL_STATIC_DIR": "/app/public",
        "SIGNAL_ALLOWED_ORIGIN": "https://signal.ratimics.com",
        "SIGNAL_REQUIRE_STATION_AUTH_SECRET": "1",
        "SIGNAL_TRUST_PROXY_HEADERS": "1",
        "RTC_GATEWAY_PREFIX": "/rtc",
        "RTC_GATEWAY_ICE_BIND": "fly-global-services",
        "RTC_GATEWAY_ICE_PORT": "50000",
        "RTC_GATEWAY_ICE_UDP_MUX": "1",
    }
    for key, expected in expected_env.items():
        if env.get(key) != expected:
            failures.append(f"[env].{key} must be {expected!r}")

    mounts = config.get("mounts")
    if not isinstance(mounts, dict):
        failures.append("[mounts] table is required")
    else:
        if not isinstance(mounts.get("source"), str) or not mounts["source"]:
            failures.append("[mounts].source must name a persistent volume")
        if mounts.get("destination") != env.get("SIGNAL_DATA_DIR"):
            failures.append(
                "[mounts].destination must equal SIGNAL_DATA_DIR"
            )

    http = config.get("http_service")
    if not isinstance(http, dict):
        failures.append("[http_service] table is required")
        http = {}
    if _port(http.get("internal_port")) != _port(env.get("PORT")):
        failures.append(
            "[http_service].internal_port must equal [env].PORT"
        )
    if http.get("force_https") is not True:
        failures.append("[http_service].force_https must be true")
    checks = http.get("checks")
    if not isinstance(checks, list):
        checks = []
    has_health = any(
        isinstance(check, dict)
        and check.get("method") == "GET"
        and check.get("path") == "/health"
        for check in checks
    )
    if not has_health:
        failures.append(
            "[[http_service.checks]] must GET /health"
        )

    services = config.get("services")
    if not isinstance(services, list):
        services = []
    ice_port = _port(env.get("RTC_GATEWAY_ICE_PORT"))
    has_udp_mux = False
    for service in services:
        if not isinstance(service, dict):
            continue
        ports = service.get("ports")
        exposed = set()
        if isinstance(ports, list):
            exposed = {
                _port(port.get("port"))
                for port in ports
                if isinstance(port, dict)
            }
        if (
            service.get("protocol") == "udp"
            and _port(service.get("internal_port")) == ice_port
            and ice_port in exposed
        ):
            has_udp_mux = True
    if not has_udp_mux:
        failures.append(
            "[[services]] must expose RTC_GATEWAY_ICE_PORT over UDP"
        )

    machines = config.get("vm")
    if not isinstance(machines, list) or len(machines) != 1:
        failures.append("fly.toml must declare exactly one [[vm]] table")
    else:
        machine = machines[0]
        if not isinstance(machine, dict):
            failures.append("[[vm]] must be a table")
        else:
            if machine.get("cpu_kind") != "performance":
                failures.append("[[vm]].cpu_kind must be 'performance'")
            if machine.get("cpus") != 1:
                failures.append("[[vm]].cpus must be 1")
            if machine.get("memory") != "2gb":
                failures.append("[[vm]].memory must be '2gb'")

    return failures


def main() -> int:
    try:
        config = tomllib.loads(FLY_TOML.read_text(encoding="utf-8"))
        worker_source = WORKER_SOURCE.read_text(encoding="utf-8")
    except (OSError, tomllib.TOMLDecodeError) as exc:
        print(f"fly.toml could not be parsed: {exc}", file=sys.stderr)
        return 2
    failures = fly_config_failures(config, worker_source)
    if failures:
        for failure in failures:
            print(f"fly.toml: {failure}", file=sys.stderr)
        return 1
    print("local Fly deployment contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
