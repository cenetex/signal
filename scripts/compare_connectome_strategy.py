#!/usr/bin/env python3
"""Run paired fresh-world episodes with a fixed connectome and strategy toggle."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
import platform
import tarfile
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
METRICS = (
    "smelt_output_units", "delivered_units", "contract_completions",
    "craft_events", "construction_contributions", "destroyed_ships",
    "observed_hull_loss", "distance", "active_ticks", "travel_ticks",
    "docked_ticks", "idle_ticks", "towing_ticks",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(row: dict, seed: int, ticks: int, strategy: bool) -> None:
    if row.get("schema") != 1 or row.get("seed") != seed or row.get("ticks") != ticks:
        raise ValueError("probe identity differs from requested episode")
    if row.get("strategy") is not strategy or row.get("chain_verified") is not True:
        raise ValueError("probe mode or chain verification failed")
    if row.get("event_capacity_ticks") != 0:
        raise ValueError("event capacity reached; completion count needs review")
    for key in METRICS:
        value = row.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
            raise ValueError(f"invalid metric: {key}")
    if strategy and not row.get("strategy_changes"):
        raise ValueError("strategy did not sample a posture")
    if not strategy and row.get("strategy_changes"):
        raise ValueError("disabled strategy sampled a posture")


def summarize(rows: list[dict], seeds: list[int]) -> dict:
    pairs = []
    for seed in seeds:
        pair = {r["strategy"]: r for r in rows if r["seed"] == seed}
        if len(pair) != 2 or sum(r["seed"] == seed for r in rows) != 2:
            raise ValueError("each seed needs exactly one episode per mode")
        off, on = pair[False], pair[True]
        if off["initial_rng"] != on["initial_rng"]:
            raise ValueError("paired initial worlds differ")
        pairs.append({"seed": seed, "delta_on_minus_off": {
            key: on[key] - off[key] for key in METRICS}})
    totals = {}
    for key in METRICS:
        deltas = [pair["delta_on_minus_off"][key] for pair in pairs]
        totals[key] = {
            "off": sum(r[key] for r in rows if not r["strategy"]),
            "on": sum(r[key] for r in rows if r["strategy"]),
            "median_delta": statistics.median(deltas),
            "min_delta": min(deltas), "max_delta": max(deltas),
            "higher": sum(d > 0 for d in deltas),
            "equal": sum(d == 0 for d in deltas),
            "lower": sum(d < 0 for d in deltas),
        }
    return {"pairs": pairs, "totals": totals}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--circuit", type=Path, default=ROOT / "assets/connectome/nav.cnx")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ticks", type=int, default=36000)
    parser.add_argument("--seeds", type=int, nargs="+", default=[2037, 2141, 3253, 4363, 5471])
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if not 1 <= args.ticks <= 1000000 or not 1 <= args.workers <= 4:
        parser.error("ticks must be 1..1000000 and workers 1..4")
    if len(set(args.seeds)) != len(args.seeds) or any(not 1 <= s <= 0xffffffff for s in args.seeds):
        parser.error("seeds must be distinct positive uint32 values")
    source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True):
        parser.error("commit study sources before running")
    probe, circuit = args.probe.resolve(), args.circuit.resolve()
    cache = (probe.parent / "CMakeCache.txt").read_text()
    source_root = next(line.split("=", 1)[1] for line in cache.splitlines()
                       if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="))
    if Path(source_root).resolve() != ROOT.resolve():
        parser.error("probe build must belong to this checkout")
    subprocess.run(["cmake", "--build", str(probe.parent), "--target", "connectome_swarm_probe",
                    "-j", str(args.workers)], check=True)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = {"PATH": os.defpath, "LC_ALL": "C", "TZ": "UTC",
           "SIGNAL_CONNECTOME_FAST": str(circuit),
           "SIGNAL_CONNECTOME_BUDGET": "30", "SIGNAL_CONNECTOME_STRATEGY_PERIOD": "300",
           "SIGNAL_CONNECTOME_SEED": "42"}
    manifest = {"schema": "signal.connectome_strategy_comparison.v1", "source_commit": source,
                "platform": platform.platform(), "machine": platform.machine(),
                "build_type": next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("CMAKE_BUILD_TYPE:STRING=")),
                "probe_sha256": sha256(probe), "circuit_sha256": sha256(circuit),
                "runner_sha256": sha256(Path(__file__)), "ticks": args.ticks,
                "seeds": args.seeds, "workers": args.workers,
                "settings": {k: v for k, v in env.items() if k.startswith("SIGNAL_") and k != "SIGNAL_CONNECTOME_FAST"},
                "environment": "isolated; default settings plus listed overrides",
                "scenario": "fresh server genesis; no connected players; normal NPC respawn",
                "status": "running"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def episode(job: tuple[int, bool, bool]) -> dict:
        seed, strategy, repeat = job
        label = f"{seed}-{'on' if strategy else 'off'}{'-repeat' if repeat else ''}"
        result_path = out / f"{label}.json"
        start = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="signal-study-") as tmp:
            cmd = [str(probe), str(args.ticks), "--seed", str(seed),
                   "--json", str(result_path), "--chain-dir", str(Path(tmp) / "chain")]
            with (out / f"{label}.log").open("w") as log:
                subprocess.run(cmd, cwd=tmp, env={**env, "SIGNAL_CONNECTOME_STRATEGY": str(int(strategy))},
                               stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout, check=True)
            with tarfile.open(out / f"{label}-chain.tar.gz", "w:gz") as archive:
                archive.add(Path(tmp) / "chain", arcname="chain")
        row = json.loads(result_path.read_text())
        validate(row, seed, args.ticks, strategy)
        print(f"{label}: {time.monotonic() - start:.1f}s; smelt={row['smelt_output_units']}, "
              f"delivered={row['delivered_units']}, lost={row['destroyed_ships']}", flush=True)
        return row

    jobs = [(seed, strategy, False) for seed in args.seeds for strategy in (False, True)]
    jobs += [(args.seeds[0], strategy, True) for strategy in (False, True)]
    try:
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            results = list(pool.map(episode, jobs))
        rows, repeats = results[:-2], results[-2:]
        if repeats != rows[:2]:
            raise ValueError("full-horizon repeat differs from original episode")
        report = summarize(rows, args.seeds)
        report.update({"episodes": rows, "repeat_seed": args.seeds[0], "repeat_exact": True})
        (out / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
        manifest["status"] = "complete"
    except Exception as error:
        manifest.update(status="failed", error=str(error))
        raise
    finally:
        manifest["artifacts"] = {p.name: sha256(p) for p in sorted(out.iterdir()) if p.name != "manifest.json"}
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
