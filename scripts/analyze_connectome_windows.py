#!/usr/bin/env python3
"""Count production by time window from a completed probe's verified archives."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tarfile


def smelt_windows(data: bytes, ticks: int, width: int) -> list[int]:
    counts = [0] * ((ticks + width - 1) // width)
    offset = 0
    while offset < len(data):
        if len(data) - offset < 186:
            raise ValueError("truncated chain header")
        epoch = struct.unpack_from("<Q", data, offset)[0]
        kind = data[offset + 16]
        size = struct.unpack_from("<H", data, offset + 184)[0]
        if offset + 186 + size > len(data) or epoch > ticks:
            raise ValueError("truncated payload or event beyond episode")
        if kind == 1 and epoch > 0:
            if size != 80:
                raise ValueError("unexpected smelt payload size")
            counts[(epoch - 1) // width] += 1
        offset += 186 + size
    return counts


def analyze(study: Path, width: int) -> dict:
    manifest = json.loads((study / "manifest.json").read_text())
    if manifest["status"] != "complete" or width <= 0:
        raise ValueError("a completed study and positive window are required")
    ticks = manifest["ticks"]
    def checked(name: str) -> Path:
        path = study / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest["artifacts"][name]:
            raise ValueError(f"artifact hash mismatch: {name}")
        return path
    report = json.loads(checked("comparison.json").read_text())
    rows = []
    for row in report["episodes"]:
        if row["chain_verified"] is not True:
            raise ValueError("probe history verification required")
        label = f"{row['seed']}-{'on' if row['strategy'] else 'off'}"
        counts = [0] * ((ticks + width - 1) // width)
        with tarfile.open(checked(f"{label}-chain.tar.gz")) as archive:
            for member in archive.getmembers():
                if member.isdir():
                    continue
                if not member.isfile() or not member.name.startswith("chain/") or not member.name.endswith(".log"):
                    raise ValueError("unexpected archive member")
                content = archive.extractfile(member)
                if content is None:
                    raise ValueError("missing archive member")
                bins = smelt_windows(content.read(), ticks, width)
                counts = [a + b for a, b in zip(counts, bins)]
        if sum(counts) != row["smelt_output_units"]:
            raise ValueError("window counts differ from verified production total")
        rows.append({"seed": row["seed"], "strategy": row["strategy"], "smelt_units": counts})
    return {"schema": "signal.connectome_production_windows.v1",
            "measured_source": manifest["source_commit"],
            "study_manifest_sha256": hashlib.sha256((study / "manifest.json").read_bytes()).hexdigest(),
            "analyzer_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "ticks": ticks,
            "window_ticks": width, "episodes": rows,
            "totals": {mode: [sum(r["smelt_units"][i] for r in rows if r["strategy"] == enabled)
                              for i in range(len(rows[0]["smelt_units"]))]
                       for mode, enabled in (("off", False), ("on", True))}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--window-ticks", type=int, default=36000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(analyze(args.study, args.window_ticks), indent=2) + "\n")


if __name__ == "__main__":
    main()
