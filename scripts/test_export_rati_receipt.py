import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from export_rati_receipt import export_receipt, replay_receipt
from receipt_format import seal, verify

ROOT = Path(__file__).resolve().parents[1]


class ReceiptTests(unittest.TestCase):
    def test_shared_vector(self):
        receipt = json.loads((ROOT / "contracts/rati-receipt/v1/example.json").read_text())
        self.assertTrue(verify(receipt))
        self.assertEqual(seal(receipt), receipt)
        receipt["facts"]["content"] += " "
        self.assertFalse(verify(receipt))

    def test_native_report_preserves_all_proof_levels_and_large_ids(self):
        report = {"schema": "signal.rati_mining_receipts.v1",
                  "source": {"verified": True, "chain_log_path": "temporary"},
                  "receipt_count": 2,
                  "receipts": [{"proof_level": "unbound_v0", "event_id": 2**64 - 1,
                                "mining_proven": False},
                               {"proof_level": "station_attested_v1", "mining_proven": False}]}
        with tempfile.TemporaryDirectory() as folder:
            log, binary = Path(folder) / "log", Path(folder) / "verifier"
            log.write_bytes(b"native log fixture")
            binary.write_bytes(b"native verifier fixture")
            def run(command, **kwargs):
                self.assertEqual(Path(command[-1]).read_bytes(), log.read_bytes())
                return subprocess.CompletedProcess(command, 0, json.dumps(report))
            with patch("export_rati_receipt.subprocess.run", side_effect=run):
                receipt = export_receipt(log, binary, "station")
                self.assertTrue(verify(receipt))
                self.assertTrue(replay_receipt(receipt, log, binary, "station"))
                result = json.loads(receipt["result"]["content"])
                self.assertEqual(result["receipts"], report["receipts"])
                forged = copy.deepcopy(receipt)
                forged["outcome"] = "fail"
                self.assertFalse(replay_receipt(seal(forged), log, binary, "station"))
                binary.write_bytes(b"changed verifier")
                self.assertFalse(replay_receipt(receipt, log, binary, "station"))

    def test_native_failure_stops_export(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "input"
            path.write_bytes(b"fixture")
            with patch("export_rati_receipt.subprocess.run",
                       side_effect=subprocess.CalledProcessError(1, "verifier")):
                with self.assertRaises(subprocess.CalledProcessError):
                    export_receipt(path, path, "station")

    def test_unknown_format_and_empty_receipts(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "input"
            path.write_bytes(b"fixture")
            for report, accepted in [({}, False), ({"schema": "signal.rati_mining_receipts.v1",
                "source": {"verified": True}, "receipt_count": 0, "receipts": []}, True)]:
                with patch("export_rati_receipt.subprocess.run", return_value=
                           subprocess.CompletedProcess([], 0, json.dumps(report))):
                    if accepted:
                        self.assertTrue(verify(export_receipt(path, path, "station")))
                    else:
                        with self.assertRaises(ValueError):
                            export_receipt(path, path, "station")


if __name__ == "__main__":
    unittest.main()
