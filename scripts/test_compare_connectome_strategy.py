#!/usr/bin/env python3
"""Checks for paired summaries and invalid evidence."""
import unittest
import struct
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
from analyze_connectome_windows import analyze, smelt_windows
from compare_connectome_strategy import METRICS, summarize, validate


def row(seed=2037, strategy=False):
    return {**dict.fromkeys(METRICS, 0), "schema": 1, "seed": seed, "ticks": 300,
            "strategy": strategy, "initial_rng": 42, "chain_verified": True,
            "event_capacity_ticks": 0, "strategy_changes": int(strategy)}


class ComparisonTests(unittest.TestCase):
    def test_paired_differences(self):
        off, on = row(), row(strategy=True)
        off["delivered_units"], on["delivered_units"] = 3, 8
        result = summarize([on, off], [2037])
        self.assertEqual(result["totals"]["delivered_units"]["median_delta"], 5)
        self.assertEqual(result["totals"]["delivered_units"]["higher"], 1)

    def test_missing_duplicate_or_different_initial_world(self):
        for rows in ([row()], [row(), row(), row(strategy=True)],
                     [row(), {**row(strategy=True), "initial_rng": 43}]):
            with self.assertRaises(ValueError):
                summarize(rows, [2037])

    def test_rejects_unverified_wrong_mode_and_overflow(self):
        for change in ({"chain_verified": False}, {"strategy": True},
                       {"event_capacity_ticks": 1}, {"delivered_units": float("nan")}):
            with self.assertRaises(ValueError):
                validate({**row(), **change}, 2037, 300, False)

    def test_zero_outcomes_are_valid(self):
        validate(row(), 2037, 300, False)


def record(tick, kind=1, size=80):
    header = bytearray(184)
    struct.pack_into('<Q', header, 0, tick)
    header[16] = kind
    return bytes(header) + struct.pack('<H', size) + bytes(size)


class WindowTests(unittest.TestCase):
    def test_boundaries_and_genesis(self):
        data = b''.join(record(t) for t in (0, 1, 36000, 36001, 72000))
        self.assertEqual(smelt_windows(data, 72000, 36000), [2, 2])

    def test_other_events(self):
        self.assertEqual(smelt_windows(record(20, 2), 100, 50), [0, 0])

    def test_truncated_header_payload_and_late_event(self):
        for data in (record(1)[:10], record(1)[:-1], record(101), record(1, size=4)):
            with self.assertRaises(ValueError):
                smelt_windows(data, 100, 50)


class WindowArchiveTests(unittest.TestCase):
    def test_archive_hashes_and_report_total(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            content = record(1) + record(36000) + record(36001)
            name = '2037-off-chain.tar.gz'
            with tarfile.open(root / name, 'w:gz') as archive:
                info = tarfile.TarInfo('chain/station.log')
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
            report = {'episodes': [{'seed': 2037, 'strategy': False,
                                    'chain_verified': True, 'smelt_output_units': 3}]}
            (root / 'comparison.json').write_text(json.dumps(report))
            manifest = {'status': 'complete', 'ticks': 72000, 'source_commit': 'fixture',
                        'artifacts': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in root.iterdir()}}
            (root / 'manifest.json').write_text(json.dumps(manifest))
            self.assertEqual(analyze(root, 36000)['totals'], {'off': [2, 1], 'on': [0, 0]})
            with self.assertRaises(ValueError):
                analyze(root, 0)
            report['episodes'][0]['smelt_output_units'] = 4
            (root / 'comparison.json').write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                analyze(root, 36000)
            manifest['artifacts']['comparison.json'] = hashlib.sha256((root / 'comparison.json').read_bytes()).hexdigest()
            (root / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'production total'):
                analyze(root, 36000)


if __name__ == "__main__":
    unittest.main()
