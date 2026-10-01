#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stdlib-only regressions for tools/replay_ab.sh --metric analog and tools/replay_ab_report.py.

The analog metrics come from the analog replay host's "ANALOG METRIC:" and "ANALOG PROBE:" lines. What has to hold
is the same as for the digital metrics: every build is compared with the baseline inside one repeat, never across
repeats, and a build compared with itself reports no difference.

Since replay is deterministic, replay_ab.sh replays each repeat as its own realization of the capture: a copy of the
sidecar with every event moved by a fraction of a symbol, the same copy for every build in that repeat.
ReplayAbRealizations holds the copies to that.

ReplayAbReport runs the report on canned summaries and needs only Python; ReplayAbAnalogMetric, ReplayAbDigitalMetric
and ReplayAbRealizations also run replay_ab.sh. Name a class on the command line to run just that one, as
tests/CMakeLists.txt does.
"""

import json

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
# The bash tests/CMakeLists.txt found when it registered the replay_ab.sh half.
BASH = os.environ.get("DSD_TEST_BASH", "bash")

COLUMNS = [
    "variant", "case", "rep", "errs", "voice", "sync",
    "tone_snr_db", "inband_db", "clip", "audible_ms", "first_audible_ms", "rms_dbfs",
    "probe_hz", "probe_dbfs", "probe_dbc", "tone", "tone_lock_ms", "tone_lock_pct", "rc", "off_path",
]
# What replay_ab.sh writes: COLUMNS and, last, the input samples it shifted the capture's events by in that repeat.
# The canned summaries below use COLUMNS, the layout of a summary written before the shift was recorded.
SUMMARY_COLUMNS = COLUMNS + ["shift"]


def analog_row(build, rep, snr, inband="30.00", audible="1500.00", tone="NA", lock="NA", probe="NA",
               probe_hz="NA", probe_dbfs="NA", rc="0", off_path="0", lock_pct="NA"):
    return [build, "cap.json-fast", str(rep), "0", "0", "0", snr, inband, "0", audible, "0.00", "-36.00",
            probe_hz, probe_dbfs, probe, tone, lock, lock_pct, rc, off_path]


def missing_row(build, rep):
    """A repeat whose host printed no ANALOG METRIC line (here a timeout): every analog column is NA."""
    return [build, "cap.json-fast", str(rep), "NA", "0", "0"] + ["NA"] * (len(COLUMNS) - 8) + ["124", "0"]


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
            analog_row("a", 1, "20.00", tone="151.4", lock="310.00", lock_pct="90.00"),
            analog_row("b", 1, "20.00", tone="151.4", lock="290.00", lock_pct="92.50"),
            analog_row("a", 2, "20.00", tone="151.4", lock="320.00", lock_pct="88.00"),
            analog_row("b", 2, "20.00", tone="NA", lock="NA", lock_pct="0.00"),
            analog_row("a", 3, "20.00", tone="151.4", lock="300.00", lock_pct="91.00"),
            analog_row("b", 3, "20.00", tone="146.2", lock="280.00", lock_pct="93.50"),
        ]
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRegex(metric_line(result.stdout, "tone", "a"), r"151\.4\s+3/3")
        self.assertRegex(metric_line(result.stdout, "tone", "b"), r"151\.4\s+1/3")
        # Only repeats both builds measured are paired: 290-310 and 280-300.
        self.assertIn("-20.00 +/- 0.00", metric_line(result.stdout, "tone_lock_ms", "b"))
        # The lock percentage is measured even in a repeat that never locked (0.00, not NA), so every repeat pairs:
        # +2.50, -88.00 and +2.50.
        lock_pct = metric_line(result.stdout, "tone_lock_pct", "b")
        self.assertRegex(lock_pct, r"\s3/3\s")
        self.assertIn("-27.67 +/- ", lock_pct)
        self.assertTrue(lock_pct.rstrip().endswith("3/3"), lock_pct)

    def test_unmeasured_analog_columns_are_left_out(self):
        rows = [analog_row("a", 1, "20.00"), analog_row("b", 1, "21.00")]
        write_summary(self.summary, rows)
        result = report(self.summary)
        self.assertEqual(result.returncode, 0, result.stdout)
        for metric in ("tone", "tone_lock_ms", "tone_lock_pct", "probe_dbc", "probe_dbfs"):
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

    def test_build_without_any_analog_data_is_shown_and_fails_the_report(self):
        # A variant that crashed in every repeat must not vanish from the report and leave the baseline's rows
        # looking like a clean comparison.
        rows = []
        for rep, snr in enumerate(("25.10", "24.90", "25.40"), start=1):
            rows.append(analog_row("host.main", rep, snr))
            rows.append(missing_row("host.branch", rep))
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "host.main")
        self.assertEqual(result.returncode, 1, result.stdout)
        for metric in ("tone_snr_db", "inband_db", "audible_ms", "rms_dbfs"):
            self.assertRegex(metric_line(result.stdout, metric, "host.branch"), r"\s0/3\s+NA\s+NA\s+NA\s+-\s+-$")
        self.assertIn("warning: host.branch produced no usable analog measurement in any of 3 repeats",
                      result.stdout)
        self.assertIn("warning: host.branch: 3 of 3 repeats left out of the report: 3 exited non-zero (status 124)",
                      result.stdout)
        self.assertNotIn("warning: host.main", result.stdout)

    def test_partial_coverage_is_counted_and_paired_on_common_repeats(self):
        rows = [
            analog_row("a", 1, "20.00"), analog_row("b", 1, "21.00"),
            analog_row("a", 2, "30.00"), missing_row("b", 2),
            analog_row("a", 3, "10.00"), analog_row("b", 3, "11.00"),
        ]
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        line = metric_line(result.stdout, "tone_snr_db", "b")
        self.assertRegex(line, r"\s2/3\s")
        self.assertIn("+1.00 +/- 0.00", line)
        self.assertTrue(line.rstrip().endswith("2/2"), line)
        self.assertRegex(metric_line(result.stdout, "tone_snr_db", "a"), r"\s3/3\s")
        self.assertIn("note: b produced usable analog measurements in only 2 of 3 repeats", result.stdout)

    def test_repeats_that_exited_non_zero_or_left_the_monitor_path_are_not_paired(self):
        # The host prints its metrics from the stop hook, so a run that crashed afterwards, or one whose front end
        # delivered CQPSK symbols, still has numbers in every column. Paired as normal, b would read +39.50 here.
        rows = [
            analog_row("a", 1, "20.00"), analog_row("b", 1, "21.50"),
            analog_row("a", 2, "30.00"), analog_row("b", 2, "99.00", rc="139"),
            analog_row("a", 3, "10.00"), analog_row("b", 3, "70.00", off_path="1"),
        ]
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        line = metric_line(result.stdout, "tone_snr_db", "b")
        self.assertRegex(line, r"\s1/3\s+21\.50\s")
        # One usable pair is a difference but no interval.
        self.assertIn("+1.50 +/- n/a", line)
        self.assertTrue(line.rstrip().endswith("1/1"), line)
        self.assertIn("warning: b: 2 of 3 repeats left out of the report: 1 exited non-zero (status 139); 1 ran off "
                      "the monitor path", result.stdout)
        self.assertIn("note: b produced usable analog measurements in only 1 of 3 repeats", result.stdout)
        self.assertNotIn("warning: a:", result.stdout)

    def test_build_flagged_in_every_repeat_fails_the_report(self):
        rows = []
        for rep in (1, 2):
            rows.append(analog_row("host.main", rep, "25.00"))
            rows.append(analog_row("host.branch", rep, "25.00", rc="139"))
        write_summary(self.summary, rows)
        result = report(self.summary, "--baseline", "host.main")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertRegex(metric_line(result.stdout, "tone_snr_db", "host.branch"), r"\s0/2\s+NA\s")
        self.assertIn("warning: host.branch produced no usable analog measurement in any of 2 repeats",
                      result.stdout)

    def test_summary_without_run_status_columns_still_pairs(self):
        # summary.tsv files written before replay_ab.sh recorded rc and off_path.
        rows = [analog_row("a", 1, "20.00")[:-2], analog_row("b", 1, "21.00")[:-2]]
        write_summary(self.summary, rows, COLUMNS[:-2])
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("+1.00 +/- n/a", metric_line(result.stdout, "tone_snr_db", "b"))
        self.assertNotIn("left out of the report", result.stdout)

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

    def test_single_pair_has_no_interval(self):
        # One repeat cannot estimate the repeat-to-repeat spread, so neither report may print a zero-width interval
        # for it (two identical differences, as in the A-vs-A control, do give a real +/- 0.00).
        write_summary(self.summary, [analog_row("a", 1, "20.00"), analog_row("b", 1, "21.50")])
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("+1.50 +/- n/a", metric_line(result.stdout, "tone_snr_db", "b"))
        self.assertIn("needs at least two paired repeats", " ".join(result.stdout.split()))

        digital = [["before", "cap.json-fast", "1", "40", "20", "5"], ["after", "cap.json-fast", "1", "20", "20", "5"]]
        write_summary(self.summary, digital, COLUMNS[:6])
        result = report(self.summary, "--baseline", "before")
        self.assertEqual(result.returncode, 0, result.stdout)
        after = [line for line in result.stdout.splitlines() if line.split()[:1] == ["after"]]
        self.assertEqual(len(after), 1, result.stdout)
        self.assertIn("-1.00 +/- n/a", after[0])
        self.assertIn("needs at least two paired repeats", " ".join(result.stdout.split()))

    def test_report_names_the_realizations_the_repeats_replayed(self):
        rows = []
        for rep, shift in ((1, "0"), (2, "214"), (3, "428")):
            rows.append(["before", "cap.json-fast", str(rep), "40", "20", "5", shift])
            rows.append(["after", "cap.json-fast", str(rep), "20", "20", "5", shift])
        write_summary(self.summary, rows, COLUMNS[:6] + ["shift"])
        result = report(self.summary, "--baseline", "before")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("realizations: 3 (events shifted 0 to 428 input samples)", result.stdout)
        self.assertIn("interval spans the realizations", " ".join(result.stdout.split()))
        self.assertNotIn("same realization of the capture", result.stdout)

        analog = []
        for rep, shift in ((1, "0"), (2, "214")):
            analog.append(analog_row("a", rep, "20.00") + [shift])
            analog.append(analog_row("b", rep, "21.00") + [shift])
        write_summary(self.summary, analog, SUMMARY_COLUMNS)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("realizations: 2 (events shifted 0 to 214 input samples)", result.stdout)

    def test_one_realization_in_every_repeat_is_called_out(self):
        # --no-realizations, or a capture with no events: the repeats are identical replays, so the difference
        # reads +/- 0.00 whatever the capture would say about it, and the report must not let that pass for a
        # resolved difference.
        rows = []
        for rep in (1, 2, 3):
            rows.append(["before", "cap.json-fast", str(rep), "40", "20", "5", "0"])
            rows.append(["after", "cap.json-fast", str(rep), "20", "20", "5", "0"])
        write_summary(self.summary, rows, COLUMNS[:6] + ["shift"])
        result = report(self.summary, "--baseline", "before")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("-1.00 +/- 0.00", result.stdout)
        self.assertIn("realizations: 1 (the capture as recorded)", result.stdout)
        self.assertIn("note: every repeat replayed the same realization of the capture", result.stdout)
        self.assertNotIn("interval spans the realizations", " ".join(result.stdout.split()))

        analog = [analog_row(build, rep, "20.00") + ["0"] for rep in (1, 2) for build in ("a", "b")]
        write_summary(self.summary, analog, SUMMARY_COLUMNS)
        result = report(self.summary, "--baseline", "a")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("note: every repeat replayed the same realization of the capture", result.stdout)

    def test_summary_without_a_shift_column_names_no_realizations(self):
        rows = [["before", "cap.json-fast", "1", "40", "20", "5"], ["after", "cap.json-fast", "1", "20", "20", "5"],
                ["before", "cap.json-fast", "2", "40", "20", "5"], ["after", "cap.json-fast", "2", "20", "20", "5"]]
        write_summary(self.summary, rows, COLUMNS[:6])
        result = report(self.summary, "--baseline", "before")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("realization", result.stdout)

    def test_analog_request_without_analog_rows_fails(self):
        write_summary(self.summary, [["a", "cap.json-fast", "1", "4", "2", "1"]], COLUMNS[:6])
        result = report(self.summary, "--metric", "analog")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no analog", result.stdout)


FAKE_HOST = """#!/usr/bin/env bash
# Stands in for dsd-neo_test_analog_replay: its output depends only on its own name and on
# a per-variant flag, the way a wrapper script passes one to the real host.
# The received-tone fields follow the contract in tests/engine/analog_replay.c's file comment.
snr=25.00
tone=NA
lock=NA
pct=0.00
case "$*" in *--fake-boost*) snr=26.00 ;; esac
case "$*" in *--fake-tone*) tone=D023N/D047I lock=312.50 pct=87.50 ;; esac
echo "NOTICE: Total audio errors: 0"
echo "ANALOG METRIC: rate_hz=48000 total_ms=1500.00 captured_ms=1500.00 audible_ms=1480.00" \\
  "first_audible_ms=20.00 rms_dbfs=-36.00 peak_dbfs=-31.00 clip=0 inband_db=30.50 tone_hz=1000.00" \\
  "tone_dbfs=-36.00 tone_snr_db=$snr tone=$tone tone_lock_ms=$lock tone_lock_pct=$pct"
