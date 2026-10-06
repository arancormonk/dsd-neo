#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for tools/pcm_noise_squelch_model.py's report (numpy and scipy, as the model needs them).

The report is built from canned per-chain results, so these run in seconds and never run the model itself.
"""

import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tools"))

import pcm_noise_squelch_model as model  # noqa: E402


def no_room_result(native_hz: int) -> dict:
    """What run_chain() returns for a source whose native rate leaves no band above voice."""
    return {
        "chain": f"tap-12k5-none@{native_hz}",
        "native_hz": native_hz,
        "monitor_hz": 48000,
        "expect": "band",
        "band": None,
        "why": "less than 1200 Hz fits between 3800 Hz and 0.45 of the native rate",
        "candidates": {},
    }


class PcmNoiseSquelchModelReport(unittest.TestCase):
    def test_a_run_of_sources_without_room_only_reports_them(self):
        """An --only selection of 8 and 9.6 kHz sources measures nothing; the report still lists them as having no
        room above voice and writes both files."""
        results = [no_room_result(8000), no_room_result(9600)]
        with tempfile.TemporaryDirectory() as out:
            text = model.report(results, Path(out), time.time(), "quick")
            self.assertIn("Sources with a band: 0 of 2.", text)
            self.assertIn("No room above voice: tap-12k5-none@8000, tap-12k5-none@9600.", text)
            self.assertIn("| tap-12k5-none@8000 | 8000 | 48000 | none |", text)
            saved = json.loads((Path(out) / "pcm_noise_squelch_results.json").read_text())
            self.assertEqual(len(saved["results"]), 2)

    def test_the_band_rule_follows_the_native_rate(self):
        """The band stops at 0.45 of the native rate (or the chosen top), and needs 1200 Hz above 3800 Hz."""
        self.assertIsNone(model.band_edges(8000, None))
        self.assertIsNone(model.band_edges(9600, None))
        lo, hi = model.band_edges(12000, None)
        self.assertEqual((lo, hi), (3800.0, 5400.0))
        self.assertEqual(model.band_edges(48000, 8000.0), (3800.0, 8000.0))
        self.assertEqual(model.band_edges(16000, 8000.0), (3800.0, 7200.0))


if __name__ == "__main__":
    unittest.main()
