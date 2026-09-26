#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stdlib-only regressions for tools/replay_ab.sh --metric analog and tools/replay_ab_report.py.

The analog metrics come from the analog replay host's "ANALOG METRIC:" and "ANALOG PROBE:" lines. What has to hold
is the same as for the digital metrics: every build is compared with the baseline inside one repeat, never across
repeats, and a build compared with itself reports no difference.

ReplayAbReport runs the report on canned summaries and needs only Python; ReplayAbAnalogMetric also runs
replay_ab.sh. Name a class on the command line to run just that one, as tests/CMakeLists.txt does.
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
# The bash tests/CMakeLists.txt found when it registered the replay_ab.sh half.
BASH = os.environ.get("DSD_TEST_BASH", "bash")

COLUMNS = [
    "variant", "case", "rep", "errs", "voice", "sync",
    "tone_snr_db", "inband_db", "clip", "audible_ms", "first_audible_ms", "rms_dbfs",
    "probe_hz", "probe_dbfs", "probe_dbc", "tone", "tone_lock_ms", "tone_lock_pct", "rc", "off_path",
]


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
            self.assertEqual(row["tone_lock_pct"], "0.00")
            self.assertEqual(row["rc"], "0")
            self.assertEqual(row["off_path"], "0")

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


if __name__ == "__main__":
    unittest.main()