echo "ANALOG PROBE: hz=12500.0 dbfs=-100.00 dbc=-64.00"
echo "ANALOG PROBE: hz=5000.0 dbfs=-80.00 dbc=-44.00"
# The host's own warning text (tests/engine/analog_replay.c), which replay_ab.sh matches.
case "$*" in *--fake-off-path*)
  echo "analog replay: warning: the RTL front end delivered 35121 CQPSK symbols instead of monitor samples;" \\
    "total_ms counts them as samples, so it does not measure stream time" >&2 ;;
esac
# Crashes after the stop hook printed its metrics.
case "$*" in *--fake-crash*) exit 139 ;; esac
exit 0
"""


class ReplayAbAnalogMetric(unittest.TestCase):
    """Drives the real replay_ab.sh, so it needs bash and coreutils timeout. tests/CMakeLists.txt runs this class as
    its own test and registers it only where both are found, outside Windows (whose timeout.exe is not coreutils
    timeout, and whose bash is WSL or Git bash with their own path rules)."""

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
        return run([BASH, str(REPLAY_AB), "--capture", str(self.capture), "--mode", "-fA", "--rate", "fast", *args],
                   cwd=str(ROOT))

    def test_analog_columns_come_from_the_host_lines(self):
        boosted = self.wrapper("host.boost", "--fake-boost")
        out = self.tmp / "out"
        result = self.replay_ab("--metric", "analog", "--reps", "2", "--out", str(out), str(self.host), str(boosted))
        self.assertEqual(result.returncode, 0, result.stdout)
        lines = (out / "summary.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0].split("\t"), SUMMARY_COLUMNS)
        rows = [dict(zip(SUMMARY_COLUMNS, line.split("\t"))) for line in lines[1:]]
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
            self.assertEqual(row["tone_lock_pct"], "0.00")
            self.assertEqual(row["rc"], "0")
            self.assertEqual(row["off_path"], "0")
            # The capture has no events, so no repeat is shifted.
            self.assertEqual(row["shift"], "0")

        result = report(out / "summary.tsv", "--baseline", "host.main")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("+1.00 +/- 0.00", metric_line(result.stdout, "tone_snr_db", "host.boost"))

    def test_crashed_and_off_path_runs_are_recorded_and_left_out(self):
        # Both variants print a full ANALOG METRIC line in every repeat; only the exit status and the host's
        # warning say that none of it measures the build.
        crash = self.wrapper("host.crash", "--fake-crash --fake-boost")
        off_path = self.wrapper("host.offpath", "--fake-off-path --fake-boost")
        out = self.tmp / "out"
        result = self.replay_ab("--metric", "analog", "--reps", "2", "--out", str(out), str(self.host), str(crash),
                                str(off_path))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("(exit 139)", result.stdout)
        self.assertIn("(off the monitor path)", result.stdout)
        lines = (out / "summary.tsv").read_text(encoding="utf-8").splitlines()
        rows = {(row["variant"], row["rep"]): row for row in (dict(zip(COLUMNS, line.split("\t"))) for line in lines[1:])}
        for rep in ("1", "2"):
            self.assertEqual((rows[("host.main", rep)]["rc"], rows[("host.main", rep)]["off_path"]), ("0", "0"))
            self.assertEqual((rows[("host.crash", rep)]["rc"], rows[("host.crash", rep)]["off_path"]), ("139", "0"))
            self.assertEqual((rows[("host.offpath", rep)]["rc"], rows[("host.offpath", rep)]["off_path"]),
                             ("0", "1"))
            self.assertEqual(rows[("host.crash", rep)]["tone_snr_db"], "26.00")

        result = report(out / "summary.tsv", "--baseline", "host.main")
        self.assertEqual(result.returncode, 1, result.stdout)
        for build in ("host.crash", "host.offpath"):
            self.assertRegex(metric_line(result.stdout, "tone_snr_db", build), r"\s0/2\s+NA\s")
            self.assertIn(f"warning: {build} produced no usable analog measurement in any of 2 repeats",
                          result.stdout)
        self.assertIn("warning: host.crash: 2 of 2 repeats left out of the report: 2 exited non-zero (status 139)",
                      result.stdout)
        self.assertIn("warning: host.offpath: 2 of 2 repeats left out of the report: 2 ran off the monitor path",
                      result.stdout)

    def test_received_tone_fields_follow_the_host_contract(self):
        # tone= must not be confused with the host's tone_hz= (the expected test tone), and a detector's label and
        # lock time land in their own columns and in the report. A DCS label is both spellings of the code's
        # signal joined by a slash, one token that reaches the column and the report whole.
        toned = self.wrapper("host.tone", "--fake-tone")
        out = self.tmp / "out"
        result = self.replay_ab("--metric", "analog", "--reps", "2", "--out", str(out), str(self.host), str(toned))
        self.assertEqual(result.returncode, 0, result.stdout)
        lines = (out / "summary.tsv").read_text(encoding="utf-8").splitlines()
        rows = {(row["variant"], row["rep"]): row for row in (dict(zip(COLUMNS, line.split("\t"))) for line in lines[1:])}
        self.assertEqual(rows[("host.tone", "1")]["tone"], "D023N/D047I")
        self.assertEqual(rows[("host.tone", "2")]["tone_lock_ms"], "312.50")
        self.assertEqual(rows[("host.main", "1")]["tone"], "NA")
        self.assertEqual(rows[("host.main", "2")]["tone_lock_ms"], "NA")
        self.assertEqual(rows[("host.tone", "1")]["tone_lock_pct"], "87.50")
        self.assertEqual(rows[("host.main", "1")]["tone_lock_pct"], "0.00")

        result = report(out / "summary.tsv", "--baseline", "host.main")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRegex(metric_line(result.stdout, "tone", "host.tone"), r"\sD023N/D047I\s+2/2$")
        self.assertRegex(metric_line(result.stdout, "tone", "host.main"), r"NA\s+0/2")
        self.assertRegex(metric_line(result.stdout, "tone_lock_ms", "host.main"), r"\s0/2\s+NA")
        self.assertIn("+87.50 +/- 0.00", metric_line(result.stdout, "tone_lock_pct", "host.tone"))

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


FAKE_DIGITAL_HOST = """#!/usr/bin/env bash
# Stands in for dsd-neo in a digital replay: the startup notices a ProVoice or EDACS preset prints, then either
# four ProVoice frames (--fake-provoice), one of them conventional ProVoice as a PVCONVENTIONAL build prints it,
# three D-STAR superframes (--fake-dstar), one behind a header decoded from noise, whose callsigns print as raw
# bytes that are not UTF-8, with some of their -Z AMBE lines, or two frames of a protocol that labels voice as "Voice".
echo "NOTICE: Decoding only ProVoice frames."
echo "NOTICE: EDACS Analog Voice Channels are Experimental."
case "$*" in
  *--fake-provoice*)
    echo "12:00:00 Sync: -PV     VOICE"
    echo " IMBE 965140CC8A0BFFB4BB7FC2 err = [0] [0]  7100"
    echo "12:00:00 Sync: +PV     VOICE"
    echo "12:00:00 Sync: -PV_C  TX: 172 RX: 5 ALL CALL  VOICE"
    echo "12:00:00 Sync: -PV     VOICE"
    echo "NOTICE: Total audio errors: 12"
    ;;
  *--fake-dstar*)
    printf '12:00:00 Sync: +DSTAR HEADER  RPT 2: \\xf1\\x9e~\\xa4 RPT 1: \\xc3( DST: CQCQCQ   SRC: \\xff\\xfe\\n'
    echo " AMBE F709018E901180 err = [0] [0] "
    echo " AMBE F7880B21882080 err = [1] [2] "
    echo "12:00:00 Sync: -DSTAR VOICE  "
    echo " AMBE F757D8434C3A80 err = [0] [0] "
    echo "12:00:00 Sync: +DSTAR VOICE  "
    echo "NOTICE: Total audio errors: 9"
    ;;
  *)
    echo "12:00:00 Sync: +NXDN48 RTCH Voice"
    echo "12:00:00 Sync: +NXDN48 RTCH Voice"
    echo "NOTICE: Total audio errors: 4"
    ;;
