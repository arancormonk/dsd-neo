#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for tools/noise_squelch_model.py's report (numpy and scipy, as the model needs them).

The report is built from canned per-plan results, so these run in seconds and never run the model itself.
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

import noise_squelch_model as model  # noqa: E402


def no_band_result(width: int, fs: int) -> dict:
    """What run_plan() returns for a plan whose band does not fit."""
    return {
        "key": f"{fs}:hb15:k:p5:{width}",
        "fs": fs,
        "width": width,
        "deviation": 1000.0,
        "candidates": {},
        "edge_hz": width / 2.0 + 200.0,
        "why": "less than 1200 Hz fits between 3800 Hz and the channel edge less 800 Hz",
    }


class NoiseSquelchModelReport(unittest.TestCase):
    def test_a_run_of_fallback_plans_only_reports_them(self):
        """An --only selection of plans with no band (every 8 kHz plan, say) measures nothing; the report still lists
        them as auto and writes both files."""
        results = [no_band_result(8000, 48000), no_band_result(8000, 24000)]
        members = {r["key"]: [f"NFM 8000 @ rtl-{r['fs'] // 1000}k"] for r in results}
        with tempfile.TemporaryDirectory() as out:
            text = model.report(results, members, Path(out), time.time(), "quick")
            self.assertIn("Plans with a band: 0 of 2.", text)
            self.assertIn("No band (noise runs as auto): 8000 Hz at 24000 Hz, 8000 Hz at 48000 Hz.", text)
            self.assertIn("| 8000 | 48000 | none (edge 4200) |", text)
            saved = json.loads((Path(out) / "noise_squelch_results.json").read_text())
            self.assertEqual(len(saved["results"]), 2)
            self.assertEqual(saved["members"], members)


if __name__ == "__main__":
    unittest.main()
