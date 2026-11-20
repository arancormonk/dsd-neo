#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tests/analog_audio_out_check.py reads the analog replay host's lines whatever their line ends.

The host prints with "\\n", which Windows writes as CRLF. The ANALOG AUDIO OUT pattern ends at the line's end, so the
checker must read a CRLF end as one newline or it finds no line on Windows. Stdlib only; runs no host.
"""

import importlib
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tests"))

check = importlib.import_module("analog_audio_out_check")

METRIC = "ANALOG METRIC: captured_ms=4980.0 rms_dbfs=-18.42 tone_snr_db=31.20 "
AUDIO_OUT = "ANALOG AUDIO OUT: rate_hz=48000 samples=239040"


def host_output(newline):
    """The host's stderr and stdout, each line ended by @p newline, as bytes."""
    stderr = newline.join(("ANALOG AUDIO OK", METRIC, AUDIO_OUT, "")).encode("utf-8")
    stdout = newline.join(("done", "")).encode("utf-8")
    return stdout, stderr


class LineEnds(unittest.TestCase):
    def check_reads(self, newline):
        stdout, stderr = host_output(newline)
        text = check.host_text(stdout, stderr)
        self.assertNotIn("\r\n", text)
        metric, out = check.metric_lines(text)
        self.assertIsNotNone(metric)
        self.assertIsNotNone(out)
        self.assertEqual(metric.group(1), "-18.42")
        self.assertEqual((out.group(1), out.group(2)), ("48000", "239040"))

    def test_reads_lf_lines(self):
        self.check_reads("\n")

    def test_reads_crlf_lines(self):
        self.check_reads("\r\n")

    def test_stderr_comes_first(self):
        text = check.host_text(b"out\r\n", b"err\r\n")
        self.assertEqual(text, "err\nout\n")


if __name__ == "__main__":
    unittest.main()
