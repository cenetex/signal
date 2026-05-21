#!/usr/bin/env python3
"""Run heuristic-vs-neural bot gap trials against matched Signal worlds."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Any
from urllib.error import URLError
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = Path("/tmp/signal-neural-gap")
DEFAULT_CHECKPOINTS = (
    ROOT / "build/float/signal_flight_longhorizon_live/signal_flight.nnckpt",
    Path("/Users/ratimics/develop/crlplrimes/build/float/signal_flight_longhorizon_live/signal_flight.nnckpt"),
)

SMELT_RE = re.compile(
    r"\[smelt\] station (?P<station>\d+) (?P<commodity>[A-Z]+) Ingot "
    r"grade=(?P<grade>\d+) ore=(?P<ore>[0-9.]+) units=(?P<units>\d+) pushed=(?P<pushed>\d+)"
)
STUCK_RE = re.compile(r"\[autopilot\] player (?P<player>\d+) stuck for 8s")
DOCK_RE = re.compile(r"\[sim\] player (?P<player>\d+) docked at station (?P<station>\d+)")
LAUNCH_RE = re.compile(r"\[sim\] player (?P<player>\d+) launched")
BUY_REQ_RE = re.compile(r"\[buy\] player (?P<player>\d+) req c=(?P<commodity>\d+)")
BUY_OK_RE = re.compile(r"\[buy\] OK player (?P<player>\d+) bought (?P<qty>[0-9.]+) of c=(?P<commodity>\d+) for (?P<credits>[0-9.]+)")
BUY_REJECT_RE = re.compile(r"\[buy\] REJECT")
SOLD_CARGO_RE = re.compile(r"\[sim\] player (?P<player>\d+) sold cargo for (?P<credits>[0-9.]+) cr at (?P<station>.+)")
SOLD_UNIT_RE = re.compile(r"\[sim\] player (?P<player>\d+) sold 1.* for (?P<credits>[0-9.]+) cr at (?P<station>.+)")
REPAIR_RE = re.compile(r"\[sim\] player (?P<player>\d+) repaired (?P<hp>\d+) HP")
KIT_RE = re.compile(r"\[shipyard-fab\] station (?P<station>\d+) minted (?P<count>\d+) kits")
FRACTURE_RE = re.compile(r"\[sim\] asteroid (?P<asteroid>\d+) fractured into (?P<count>\d+) children")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Run matched heuristic and neural Signal bot trials and summarize behavior gaps."
    )
    ap.add_argument("--server", default=str(ROOT / "build/signal_server"))
    ap.add_argument("--checkpoint", default=os.environ.get("SIGNAL_BOT_BRAIN_CHECKPOINT", ""))
    ap.add_argument("--contract-checkpoint", default=os.environ.get("SIGNAL_BOT_CONTRACT_BRAIN_CHECKPOINT", ""))
    ap.add_argument("--duration", type=float, default=120.0, help="seconds per mode")
    ap.add_argument("--bots", type=int, default=31)
    ap.add_argument("--seed", type=int, default=2037)
    ap.add_argument("--world-seq", type=int, default=2037)
    ap.add_argument("--base-port", type=int, default=19191)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--modes", default="heuristic,neural", help="comma list: autopilot,heuristic,neural")
    return ap.parse_args()


def resolve_checkpoint(path: str) -> str:
    if path:
        return path
    for candidate in DEFAULT_CHECKPOINTS:
        if candidate.exists():
            return str(candidate)
    return ""


def fetch_json(url: str, timeout: float = 1.0) -> dict[str, Any] | None:
    try:
        with urlopen(url, timeout=timeout) as resp:
            if resp.status != 200:
                return None
            return json.loads(resp.read().decode("utf-8"))
    except (OSError, URLError, TimeoutError, json.JSONDecodeError):
        return None


def wait_for_health(port: int, proc: subprocess.Popen[Any], timeout: float = 2.0) -> dict[str, Any] | None:
    deadline = time.monotonic() + timeout
    url = f"http://127.0.0.1:{port}/health"
    last = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return last
        last = fetch_json(url, timeout=0.5)
        if last:
            return last
        time.sleep(0.2)
    return last


def terminate(proc: subprocess.Popen[Any]) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def inc(mapping: dict[str, int], key: str, amount: int = 1) -> None:
    mapping[key] = mapping.get(key, 0) + amount


def parse_log(path: Path) -> dict[str, Any]:
    text = path.read_text(errors="replace") if path.exists() else ""
    metrics: dict[str, Any] = {
        "bot_players_spawned": text.count("bot player "),
        "server_brain_loaded": "loaded neural bot brain checkpoint" in text,
        "contract_teacher_fallback": "contract brain: teacher fallback" in text,
        "contract_brain_loaded": "loaded neural contract brain checkpoint" in text,
        "stuck_replans_total": 0,
        "stuck_replans_by_player": {},
        "smelt_events_total": 0,
        "smelt_units_pushed": 0,
        "smelt_ore_total": 0.0,
        "zero_push_smelt_events": 0,
        "smelt_by_station_commodity": {},
        "staged_crystal_fragments": text.count("staged crystal fragment"),
        "dock_events": 0,
        "launch_events": 0,
        "buy_requests": 0,
        "buy_ok": 0,
        "buy_rejects": 0,
        "buy_credits_spent": 0.0,
        "sold_cargo_events": 0,
        "sold_cargo_credits": 0.0,
        "sold_unit_events": 0,
        "sold_unit_credits": 0.0,
        "repair_events": 0,
        "repair_hp": 0,
        "repair_kits_minted": 0,
        "asteroid_fractures": 0,
    }
    for line in text.splitlines():
        if m := STUCK_RE.search(line):
            metrics["stuck_replans_total"] += 1
            inc(metrics["stuck_replans_by_player"], m.group("player"))
            continue
        if m := SMELT_RE.search(line):
            station = m.group("station")
            commodity = m.group("commodity")
            units = int(m.group("units"))
            pushed = int(m.group("pushed"))
            ore = float(m.group("ore"))
            key = f"{station}:{commodity}"
            metrics["smelt_events_total"] += 1
            metrics["smelt_units_pushed"] += pushed
            metrics["smelt_ore_total"] += ore
            if pushed == 0:
                metrics["zero_push_smelt_events"] += 1
            inc(metrics["smelt_by_station_commodity"], key, pushed)
            continue
        if DOCK_RE.search(line):
            metrics["dock_events"] += 1
            continue
        if LAUNCH_RE.search(line):
            metrics["launch_events"] += 1
            continue
        if BUY_REQ_RE.search(line):
            metrics["buy_requests"] += 1
            continue
        if m := BUY_OK_RE.search(line):
            metrics["buy_ok"] += 1
            metrics["buy_credits_spent"] += float(m.group("credits"))
            continue
        if BUY_REJECT_RE.search(line):
            metrics["buy_rejects"] += 1
            continue
        if m := SOLD_CARGO_RE.search(line):
            metrics["sold_cargo_events"] += 1
            metrics["sold_cargo_credits"] += float(m.group("credits"))
            continue
        if m := SOLD_UNIT_RE.search(line):
            metrics["sold_unit_events"] += 1
            metrics["sold_unit_credits"] += float(m.group("credits"))
            continue
        if m := REPAIR_RE.search(line):
            metrics["repair_events"] += 1
            metrics["repair_hp"] += int(m.group("hp"))
            continue
        if m := KIT_RE.search(line):
            metrics["repair_kits_minted"] += int(m.group("count"))
            continue
        if FRACTURE_RE.search(line):
            metrics["asteroid_fractures"] += 1
            continue
    return metrics


def add_rates(metrics: dict[str, Any], duration_s: float, bots: int) -> None:
    minutes = max(duration_s / 60.0, 1e-6)
    bot_minutes = max(minutes * max(bots, 1), 1e-6)
    metrics["smelt_units_per_min"] = metrics["smelt_units_pushed"] / minutes
    metrics["sold_credits_per_min"] = (
        metrics["sold_cargo_credits"] + metrics["sold_unit_credits"]
    ) / minutes
    metrics["stuck_replans_per_min"] = metrics["stuck_replans_total"] / minutes
    metrics["stuck_replans_per_bot_min"] = metrics["stuck_replans_total"] / bot_minutes
    metrics["dock_events_per_min"] = metrics["dock_events"] / minutes
    metrics["buy_ok_per_min"] = metrics["buy_ok"] / minutes


def run_case(args: argparse.Namespace, mode: str, port: int, checkpoint: str) -> dict[str, Any]:
    args.out_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.out_dir / f"{mode}.log"
    env = os.environ.copy()
    env.update(
        {
            "SIGNAL_PERSISTENCE_MODE": "ephemeral",
            "SIGNAL_DATA_DIR": str(args.out_dir / mode),
            "PORT": str(port),
            "SIGNAL_BOT_PLAYERS": str(args.bots),
            "SIGNAL_BOT_BRAIN_MODE": mode,
            "SIGNAL_WORLD_SEED": str(args.seed),
            "SIGNAL_WORLD_SEQ": str(args.world_seq),
        }
    )
    if mode == "neural":
        if not checkpoint:
            raise SystemExit("neural mode requires --checkpoint or SIGNAL_BOT_BRAIN_CHECKPOINT")
        env["SIGNAL_BOT_BRAIN_CHECKPOINT"] = checkpoint
        if args.contract_checkpoint:
            env["SIGNAL_BOT_CONTRACT_BRAIN_CHECKPOINT"] = args.contract_checkpoint

    started = time.time()
    with log_path.open("wb") as log:
        proc = subprocess.Popen(
            [args.server],
            cwd=ROOT,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        initial_health = wait_for_health(port, proc)
        if proc.poll() is not None:
            raise RuntimeError(f"{mode} server exited early; see {log_path}")
        time.sleep(args.duration)
        final_health = fetch_json(f"http://127.0.0.1:{port}/health", timeout=1.0)
        trace_weights = fetch_json(
            f"http://127.0.0.1:{port}/training/v1/bot-trace-weights",
            timeout=1.0,
        )
        terminate(proc)
        rc = proc.returncode

    elapsed = time.time() - started
    metrics = parse_log(log_path)
    if mode in {"heuristic", "neural"} and not metrics["contract_brain_loaded"]:
        metrics["contract_teacher_fallback"] = True
    add_rates(metrics, args.duration, args.bots)
    return {
        "mode": mode,
        "port": port,
        "returncode": rc,
        "duration_requested_s": args.duration,
        "duration_wall_s": elapsed,
        "log_path": str(log_path),
        "initial_health": initial_health,
        "final_health": final_health,
        "trace_weights": trace_weights,
        "metrics": metrics,
    }


def build_if_needed(args: argparse.Namespace) -> None:
    if args.skip_build:
        return
    subprocess.run(["make", "build-server"], cwd=ROOT, check=True)


def delta_report(runs: list[dict[str, Any]]) -> dict[str, Any]:
    by_mode = {run["mode"]: run for run in runs}
    baseline = "heuristic" if "heuristic" in by_mode else "autopilot"
    if baseline not in by_mode or "neural" not in by_mode:
        return {}
    a = by_mode[baseline]["metrics"]
    n = by_mode["neural"]["metrics"]
    keys = [
        "smelt_units_per_min",
        "sold_credits_per_min",
        "stuck_replans_per_bot_min",
        "dock_events_per_min",
        "buy_ok_per_min",
        "zero_push_smelt_events",
        "repair_kits_minted",
    ]
    out = {key: n.get(key, 0) - a.get(key, 0) for key in keys}
    out["baseline_mode"] = baseline
    return out


def write_markdown(report: dict[str, Any], path: Path) -> None:
    rows = []
    for run in report["runs"]:
        m = run["metrics"]
        rows.append(
            "| {mode} | {smelts:.1f} | {credits:.1f} | {stuck:.3f} | {dock:.1f} | {buy:.1f} | {zero} | {kits} |".format(
                mode=run["mode"],
                smelts=m["smelt_units_per_min"],
                credits=m["sold_credits_per_min"],
                stuck=m["stuck_replans_per_bot_min"],
                dock=m["dock_events_per_min"],
                buy=m["buy_ok_per_min"],
                zero=m["zero_push_smelt_events"],
                kits=m["repair_kits_minted"],
            )
        )
    body = [
        "# Signal Neural Gap A/B",
        "",
        f"- seed: `{report['seed']}`",
        f"- world_seq: `{report['world_seq']}`",
        f"- bots: `{report['bots']}`",
        f"- duration per mode: `{report['duration_s']:.1f}s`",
        "",
        "| mode | smelt units/min | sold credits/min | stuck replans/bot/min | docks/min | buys/min | zero-push smelts | kits minted |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        *rows,
        "",
        f"## Delta: neural - {report.get('delta', {}).get('baseline_mode', 'baseline')}",
        "",
    ]
    for key, value in report.get("delta", {}).items():
        if key == "baseline_mode":
            continue
        body.append(f"- `{key}`: `{value:.3f}`")
    body.extend(
        [
            "",
            "## Log Files",
            "",
            *[f"- `{run['mode']}`: `{run['log_path']}`" for run in report["runs"]],
            "",
        ]
    )
    path.write_text("\n".join(body))


def main() -> int:
    args = parse_args()
    checkpoint = resolve_checkpoint(args.checkpoint)
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for mode in modes:
        if mode not in {"autopilot", "heuristic", "neural"}:
            raise SystemExit(f"unsupported mode: {mode}")
    build_if_needed(args)

    runs: list[dict[str, Any]] = []
    for i, mode in enumerate(modes):
        print(f"[gap] running {mode} seed={args.seed} duration={args.duration:.1f}s", flush=True)
        runs.append(run_case(args, mode, args.base_port + i, checkpoint))

    report = {
        "schema": "signal.neural_gap_ab.v1",
        "seed": args.seed,
        "world_seq": args.world_seq,
        "bots": args.bots,
        "duration_s": args.duration,
        "checkpoint": checkpoint,
        "contract_checkpoint": args.contract_checkpoint,
        "runs": runs,
    }
    report["delta"] = delta_report(runs)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.out_dir / "report.json"
    md_path = args.out_dir / "report.md"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True))
    write_markdown(report, md_path)
    print(f"[gap] wrote {json_path}")
    print(f"[gap] wrote {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
