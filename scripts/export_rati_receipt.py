#!/usr/bin/env python3
"""Run Signal's station-log verifier and export a shared RATi receipt."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path

from receipt_format import blob, read_json, seal, verify

PROFILE = "signal.station-log.v1"
CLAIM = "The station log passes Signal's signature, chain, and smelt checks."
MAX_LOG_BYTES = 64 * 1024 * 1024


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def export_receipt(log: Path, verifier: Path, station_pubkey: str) -> dict:
    with log.open("rb") as source:
        raw = source.read(MAX_LOG_BYTES + 1)
    if len(raw) > MAX_LOG_BYTES:
        raise ValueError("Station logs must fit the native verifier's 64 MiB limit")
    executable = verifier.resolve()
    binary_digest = sha(executable.read_bytes())
    # The native verifier and the envelope read the same immutable snapshot.
    with tempfile.TemporaryDirectory(prefix="signal-rati-") as folder:
        snapshot = Path(folder) / "station.chain"
        snapshot.write_bytes(raw)
        completed = subprocess.run(
            [str(executable), f"--station-pubkey={station_pubkey}", str(snapshot)],
            check=True, capture_output=True, text=True, encoding="utf-8", timeout=120,
        )
    if sha(executable.read_bytes()) != binary_digest:
        raise ValueError("Verifier changed during the check")
    report = read_json(completed.stdout)
    if (report.get("schema") != "signal.rati_mining_receipts.v1"
            or report.get("source", {}).get("verified") is not True
            or not isinstance(report.get("receipts"), list)
            or report.get("receipt_count") != len(report["receipts"])):
        raise ValueError("Expected a complete native Signal verification report")
    log_digest = sha(raw)
    # Replace a temporary path with the exact source commitment.
    report["source"]["chain_log_path"] = f"sha256:{log_digest}"
    root = Path(__file__).resolve().parent
    rule_files = {name: sha((root / name).read_bytes())
                  for name in ("export_rati_receipt.py", "receipt_format.py")}
    rule_files["signal_rati_receipt"] = binary_digest
    rule_digest = sha(json.dumps(rule_files, sort_keys=True, separators=(",", ":")).encode())
    return seal({
        "schema": "rati.receipt.v1", "profile": PROFILE,
        "subject": {"kind": "signal.station-log", "id": f"sha256:{log_digest}"},
        "claim": CLAIM,
        "rules": {"id": "signal.station-log-verifier", "version": "1", "sha256": rule_digest},
        "facts": blob({"chain_log_sha256": log_digest,
                       "station_pubkey": station_pubkey, "rule_files": rule_files}),
        "result": blob(report), "outcome": "pass",
        "evaluated_at_unix_ms": str(time.time_ns() // 1_000_000),
        "validity": "snapshot", "issuer": "signal",
    })


def replay_receipt(receipt: dict, log: Path, verifier: Path, station_pubkey: str) -> bool:
    if not verify(receipt):
        return False
    fresh = export_receipt(log, verifier, station_pubkey)
    return all(fresh[key] == receipt[key]
               for key in fresh if key not in {"sha256", "evaluated_at_unix_ms"})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--verifier", type=Path, default=Path("build/signal_rati_receipt"))
    parser.add_argument("--station-pubkey", required=True)
    parser.add_argument("--verify-receipt", type=Path)
    args = parser.parse_args()
    if args.verify_receipt:
        receipt = read_json(args.verify_receipt.read_text(encoding="utf-8"))
        valid = replay_receipt(receipt, args.log, args.verifier, args.station_pubkey)
        print(json.dumps({"integrity": verify(receipt), "replay": valid}))
        return 0 if valid else 1
    print(json.dumps(export_receipt(args.log, args.verifier, args.station_pubkey), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
