#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/bench_scan_skip_silent.py's per-mode report names only modes the run decoded (issue #834).

- --scan untyped runs every row under -ft (DMR and P25) from a map with no mode column: it refuses --silent-modes,
  which would name modes no row runs, and the typed scan takes them.
- --by-mode groups an untyped run's silent visits under the one decoder set every row runs, and a typed run's under
  each row's mode, a frontend's (--serve, scored with --analyze) by the list it scanned.
- --by-mode counts the visits that left an active channel mid-call, those at the call's onset included.

These run in seconds and never start a decoder or serve a scene.
"""

import contextlib
import importlib
import importlib.util
import io
import os
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

if importlib.util.find_spec("numpy") is None:
    # The tool needs numpy; without it this is a skip CTest reports (SKIP_RETURN_CODE 77), not a pass.
    print(f"SKIP: tools/bench_scan_skip_silent.py needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tools"))

bench = importlib.import_module("bench_scan_skip_silent")


def parse(*argv):
    """parse_args() on @p argv: the args, or the exit code and the message it refused them with."""
    err = io.StringIO()
    with unittest.mock.patch.object(sys, "argv", ["bench_scan_skip_silent.py", *argv]), contextlib.redirect_stderr(err):
        try:
            return bench.parse_args(), None
        except SystemExit as stop:
            return stop.code, err.getvalue()


BASE_ARGS = ("--dsd", "dsd-neo", "--dmr-synth-gen", "gen", "--out", "out")


class SilentModes(unittest.TestCase):
    def test_untyped_refuses_silent_modes(self):
        code, message = parse(*BASE_ARGS, "--scan", "untyped", "--by-mode", "--silent-modes", "nxdn48,dpmr")
        self.assertEqual(code, 2)
        self.assertIn("--scan untyped", message)
        self.assertIn("-ft", message)

    def test_untyped_takes_the_default_modes(self):
        args, message = parse(*BASE_ARGS, "--scan", "untyped", "--by-mode")
        self.assertIsNone(message)
        self.assertEqual(args.silent_modes, bench.ACTIVE)

    def test_typed_takes_silent_modes(self):
        args, message = parse(*BASE_ARGS, "--scan", "typed", "--by-mode", "--silent-modes", "nxdn48,dpmr")
        self.assertIsNone(message)
        self.assertEqual(args.silent_modes, ("nxdn48", "dpmr"))

    def test_silent_modes_go_to_the_silent_channels_in_turn(self):
        # Issue #834: by silent ordinal, not channel index, which counts the active channels too: with three silent
        # channels both modes run, and a trunk target list has the nxdn48 ones.
        channels = bench.scene_layout(3)
        modes = [bench.channel_mode(c, ("dpmr", "nxdn48")) for c in channels if c.kind == "silent"]
        self.assertEqual(modes, ["dpmr", "nxdn48", "dpmr"])
        with tempfile.TemporaryDirectory() as out_dir:
            paths = bench.write_lists(channels, out_dir, ("dpmr", "nxdn48"))
            with open(paths["trunk"], encoding="ascii") as trunk:
                self.assertIn("nxdn48-conventional", trunk.read())


    def test_a_trunk_run_with_no_silent_target_is_refused(self):
        # One silent channel takes the first mode, dPMR, which a trunk target list has no type for: nothing silent runs.
        code, message = parse(*BASE_ARGS, "--scan", "trunk", "--silent", "1", "--silent-modes", "dpmr,nxdn48")
        self.assertEqual(code, 2)
        self.assertIn("no silent", message)

    def test_rescoring_a_saved_trunk_run_is_not_refused(self):
        # A saved run is scored with its own map: the check of today's rotation is for a run about to start.
        for extra in (("--rescore",), ("--analyze", "logcat.txt")):
            with self.subTest(extra=extra):
                _args, message = parse(
                    *BASE_ARGS, "--scan", "trunk", "--silent", "1", "--silent-modes", "dpmr,nxdn48", *extra
                )
                self.assertIsNone(message)

    def test_rescoring_takes_the_modes_the_run_used(self):
        # A saved run is scored with the modes its typed map gave each frequency, whatever today's rotation gives.
        channels = bench.scene_layout(3)
        with tempfile.TemporaryDirectory() as out_dir:
            with open(os.path.join(out_dir, "typed.csv"), "w", encoding="ascii") as typed:
                typed.write("channel,frequency_hz,name,mode,options\n")
                modes = {c.index: "dpmr" if c.kind == "silent" else c.kind for c in channels}
                typed.writelines(f"{c.index + 1},{c.frequency},{c.name},{modes[c.index]},\n" for c in channels)
            bench.apply_saved_modes(channels, out_dir)
        modes = [bench.channel_mode(c, ("dpmr", "nxdn48")) for c in channels if c.kind == "silent"]
        self.assertEqual(modes, ["dpmr", "dpmr", "dpmr"])


class DecoderCommand(unittest.TestCase):
    def test_an_uncapped_run_disables_the_cap(self):
        # A cap a DSD_NEO_CONFIG file sets must not stay in force: 0 is passed, as the decoder's "off".
        args, message = parse(*BASE_ARGS, "--scan", "typed", "--max-visit-ms", "0")
        self.assertIsNone(message)
        cmd = bench.dsd_command(args, 1234, {"trunk": "t.csv", "typed": "y.csv", "untyped": "u.csv"})
        at = cmd.index("--scan-max-visit-ms")
        self.assertEqual(cmd[at + 1], "0")


def visits_record(scan, modes, channels):
    """A run that visits each silent channel twice, 2 s each, after a lead visit; the last visit is cut off."""
    tunes = [(0, 0.0, channels[0].frequency, 0)]
    order = [c for c in channels] * 2 + [channels[0]]
    for k, c in enumerate(order, start=1):
        tunes.append((0, 2.0 * k, c.frequency, 0))
    return {"scan": scan, "silent_modes": list(modes), "scene_started": 0.0, "tunes": tunes, "wall_s": 2.0 * len(tunes)}


class ByMode(unittest.TestCase):
    def setUp(self):
        self.channels = [bench.Channel(i, "silent") for i in range(4)]

    def test_untyped_visits_are_one_decoder_set(self):
        record = visits_record("untyped", bench.ACTIVE, self.channels)
        out = bench.silent_by_mode(record, self.channels, [], [])
        self.assertEqual(len(out), 1, f"one decoder set ran every row, the report has {sorted(out)}")
        self.assertEqual(set(out), {bench.UNTYPED_MODE})
        self.assertEqual(out[bench.UNTYPED_MODE]["visits"], 8)

    def test_typed_visits_are_each_rows_mode(self):
        record = visits_record("typed", ("nxdn48", "dpmr"), self.channels)
        out = bench.silent_by_mode(record, self.channels, [], [])
        self.assertEqual(set(out), {"nxdn48", "dpmr"})
        self.assertEqual(sum(row["visits"] for row in out.values()), 8)


class LeftMidCall(unittest.TestCase):
    """--by-mode counts every visit that left an active channel with LEFT_MID_CALL_S or more of its call to go, those
    that left within the call's first 200 ms included; those are also reported on their own."""

    def record(self, channels, leave_s):
        silent, dmr = channels
        tunes = [(0.0, 0.0, silent.frequency, 0), (5.0, 0.0, dmr.frequency, 0), (leave_s, 0.0, silent.frequency, 0)]
        tunes.append((20.0, 0.0, dmr.frequency, 0))
        return {"tunes": tunes, "end_s": 40.0}

    def setUp(self):
        self.channels = [bench.Channel(0, "silent"), bench.Channel(1, "dmr")]
        self.channels[1].starts = bench.np.array([10.0, 30.0])

    def test_a_departure_at_the_calls_onset_counts(self):
        out = bench.left_mid_call(self.record(self.channels, 10.1), self.channels)
        self.assertEqual(out.get("dmr"), 1, f"left 100 ms into a {bench.CALL_S} s call: {out}")
        self.assertEqual(out.get("dmr in its first 200 ms"), 1, f"and reported on its own: {out}")

    def test_a_departure_later_in_the_call(self):
        out = bench.left_mid_call(self.record(self.channels, 11.0), self.channels)
        self.assertEqual(out.get("dmr"), 1, f"left 1 s into the call: {out}")
        self.assertIsNone(out.get("dmr in its first 200 ms"), f"not at its onset: {out}")

    def test_a_departure_with_little_of_the_call_left(self):
        out = bench.left_mid_call(self.record(self.channels, 10.0 + bench.CALL_S - 0.2), self.channels)
        self.assertEqual(out.get("dmr"), 0, f"200 ms of the call left: {out}")


