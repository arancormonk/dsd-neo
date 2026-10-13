#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "tetra"))
from verify_field_capture import ValidationError, validate_capture  # noqa: E402


class VerifyTetraFieldCaptureTest(unittest.TestCase):
    def make_bundle(self, directory: Path) -> Path:
        data = directory / "field.iq"
        data.write_bytes(bytes(range(32)))
        metadata = {
            "format": "dsd-neo-iq", "version": 2, "sample_format": "cu8",
            "sample_rate_hz": 8, "center_frequency_hz": 390000000,
            "capture_started_utc": "2026-09-20T00:00:00Z",
            "source_backend": "rtl", "source_args": "dev=0",
            "data_file": data.name, "data_bytes": 32,
            "capture_drops": 0, "capture_drop_blocks": 0, "input_ring_drops": 0,
            "contains_retunes": True, "capture_retune_count": 2,
            "events": [
                {"kind": "RETUNE", "byte_offset": 8, "reason": "frequency",
                 "center_frequency_hz": 391000000,
                 "capture_center_frequency_hz": 391384000, "sample_rate_hz": 8},
                {"kind": "MUTE", "byte_offset": 8, "reason": "retune_mute", "duration_bytes": 4},
                {"kind": "RESET", "byte_offset": 8, "reason": "frequency",
                 "center_frequency_hz": 391000000,
                 "capture_center_frequency_hz": 391384000, "sample_rate_hz": 8},
                {"kind": "RETUNE", "byte_offset": 24, "reason": "frequency",
                 "center_frequency_hz": 390000000,
                 "capture_center_frequency_hz": 390384000, "sample_rate_hz": 8},
                {"kind": "RESET", "byte_offset": 24, "reason": "frequency",
                 "center_frequency_hz": 390000000,
                 "capture_center_frequency_hz": 390384000, "sample_rate_hz": 8},
            ],
        }
        path = directory / "field.iq.json"
        path.write_text(json.dumps(metadata), encoding="utf-8")
        return path

    def test_acceptance_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            metadata = self.make_bundle(directory)
            log = directory / "decode.log"
            log.write_text("[TETRA BSCH]\n[TETRA CHANNEL-ALLOCATION]\n"
                           "[TETRA TCH/FS]\n[TETRA BSCH]\n", encoding="utf-8")
            provenance = directory / "PROVENANCE.md"
            provenance.write_text(
                "Capture UTC: 2026-09-20T00:00:00Z\n"
                "Location: test bench\nReceiver: RTL-SDR\nAntenna: dummy load\n"
                "Operator/source: automated test\nControl channel Hz: 390000000\n"
                "Traffic channel Hz: 391000000\n", encoding="utf-8")
            report = validate_capture(metadata, cc_hz=390000000, vc_hz=391000000,
                                      decode_log=log, provenance=provenance, acceptance=True)
            self.assertEqual(report["status"], "accepted")
            self.assertEqual(report["retune_count"], 2)
            self.assertEqual(len(report["decode_log_milestones"]), 4)
            self.assertEqual(len(report["hashes"]["data_sha256"]), 64)

    def test_rejects_missing_return(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            path = self.make_bundle(directory)
            metadata = json.loads(path.read_text(encoding="utf-8"))
            metadata["events"][3]["center_frequency_hz"] = 392000000
            metadata["events"][4]["center_frequency_hz"] = 392000000
            path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(ValidationError, "does not return"):
                validate_capture(path)

    def test_rejects_drops_and_out_of_order_log(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            path = self.make_bundle(directory)
            metadata = json.loads(path.read_text(encoding="utf-8"))
            metadata["input_ring_drops"] = 1
            path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(ValidationError, "input_ring_drops"):
                validate_capture(path)
            metadata["input_ring_drops"] = 0
            path.write_text(json.dumps(metadata), encoding="utf-8")
            log = directory / "decode.log"
            log.write_text("[TETRA BSCH]\n[TETRA TCH/FS]\n"
                           "[TETRA CHANNEL-ALLOCATION]\n[TETRA BSCH]\n", encoding="utf-8")
            provenance = directory / "PROVENANCE.md"
            provenance.write_text(
                "Capture UTC: 2026-09-20T00:00:00Z\n"
                "Location: test bench\nReceiver: RTL-SDR\nAntenna: dummy load\n"
                "Operator/source: automated test\nControl channel Hz: 390000000\n"
                "Traffic channel Hz: 391000000\n", encoding="utf-8")
            with self.assertRaisesRegex(ValidationError, "TCH/FS"):
                validate_capture(path, decode_log=log, provenance=provenance, acceptance=True)


if __name__ == "__main__":
    unittest.main()
