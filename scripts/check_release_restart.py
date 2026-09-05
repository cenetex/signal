#!/usr/bin/env python3
"""Check a packaged server through two starts and graceful local shutdowns."""
import argparse
import json
import os
from pathlib import Path
import signal
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.request


def get_json(base, path, token):
    request = urllib.request.Request(base + path, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(request, timeout=2) as response:
        return json.load(response)


def run_epoch(binary, root, epoch, protocol, minimum_tick, token):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    environment = {key: os.environ[key] for key in ("PATH", "TMPDIR", "LANG")
                   if key in os.environ}
    environment.update(PORT=str(port), SIGNAL_BIND_HOST="127.0.0.1",
                       SIGNAL_DATA_DIR=str(root / "data"), SIGNAL_WORLD_SEED="2037",
                       SIGNAL_API_TOKEN=token, SIGNAL_STATION_AUTH_SECRET=token)
    base = f"http://127.0.0.1:{port}"
    with (root / f"server-{epoch}.log").open("wb") as log:
        process = subprocess.Popen([str(binary)], cwd=root, env=environment,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 30
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"Server exited during startup; inspect {log.name}")
                try:
                    health = get_json(base, "/health", token)
                    if health["world_tick"] > minimum_tick:
                        break
                except (OSError, urllib.error.URLError):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"Server startup timed out; inspect {log.name}")
                time.sleep(0.1)
            assert health["status"] == "ok", health
            assert health["chain"]["status"] == "ok", health["chain"]
            assert health["chain"]["stations"] == 4, health["chain"]
            assert health["players"] == 0, health
            discovered = get_json(base, "/api/protocol", token)
            assert discovered["version"] == protocol, discovered
            started = time.monotonic()
            process.send_signal(signal.SIGTERM)
            status = process.wait(timeout=10)
            elapsed = time.monotonic() - started
            assert status == 0, f"Shutdown exit {status}; inspect {log.name}"
            return {"version": health["version"], "world_tick": health["world_tick"],
                    "protocol": discovered["version"], "healthy_chains": 4,
                    "shutdown_seconds": round(elapsed, 3)}
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--protocol", required=True, type=int)
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("This graceful-signal check uses a POSIX host")
    binary = args.server.resolve(strict=True)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    token = secrets.token_hex(32)
    first = run_epoch(binary, root, 1, args.protocol, 120, token)
    marker = root / "data/.signal-generations/CURRENT"
    previous = marker.read_bytes()
    second = run_epoch(binary, root, 2, args.protocol, first["world_tick"], token)
    assert marker.read_bytes() != previous, "Second shutdown must publish a new generation"
    assert second["version"] == first["version"], "Both starts must use the same binary"
    report = {"binary": str(binary), "epochs": [first, second], "passed": True}
    (root / "restart-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