class SavedRunReport(unittest.TestCase):
    """A run scored again (--rescore) or a frontend's (--analyze) reports by mode when the scoring asks for it, whatever
    the run saved; a frontend run (--serve) saves its mode list, which the report reads."""

    def test_rescore_by_mode_asks_for_the_report(self):
        args, message = parse(*BASE_ARGS, "--rescore", "--by-mode")
        self.assertIsNone(message)
        record = bench.report_options({"scan": "typed"}, args)
        self.assertTrue(record["by_mode"])
        self.assertEqual(tuple(record["silent_modes"]), bench.ACTIVE)

    def test_rescore_keeps_the_saved_modes(self):
        args, _message = parse(*BASE_ARGS, "--rescore", "--by-mode")
        record = bench.report_options({"scan": "typed", "silent_modes": ["dpmr"], "by_mode": False}, args)
        self.assertTrue(record["by_mode"])
        self.assertEqual(record["silent_modes"], ["dpmr"])

    def test_serve_saves_its_modes(self):
        args, message = parse(
            "--serve", "5000", "--dmr-synth-gen", "gen", "--out", "out", "--silent-modes", "nxdn48,dpmr", "--by-mode"
        )
        self.assertIsNone(message)
        record = bench.frontend_record(args)
        self.assertEqual(record["silent_modes"], ["nxdn48", "dpmr"])
        self.assertTrue(record["by_mode"])

    def test_an_untyped_frontend_run_is_one_decoder_set(self):
        """A frontend scanning the untyped map (--serve --scan untyped) is scored as one ("scan": "frontend") and its
        silent visits grouped as untyped, not by the modes typed.csv gives their frequencies (apply_saved_modes())."""
        args, message = parse("--serve", "5000", "--dmr-synth-gen", "gen", "--out", "out", "--scan", "untyped")
        self.assertIsNone(message)
        record = bench.frontend_record(args)
        self.assertEqual(record["scan"], "frontend")
        self.assertEqual(record["scan_variant"], "untyped")
        channels = [bench.Channel(i, "silent") for i in range(4)]
        for c, mode in zip(channels, ("nfm", "dmr", "p25", "nxdn48")):
            c.mode = mode  # what the saved typed.csv would give them
        record.update(visits_record("frontend", record["silent_modes"], channels))
        out = bench.silent_by_mode(record, channels, [], [])
        self.assertEqual(set(out), {bench.UNTYPED_MODE}, f"every row ran -ft, the report has {sorted(out)}")
        self.assertEqual(out[bench.UNTYPED_MODE]["visits"], 8)

    def test_a_typed_frontend_run_is_each_rows_mode(self):
        args, _message = parse("--serve", "5000", "--dmr-synth-gen", "gen", "--out", "out", "--scan", "typed")
        record = bench.frontend_record(args)
        self.assertEqual(record["scan_variant"], "typed")
        channels = [bench.Channel(i, "silent") for i in range(4)]
        record.update(visits_record("frontend", ("nxdn48", "dpmr"), channels))
        out = bench.silent_by_mode(record, channels, [], [])
        self.assertEqual(set(out), {"nxdn48", "dpmr"})


class StampedLines(unittest.TestCase):
    """dsd-neo's output, as stamp_lines() logs it, ends each line with one newline whatever the child wrote: LF, or CRLF
    on Windows. The log's readers match its lines up to their end."""

    def stamp(self, data):
        log = io.StringIO()
        with unittest.mock.patch.object(bench.time, "monotonic", return_value=12.5):
            bench.stamp_lines(io.BytesIO(data), log)
        return log.getvalue()

    def test_lf_lines(self):
        self.assertEqual(self.stamp(b"one\n\x1b[1mtwo\x1b[0m\n"), "12.500 one\n12.500 two\n")

    def test_crlf_lines_keep_no_carriage_return(self):
        logged = self.stamp(b"one\r\n\x1b[1mtwo\x1b[0m\r\n")
        self.assertNotIn("\r", logged)
        self.assertEqual(logged, "12.500 one\n12.500 two\n")
        stamped = [bench.STAMPED.match(line) for line in logged.splitlines()]
        self.assertEqual([match.group(2) for match in stamped], ["one", "two"])


if __name__ == "__main__":
    unittest.main()
