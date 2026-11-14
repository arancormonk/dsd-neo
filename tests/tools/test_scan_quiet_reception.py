#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/scan_quiet_reception.py's width rows and their hand-off to the quiet check's calibration driver (numpy).

- Row keys name widths exactly (8333 -> 8k333) and keep the shipped calibration's keys for the transmitter it measured.
- Every NFM width has both transmitters' tone rows and voice rows; every AM width an on-channel and an edge row.
- The multitone's audio S/N reads its in-band S/N on a known mixture and fails on noise alone.
- driver_modes.tsv carries every analog row with a threshold, each tagged by its width, and each digital mode's kept row.

These run in seconds and never start a decoder.
"""

import math
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest

try:
    import numpy as np
except ImportError:
    # The tool needs numpy; without it this is a skip CTest reports (SKIP_RETURN_CODE 77), not a pass.
    print(f"SKIP: tools/scan_quiet_reception.py needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tools"))

import scan_quiet_reception as rx  # noqa: E402


class WidthRows(unittest.TestCase):
    def test_width_keys_are_exact(self):
        cases = {5000: "5k", 8000: "8k", 8333: "8k333", 10000: "10k", 11250: "11k25", 12000: "12k", 12500: "12k5"}
        for width, key in cases.items():
            self.assertEqual(rx.width_key(width), key, width)

    def test_every_width_has_its_rows(self):
        rows = rx.analog_rows()
        keys = [row.key for row in rows]
        self.assertEqual(len(keys), len(set(keys)), "row keys repeat")
        for width in rx.NFM_WIDTHS_HZ:
            at = [row for row in rows if row.width_hz == width and row.mode.startswith("nfm_")]
            self.assertEqual(len(at), 4, width)
            self.assertEqual(sum(row.voice for row in at), 2, width)
            self.assertEqual(
                {row.mode for row in at}, {f"nfm_{rx.width_key(width)}", f"nfm_{rx.width_key(width)}_voice"}
            )
        for width in rx.AM_WIDTHS_HZ:
            at = [row.key for row in rows if row.width_hz == width and row.mode.startswith("am_")]
            self.assertEqual(sorted(at), [f"am_{rx.width_key(width)}", f"am_{rx.width_key(width)}_edge"], width)
        self.assertIn(10000, rx.NFM_WIDTHS_HZ)
        self.assertIn(12000, rx.NFM_WIDTHS_HZ)
        self.assertIn(15000, rx.NFM_WIDTHS_HZ)
        self.assertIn(8333, rx.AM_WIDTHS_HZ)

    def test_shipped_rows_keep_their_keys(self):
        """The shipped calibration's source rows (its sources.tsv) keep their keys, and with them their sources."""
        keys = {row.key: row for row in rx.analog_rows()}
        shipped = {
            "nfm_8k": rx.NFM_NARROW_TONE_HZ,
            "nfm_11k25_wide": rx.NFM_WIDE_TONE_HZ,
            "nfm_12k5": rx.NFM_NARROW_TONE_HZ,
            "nfm_16k_wide": rx.NFM_WIDE_TONE_HZ,
            "nfm_20k_wide": rx.NFM_WIDE_TONE_HZ,
            "nfm_25k": rx.NFM_WIDE_TONE_HZ,
        }
        for key, tone in shipped.items():
            self.assertIn(key, keys)
            self.assertIn(f"{tone:.0f} Hz deviation", keys[key].build(None).note, key)
        for width in (5000, 6000, 8000, 10000, 15000, 20000):
            self.assertIn(f"am_{width // 1000}k", keys)
        self.assertIn("nfm_8k_wide", keys)
        self.assertIn("nfm_25k_narrow", keys)

    def test_am_edge_row_sits_at_the_edge(self):
        row = next(row for row in rx.analog_rows() if row.key == "am_8k333_edge")
        note = row.build(None).note
        self.assertIn(f"carrier {8333 / 2 - rx.AM_EDGE_GAP_HZ:+.1f} Hz off the channel", note)


class VoiceScore(unittest.TestCase):
    RATE = 48000

    def audio(self, snr_db, seconds=4.5, seed=1):
        """The multitone plus white noise whose power inside 300-3000 Hz is snr_db under the multitone's."""
        message = np.tile(rx.voice_message(), math.ceil(seconds))[: round(seconds * self.RATE)]
        band = (rx.VOICE_HIGH_HZ - rx.VOICE_LOW_HZ + 1) / (self.RATE / 2)
        sigma = math.sqrt(np.mean(message**2) / band / 10 ** (snr_db / 10))
        return message + np.random.default_rng(seed).normal(0.0, sigma, len(message))

    def test_tones_are_distinct_and_span_the_band(self):
        tones = rx.voice_tones()
        self.assertEqual(len(tones), rx.VOICE_TONE_COUNT)
        self.assertEqual((tones[0], tones[-1]), (rx.VOICE_LOW_HZ, rx.VOICE_HIGH_HZ))
        self.assertLessEqual(float(np.max(np.abs(rx.voice_message()))), 1.0 + 1e-12)

    def test_reads_the_in_band_snr(self):
        for snr in (6.0, 12.0, 20.0):
            self.assertAlmostEqual(rx.voice_snr_db(self.audio(snr), self.RATE), snr, delta=0.5)

    def test_noise_alone_fails(self):
        noise = np.random.default_rng(2).normal(0.0, 0.1, round(4.5 * self.RATE))
        self.assertLess(rx.voice_snr_db(noise, self.RATE), rx.VOICE_SNR_TARGET_DB - 20.0)

    def test_too_short_reads_the_floor(self):
        short = self.audio(20.0, seconds=rx.VOICE_SETTLE_S + 0.9)
        self.assertEqual(rx.voice_snr_db(short, self.RATE), rx.VOICE_FLOOR_DB)


class DriverModes(unittest.TestCase):
    def test_writes_every_analog_row_and_each_kept_digital_row(self):
        def result(key, mode, width, threshold, analog):
            return rx.Result(key, mode, key, width, rx.CLEAN, "", threshold_db=threshold, analog_row=analog)

        results = [
            result("p25p1", "p25p1", 12500, 8.0, False),
            result("p25p1_regen", "p25p1", 12500, 8.5, False),
            result("nfm_8k", "nfm_8k", 8000, 10.5, True),
            result("nfm_8k_wide", "nfm_8k", 8000, 7.0, True),
            result("nfm_11k25_voice_wide", "nfm_11k25_voice", 11250, 8.5, True),
            result("am_8k333_edge", "am_8k333_edge", 8333, 20.0, True),
            result("am_10k", "am_10k", 10000, None, True),
        ]
        with tempfile.TemporaryDirectory() as out:
            rx.ARGS = types.SimpleNamespace(out=out)
            rx.write_driver_modes(results, rx.kept_thresholds(results))
            lines = Path(out, "driver_modes.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "name\ttag\trow\treception_db\twidth_hz\tclass\tanalog")
        self.assertEqual(
            lines[1:],
            [
                "p25p1\tp25p1\tp25p1\t8.0\t12500\t12k5\t0",
                "nfm_8k\tnfm_8k\tnfm_8k\t10.5\t8000\tanalog\t1",
                "nfm_8k_wide\tnfm_8k\tnfm_8k_wide\t7.0\t8000\tanalog\t1",
                "nfm_11k25_voice_wide\tnfm_11k25\tnfm_11k25_voice_wide\t8.5\t11250\tanalog\t1",
                "am_8k333_edge\tam_8k333\tam_8k333_edge\t20.0\t8333\tanalog\t1",
            ],
        )


if __name__ == "__main__":
    unittest.main()
