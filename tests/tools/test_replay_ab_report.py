#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stdlib-only regressions for tools/replay_ab.sh --metric analog and tools/replay_ab_report.py.

The analog metrics come from the analog replay host's "ANALOG METRIC:" and "ANALOG PROBE:" lines. What has to hold
is the same as for the digital metrics: every build is compared with the baseline inside one repeat, never across
repeats, and a build compared with itself reports no difference.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
REPORT = ROOT / "tools" / "replay_ab_report.py"
REPLAY_AB = ROOT / "tools" / "replay_ab.sh"

COLUMNS = [
    "variant", "case", "rep", "errs", "voice", "sync",
    "tone_snr_db", "inband_db", "clip", "audible_ms", "first_audible_ms", "rms_dbfs",
    "probe_hz", "probe_dbfs", "probe_dbc", "tone", "tone_lock_ms",
]


def analog_row(build, rep, snr, inband="30.00", audible="1500.00", tone="NA", lock="NA", probe="NA",
               probe_hz="NA", probe_dbfs="NA"):
    return [build, "cap.json-fast", str(rep), "0", "0", "0", snr, inband, "0", audible, "0.00", "-36.00",
            probe_hz, probe_dbfs, probe, tone, lock]


def write_script(path, text):
    """An executable script with LF line endings whatever the platform's default."""
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    path.chmod(0o755)


def write_summary(path, rows, columns=COLUMNS):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\t".join(columns) + "\n")
        for row in rows:
            handle.write("\t".join(row) + "\n")


def run(args, **kwargs):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)


def report(summary, *extra):
    return run([sys.executable, str(REPORT), str(summary), *extra])


def metric_line(output, metric, build):
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == metric and fields[1] == build:
            return line
    raise AssertionError(f"no '{metric} {build}' line in:\n{output}")


class ReplayAbReport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="replay-ab-report-")
        self.summary = Path(self.tmp) / "summary.tsv"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_analog_metrics_pair_within_each_repeat(self):
        # The repeats differ from each other by far more than the builds do, and the rows come in rotated order,
        # so only pairing by repeat recovers the +1.5 dB.
        rows = [
            analog_row("a", 1, "20.00"), analog_row("b", 1, "21.50"),
            analog_row("b", 2, "31.50"), analog_row("a", 2, "30.00"),
            analog_row("a", 3, "10.00"), analog_row("b", 3, "11.50"),
        ]
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("metric: analog", result.stdout)
        line = metric_line(result.stdout, "tone_snr_db", "b")
        self.assertIn("+1.50 +/- 0.00", line)
        self.assertTrue(line.rstrip().endswith("3/3"), line)
        self.assertIn(" - ", metric_line(result.stdout, "tone_snr_db", "a"))

    def test_a_vs_a_control_reports_no_difference(self):
        rows = []
        for rep, snr in enumerate(("25.10", "24.90", "25.40"), start=1):
            rows.append(analog_row("host.main", rep, snr, probe="-60.00", probe_hz="12500.0", probe_dbfs="-96.00"))
            rows.append(analog_row("host.copy", rep, snr, probe="-60.00", probe_hz="12500.0", probe_dbfs="-96.00"))
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "host.main")
        self.assertEqual(result.returncode, 0, result.stdout)
        for metric in ("tone_snr_db", "inband_db", "audible_ms", "rms_dbfs", "probe_dbfs@12500.0",
                       "probe_dbc@12500.0"):
            line = metric_line(result.stdout, metric, "host.copy")
            self.assertIn("+0.00 +/- 0.00", line)
            self.assertTrue(line.rstrip().endswith("0/3"), line)

    def test_tone_label_reports_modal_value_and_agreement(self):
        rows = [
            analog_row("a", 1, "20.00", tone="151.4", lock="310.00"),
            analog_row("b", 1, "20.00", tone="151.4", lock="290.00"),
            analog_row("a", 2, "20.00", tone="151.4", lock="320.00"),
            analog_row("b", 2, "20.00", tone="NA", lock="NA"),
            analog_row("a", 3, "20.00", tone="151.4", lock="300.00"),
            analog_row("b", 3, "20.00", tone="146.2", lock="280.00"),
        ]
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRegex(metric_line(result.stdout, "tone", "a"), r"151\.4\s+3/3")
        self.assertRegex(metric_line(result.stdout, "tone", "b"), r"151\.4\s+1/3")
        # Only repeats both builds measured are paired: 290-310 and 280-300.
        self.assertIn("-20.00 +/- 0.00", metric_line(result.stdout, "tone_lock_ms", "b"))

    def test_unmeasured_analog_columns_are_left_out(self):
        rows = [analog_row("a", 1, "20.00"), analog_row("b", 1, "21.00")]
        write_summary(self.summary, rows)
        result = report(self.summary)
        self.assertEqual(result.returncode, 0, result.stdout)
        for metric in ("tone", "tone_lock_ms", "probe_dbc", "probe_dbfs"):
            self.assertNotRegex(result.stdout, rf"(?m)^{metric}[\s@]")
        self.assertRegex(result.stdout, r"(?m)^tone_snr_db\s")

    def test_probe_levels_pair_only_at_the_same_frequency(self):
        # A variant wrapper that probes 5 kHz first must not be scored against a baseline that probed 12.5 kHz,
        # and a probe on a real capture has a dBFS level even where dBc is NA.
        rows = []
        for rep in (1, 2):
            rows.append(analog_row("main", rep, "20.00", probe_hz="12500.0", probe_dbfs="-90.00"))
            rows.append(analog_row("same", rep, "20.00", probe_hz="12500.0", probe_dbfs="-88.00"))
            rows.append(analog_row("other", rep, "20.00", probe_hz="5000.0", probe_dbfs="-30.00"))
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "main")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("+2.00 +/- 0.00", metric_line(result.stdout, "probe_dbfs@12500.0", "same"))
        other = metric_line(result.stdout, "probe_dbfs@5000.0", "other")
        self.assertRegex(other, r"-30\.00\s+-30\.00\s+0\.00\s+-\s+-$")
        self.assertNotRegex(result.stdout, r"(?m)^probe_dbfs@12500\.0\s+other\s")
        self.assertNotRegex(result.stdout, r"(?m)^probe_dbc")
        self.assertIn("probed different frequencies (5000.0, 12500.0 Hz)", result.stdout)

    def test_digital_summary_still_reports_errors_per_voice_frame(self):
        rows = [
            ["before", "cap.json-fast", "1", "40", "20", "5"],
            ["after", "cap.json-fast", "1", "20", "20", "5"],
            ["before", "cap.json-fast", "2", "60", "30", "5"],
            ["after", "cap.json-fast", "2", "30", "30", "5"],
        ]
        write_summary(self.summary, rows, COLUMNS[:6])
        result = report(self.summary, "--baseline", "before")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("err/voice", result.stdout)
        self.assertIn("-1.00 +/- 0.00", result.stdout)

    def test_analog_request_without_analog_rows_fails(self):
        write_summary(self.summary, [["a", "cap.json-fast", "1", "4", "2", "1"]], COLUMNS[:6])
        result = report(self.summary, "--metric", "analog")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no analog", result.stdout)