esac
exit 0
"""


class ReplayAbDigitalMetric(unittest.TestCase):
    """The digital columns replay_ab.sh reads from a real run's log, driven through the real script. Needs bash and
    coreutils timeout, so tests/CMakeLists.txt registers it beside ReplayAbAnalogMetric (issue #588)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="replay-ab-digital-"))
        self.capture = self.tmp / "capture.iq.json"
        self.capture.write_text("{}\n", encoding="utf-8")
        self.host = self.tmp / "host.main"
        write_script(self.host, FAKE_DIGITAL_HOST)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def rows(self, flags, env=None):
        wrapper = self.tmp / "host.variant"
        write_script(wrapper, f'#!/usr/bin/env bash\nexec "{self.host}" {flags} "$@"\n')
        out = self.tmp / "out"
        result = run([BASH, str(REPLAY_AB), "--capture", str(self.capture), "--mode", "-fp", "--rate", "fast",
                      "--reps", "1", "--out", str(out), str(self.host), str(wrapper)], cwd=str(ROOT), env=env)
        self.assertEqual(result.returncode, 0, result.stdout)
        lines = (out / "summary.tsv").read_text(encoding="utf-8").splitlines()
        return {row["variant"]: row for row in (dict(zip(COLUMNS, line.split("\t"))) for line in lines[1:])}

    def test_provoice_voice_frames_are_counted_and_startup_notices_are_not(self):
        # ProVoice prints " VOICE" after its sync, trunked or conventional; the two startup notices that mention
        # "Voice" are not frames.
        rows = self.rows("--fake-provoice")
        self.assertEqual(rows["host.variant"]["voice"], "4")
        self.assertEqual(rows["host.variant"]["errs"], "12")
        self.assertEqual(rows["host.variant"]["sync"], "4")

    def test_dstar_superframes_are_counted_and_ambe_lines_are_not(self):
        # D-STAR prints nothing labelled "Voice": each sync line, voice or header, opens a superframe of 21 AMBE
        # voice frames (issue #599). The -Z AMBE lines are not counted, since DMR and NXDN print them too. The
        # header's raw callsign bytes make a UTF-8 grep treat the log as binary, and without -a it stops printing
        # lines at the first of them, so the frames after it would go uncounted.
        rows = self.rows("--fake-dstar", env={**os.environ, "LC_ALL": "C.UTF-8"})
        self.assertEqual(rows["host.variant"]["voice"], "3")
        self.assertEqual(rows["host.variant"]["errs"], "9")
        self.assertEqual(rows["host.variant"]["sync"], "3")

    def test_voice_lines_of_other_protocols_are_still_counted(self):
        rows = self.rows("")
        self.assertEqual(rows["host.main"]["voice"], "2")
        self.assertEqual(rows["host.variant"]["voice"], "2")
        self.assertEqual(rows["host.main"]["errs"], "4")


FAKE_REPLAY_HOST = """#!/usr/bin/env bash
# Stands in for dsd-neo: says which sidecar it was given to replay, so a test can see what each repeat replayed.
while [ $# -gt 0 ]; do
  if [ "$1" = --iq-replay ]; then
    echo "REPLAYED: $2"
  fi
  shift
done
echo "12:00:00 Sync: +NXDN48 RTCH Voice"
echo "NOTICE: Total audio errors: 1"
exit 0
"""

NO_EVENTS_WARNING = "the capture has no events: every repeat replays the same realization; compare it across captures"


def frequency_event(kind, offset, rate):
    return {"kind": kind, "byte_offset": offset, "center_frequency_hz": 467756250,
            "capture_center_frequency_hz": 467756250, "sample_rate_hz": rate, "reason": "frequency"}


def mute_event(offset, duration):
    return {"kind": "MUTE", "byte_offset": offset, "duration_bytes": duration, "reason": "retune_mute"}


def sidecar(sample_format, rate, data_file, data_bytes, events=None):
    """A sidecar with every field dsd-neo's replay requires, laid out as tests/fixtures/iq writes them."""
    retunes = sum(1 for event in events or () if event["kind"] == "RETUNE")
    meta = {
        "format": "dsd-neo-iq", "version": 1 if events is None else 2, "sample_format": sample_format, "iq_order": "IQ",
        "endianness": "none" if sample_format == "cu8" else "little", "capture_stage": "post_mute_pre_widen",
        "sample_rate_hz": rate, "center_frequency_hz": 467087500, "capture_center_frequency_hz": 467087500, "ppm": 0,
        "tuner_gain_tenth_db": 270, "rtl_dsp_bw_khz": 48, "base_decimation": rate // 48000, "post_downsample": 1,
        "demod_rate_hz": 48000, "offset_tuning_enabled": False, "fs4_shift_enabled": False,
        "combine_rotate_enabled": False, "muted_bytes_excluded": True, "contains_retunes": retunes > 0,
        "capture_retune_count": retunes, "source_backend": "rtl", "source_args": "fixture",
        "capture_started_utc": "2026-01-01T00:00:00Z", "data_file": data_file, "data_bytes": data_bytes,
        "capture_drops": 0, "capture_drop_blocks": 0, "input_ring_drops": 0, "notes": "scan of the caf\u00e9 site",
    }
    if events is not None:
        meta["events"] = events
    return meta


class ReplayAbRealizations(unittest.TestCase):
    """Replay is deterministic, so identical repeats of one capture are one draw of it, and a paired interval over
    them reports a precision the capture does not have. replay_ab.sh therefore replays repeat r from a copy of the
    sidecar whose every event is moved forward by s_r = floor((r-1) * P / reps) + (r-1) input samples, with P one
    2400-baud symbol at the capture's rate, so the shifts spread across a symbol. A shift after the first that lands
    on a whole multiple of the front end's total decimation moves up one sample, since such a shift hands every dwell
    the same output samples. Every build replays the same copy in a repeat. Drives the real script, so
    tests/CMakeLists.txt registers it beside ReplayAbAnalogMetric."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="replay-ab-realizations-"))
        (self.tmp / "caps").mkdir()
        self.capture = self.tmp / "caps" / "capture.iq.json"
        self.out = self.tmp / "out"
        self.builds = [self.tmp / "host.main", self.tmp / "host.copy"]
        for build in self.builds:
            write_script(build, FAKE_REPLAY_HOST)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_capture(self, meta):
        with open(self.capture, "w", encoding="utf-8") as handle:
            json.dump(meta, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        return self.capture.read_bytes()

    def replay_ab(self, *args, capture=None, cwd=None):
        return run([BASH, str(REPLAY_AB), "--capture", str(capture or self.capture), "--mode", "-fi", "--rate", "fast",
                    "--out", str(self.out), *args, *(str(build) for build in self.builds)], cwd=str(cwd or ROOT))

    def summary_rows(self):
        lines = (self.out / "summary.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0].split("\t"), SUMMARY_COLUMNS)
        return [dict(zip(SUMMARY_COLUMNS, line.split("\t"))) for line in lines[1:]]

    def replayed(self, build, rep):
        log = (self.out / f"{build}__r{rep}.log").read_text(encoding="utf-8")
        return [line[len("REPLAYED: "):] for line in log.splitlines() if line.startswith("REPLAYED: ")]

    def realization(self, rep):
        with open(self.out / "realizations" / f"r{rep}.json", encoding="utf-8") as handle:
            return json.load(handle)

    def assert_realizations(self, original, shifts, offsets, data_file):
        """Repeat r replayed realizations/r<r>.json in every build, which is the original sidecar with an absolute
        data_file naming the file data_file names and each event's byte_offset replaced by offsets[r-1], and
        summary.tsv records shifts[r-1]."""
        rows = self.summary_rows()
        self.assertEqual(len(rows), 2 * len(shifts))
        for rep, (shift, rep_offsets) in enumerate(zip(shifts, offsets), start=1):
            copy_path = str(self.out / "realizations" / f"r{rep}.json")
            for build in ("host.main", "host.copy"):
                self.assertEqual(self.replayed(build, rep), [copy_path])
            for row in rows:
                if row["rep"] == str(rep):
                    self.assertEqual(row["shift"], str(shift), row)
            copy = self.realization(rep)
            self.assertTrue(os.path.isabs(copy["data_file"]), copy["data_file"])
            self.assertEqual(os.path.realpath(copy["data_file"]), os.path.realpath(data_file))
            self.assertEqual([event["byte_offset"] for event in copy["events"]], rep_offsets)
            expected = json.loads(json.dumps(original))
            expected["data_file"] = copy["data_file"]
            for event, offset in zip(expected["events"], rep_offsets):
                event["byte_offset"] = offset
            self.assertEqual(copy, expected)

    def test_each_repeat_replays_its_own_copy_with_the_events_shifted(self):
        # 48 kHz cu8: P = 20 samples, so four repeats shift by 0, 6, 12 and 18 samples, 2 bytes each. The event at
        # 3990 stops at data_bytes from the second repeat on, and one already there stays. The capture is given by
        # a path relative to the working directory, so data_file must resolve against the sidecar's own directory.
        events = [frequency_event("RETUNE", 1000, 48000), mute_event(1000, 200), frequency_event("RESET", 1000, 48000),
                  frequency_event("RETUNE", 3990, 48000), frequency_event("RESET", 4000, 48000)]
        original = sidecar("cu8", 48000, "capture.iq", 4000, events)
        before = self.write_capture(original)
        result = self.replay_ab("--reps", "4", capture="caps/capture.iq.json", cwd=self.tmp)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_realizations(original, [0, 6, 12, 18],
                                 [[1000, 1000, 1000, 3990, 4000], [1012, 1012, 1012, 4000, 4000],
                                  [1024, 1024, 1024, 4000, 4000], [1036, 1036, 1036, 4000, 4000]],
                                 str(self.tmp / "caps" / "capture.iq"))
        self.assertEqual(self.capture.read_bytes(), before)
        self.assertRegex(result.stdout, r"(?m)^  r2\s+host\.\w+\s+shift=6\s")
        self.assertIn("0 to 18 input samples", result.stdout)
        self.assertNotIn(NO_EVENTS_WARNING, result.stdout)

        result = report(self.out / "summary.tsv", "--baseline", "host.main")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("realizations: 4 (events shifted 0 to 18 input samples)", result.stdout)

    def test_cf32_events_move_eight_bytes_a_sample(self):
        # 1.536 Msps: P = 640 samples, so three repeats shift by 0, 214 and 428 samples, 8 bytes each in cf32. An
        # absolute data_file is kept as it is.
        data = str(self.tmp / "elsewhere" / "capture.cf32")
        events = [frequency_event("RETUNE", 8000, 1536000), mute_event(8000, 4096),
                  frequency_event("RESET", 78000, 1536000)]
        original = sidecar("cf32", 1536000, data, 80000, events)
        self.write_capture(original)
        result = self.replay_ab("--reps", "3")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_realizations(original, [0, 214, 428],
                                 [[8000, 8000, 78000], [9712, 9712, 79712], [11424, 11424, 80000]], data)
        self.assertEqual({self.realization(rep)["data_file"] for rep in (1, 2, 3)}, {data})

    def test_absolute_cu8_data_file_is_kept(self):
        # 48 kHz, three repeats: 0, 7 and 15 samples, 2 bytes each.
        data = str(self.tmp / "elsewhere" / "capture.iq")
        events = [frequency_event("RETUNE", 1000, 48000), mute_event(1000, 200), frequency_event("RESET", 1000, 48000)]
        original = sidecar("cu8", 48000, data, 4000, events)
        self.write_capture(original)
        result = self.replay_ab("--reps", "3")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_realizations(original, [0, 7, 15], [[1000] * 3, [1014] * 3, [1030] * 3], data)
        self.assertEqual({self.realization(rep)["data_file"] for rep in (1, 2, 3)}, {data})

    def test_a_shift_on_a_whole_multiple_of_the_decimation_moves_up_a_sample(self):
        # 1.536 Msps decimated to 48 kHz: D = 32 and P = 640. Of nine repeats, the fifth and the ninth would shift by
        # 288 and 576 samples, 9 and 18 whole decimations, which hand every dwell the same output samples only later,
        # so each moves up one sample. D is sample_rate_hz / demod_rate_hz, else base_decimation x post_downsample;
        # with neither it is 1, every shift is a multiple of it, and none moves.
        bumped = [0, 72, 144, 216, 289, 360, 432, 504, 577]
        unbumped = [0, 72, 144, 216, 288, 360, 432, 504, 576]
        cases = (("demod_rate_hz", (), bumped, True),
                 ("base_decimation x post_downsample", ("demod_rate_hz",), bumped, True),
                 ("neither", ("demod_rate_hz", "base_decimation", "post_downsample"), unbumped, False))
        for name, missing, shifts, says_decimation in cases:
            with self.subTest(name):
                shutil.rmtree(self.out, ignore_errors=True)
                original = sidecar("cu8", 1536000, "capture.iq", 400000, [frequency_event("RESET", 8000, 1536000)])
                for key in missing:
                    del original[key]
                self.write_capture(original)
                result = self.replay_ab("--reps", "9")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assert_realizations(original, shifts, [[8000 + 2 * shift] for shift in shifts],
                                         str(self.tmp / "caps" / "capture.iq"))
                if says_decimation:
                    self.assertIn("multiple of the 32-sample decimation", result.stdout)
                else:
                    self.assertNotIn("decimation", result.stdout)

    def test_one_repeat_says_it_runs_unshifted(self):
        self.write_capture(sidecar("cu8", 48000, "capture.iq", 4000, [frequency_event("RESET", 1000, 48000)]))
        result = self.replay_ab("--reps", "1")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("realizations: only repeat 1 runs, unshifted", result.stdout)
        self.assertNotIn("0 to 0", result.stdout)
        self.assertNotIn("moved", result.stdout)
        self.assertEqual({row["shift"] for row in self.summary_rows()}, {"0"})

    def test_paths_with_spaces(self):
        # A space in the capture's directory, in its name and in --out reaches the sidecar copies, the hosts and the
        # summary whole.
        (self.tmp / "my caps").mkdir()
        self.capture = self.tmp / "my caps" / "scan capture.iq.json"
        self.out = self.tmp / "ab out"
        original = sidecar("cu8", 48000, "scan capture.iq", 4000, [frequency_event("RESET", 1000, 48000)])
        self.write_capture(original)
        result = self.replay_ab("--reps", "2")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"results: {self.out}", result.stdout)
        self.assert_realizations(original, [0, 11], [[1000], [1022]], str(self.tmp / "my caps" / "scan capture.iq"))
        self.assertEqual({row["case"] for row in self.summary_rows()}, {"scan capture.iq.json-fast"})

        shutil.rmtree(self.out)
        result = self.replay_ab("--reps", "2", "--no-realizations")
        self.assertEqual(result.returncode, 0, result.stdout)
        for rep in (1, 2):
            for build in ("host.main", "host.copy"):
                self.assertEqual(self.replayed(build, rep), [str(self.capture)])

    @unittest.skipUnless(hasattr(os, "symlink"), "needs symbolic links")
    def test_relative_data_file_resolves_as_replay_does_under_a_symlinked_directory(self):
        # Replay joins the sidecar's directory, as given, with a relative data_file, and the system then resolves
        # the path, following a symlinked directory before any "..". A copy that collapsed the ".." by itself would
        # name another file: here the decoy beside the link.
        real = self.tmp / "real" / "a" / "b"
        real.mkdir(parents=True)
        (self.tmp / "real" / "a" / "capture.iq").write_bytes(b"recorded")
        (self.tmp / "capture.iq").write_bytes(b"decoy")
        os.symlink(real, self.tmp / "link")
        self.capture = self.tmp / "link" / "capture.iq.json"
        self.write_capture(sidecar("cu8", 48000, "../capture.iq", 4000, [frequency_event("RESET", 1000, 48000)]))
        result = self.replay_ab("--reps", "2")
        self.assertEqual(result.returncode, 0, result.stdout)
        for rep in (1, 2):
            data = self.realization(rep)["data_file"]
            self.assertTrue(os.path.isabs(data), data)
            self.assertEqual(Path(data).read_bytes(), b"recorded", data)

    def test_capture_without_events_warns_once_and_replays_as_recorded(self):
        for events in (None, []):
            with self.subTest(events=events):
                shutil.rmtree(self.out, ignore_errors=True)
                self.write_capture(sidecar("cu8", 48000, "capture.iq", 4000, events))
                result = self.replay_ab("--reps", "3")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(result.stdout.count(NO_EVENTS_WARNING), 1, result.stdout)
                self.assertIn("realizations: none", result.stdout)
                self.assertFalse((self.out / "realizations").exists())
                for rep in (1, 2, 3):
                    for build in ("host.main", "host.copy"):
                        self.assertEqual(self.replayed(build, rep), [str(self.capture)])
                self.assertEqual({row["shift"] for row in self.summary_rows()}, {"0"})

    def test_no_realizations_replays_every_repeat_as_recorded(self):
        # The determinism control: identical repeats, which must pair to +0.00 +/- 0.00.
        self.write_capture(sidecar("cu8", 48000, "capture.iq", 4000, [frequency_event("RESET", 1000, 48000)]))
        result = self.replay_ab("--reps", "3", "--no-realizations")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("realizations: off", result.stdout)
        self.assertNotIn(NO_EVENTS_WARNING, result.stdout)
        self.assertFalse((self.out / "realizations").exists())
        for rep in (1, 2, 3):
            for build in ("host.main", "host.copy"):
                self.assertEqual(self.replayed(build, rep), [str(self.capture)])
        self.assertEqual({row["shift"] for row in self.summary_rows()}, {"0"})

    def test_unknown_sample_format_fails_before_any_replay(self):
        # The bytes a sample takes are known only for cu8 and cf32, the formats replay accepts.
        self.write_capture(sidecar("cs16", 48000, "capture.iq", 4000, [frequency_event("RESET", 1000, 48000)]))
        result = self.replay_ab("--reps", "2")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("sample_format 'cs16'", result.stdout)
        self.assertEqual(list(self.out.glob("*.log")), [])

        # --no-realizations needs no shift, so it still replays such a capture.
        result = self.replay_ab("--reps", "2", "--no-realizations")
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