FAKE_HOST = """#!/usr/bin/env bash
# Stands in for dsd-neo_test_analog_replay: its output depends only on its own name and on
# a per-variant flag, the way a wrapper script passes one to the real host.
snr=25.00
case "$*" in *--fake-boost*) snr=26.00 ;; esac
echo "NOTICE: Total audio errors: 0"
echo "ANALOG METRIC: rate_hz=48000 total_ms=1500.00 captured_ms=1500.00 audible_ms=1480.00" \\
  "first_audible_ms=20.00 rms_dbfs=-36.00 peak_dbfs=-31.00 clip=0 inband_db=30.50 tone_hz=1000.00" \\
  "tone_dbfs=-36.00 tone_snr_db=$snr"
echo "ANALOG PROBE: hz=12500.0 dbfs=-100.00 dbc=-64.00"
echo "ANALOG PROBE: hz=5000.0 dbfs=-80.00 dbc=-44.00"
"""


# Windows has a timeout.exe that is not coreutils timeout, and bash there is WSL or Git bash with its own path rules.
@unittest.skipIf(os.name == "nt", "replay_ab.sh is a POSIX shell tool")
@unittest.skipUnless(shutil.which("bash") and shutil.which("timeout"), "replay_ab.sh needs bash and timeout")
class ReplayAbAnalogMetric(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="replay-ab-analog-"))
        self.capture = self.tmp / "capture.iq.json"
        self.capture.write_text("{}\n", encoding="utf-8")
        self.host = self.tmp / "host.main"
        write_script(self.host, FAKE_HOST)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def wrapper(self, name, flags):
        path = self.tmp / name
        write_script(path, f'#!/usr/bin/env bash\nexec "{self.host}" {flags} "$@"\n')
        return path

    def replay_ab(self, *args):
        return run(["bash", str(REPLAY_AB), "--capture", str(self.capture), "--mode", "-fA", "--rate", "fast", *args],
                   cwd=str(ROOT))

    def test_analog_columns_come_from_the_host_lines(self):
        boosted = self.wrapper("host.boost", "--fake-boost")
        out = self.tmp / "out"
        result = self.replay_ab("--metric", "analog", "--reps", "2", "--out", str(out), str(self.host), str(boosted))
        self.assertEqual(result.returncode, 0, result.stdout)
        lines = (out / "summary.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0].split("\t"), COLUMNS)
        rows = [dict(zip(COLUMNS, line.split("\t"))) for line in lines[1:]]
        self.assertEqual(len(rows), 4)
        by_variant = {(row["variant"], row["rep"]): row for row in rows}
        self.assertEqual(by_variant[("host.main", "1")]["tone_snr_db"], "25.00")
        self.assertEqual(by_variant[("host.boost", "2")]["tone_snr_db"], "26.00")
        for row in rows:
            self.assertEqual(row["inband_db"], "30.50")
            self.assertEqual(row["clip"], "0")
            self.assertEqual(row["audible_ms"], "1480.00")
            self.assertEqual(row["first_audible_ms"], "20.00")
            self.assertEqual(row["rms_dbfs"], "-36.00")
            # The first probe line, with its frequency, not the last one.
            self.assertEqual(row["probe_hz"], "12500.0")
            self.assertEqual(row["probe_dbfs"], "-100.00")
            self.assertEqual(row["probe_dbc"], "-64.00")
            self.assertEqual(row["tone"], "NA")
            self.assertEqual(row["tone_lock_ms"], "NA")

        result = report(out / "summary.tsv", "--baseline", "host.main")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("+1.00 +/- 0.00", metric_line(result.stdout, "tone_snr_db", "host.boost"))

    def test_builds_with_the_same_name_are_rejected(self):
        other = self.tmp / "other"
        other.mkdir()
        twin = other / "host.main"
        shutil.copy(self.host, twin)
        twin.chmod(0o755)
        result = self.replay_ab("--metric", "analog", "--reps", "1", "--out", str(self.tmp / "out"), str(self.host),
                                str(twin))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same name", result.stdout)

    def test_unknown_metric_is_rejected(self):
        result = self.replay_ab("--metric", "loudness", "--out", str(self.tmp / "out"), str(self.host),
                                str(self.wrapper("host.copy", "")))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--metric", result.stdout)


if __name__ == "__main__":
    unittest.main()
