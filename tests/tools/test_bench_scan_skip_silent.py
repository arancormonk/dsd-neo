#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/bench_scan_skip_silent.py's per-mode report names only modes the run decoded (issue #834).

- --scan untyped runs every row under -ft (DMR and P25) from a map with no mode column: it refuses --silent-modes,
  which would name modes no row runs, and the typed scan takes them.
- --by-mode groups an untyped run's silent visits under the one decoder set every row runs, and a typed run's under
  each row's mode, a frontend's (--serve, scored with --analyze) by the list it scanned.
- --by-mode counts the visits that left an active channel mid-call, those at the call's onset included.
- A call is on air while its carrier is keyed (a DMR call from the end of its silent lead, as the DMR generator builds
  it), and dsd-neo's lines go on the scene clock through the scene's retunes, so a stall that leaves the scene behind
  the wall clock moves no skip. A skip counts against the one call keyed at the skip, from its key-up (as air()
  computes it, so a call scheduled off the grid is keyed at its own key-up) to (not including) its key-down.
- A whole-second stamp (the frame log's, the recording journal's) is matched over the span of its second on the scene
  clock, which during a catch-up holds more than a second.
- A frontend's recording journal is stamped by the device's clock, so it is put on the scene clock with the same
  device shift as its logcat, on --analyze and on --rescore. The shift is fitted by aligning the device's retunes in
  order with the scene's; with no retunes to go on, too few, or an alignment a lap away that fits as well, --analyze
  refuses and replaces neither dsd.log nor run.json.

These run in seconds and never start a decoder or serve a scene. The DMR cases run the DMR test generator
(dsd-neo_test_dmr_synth_gen, named by DSD_NEO_TEST_DMR_SYNTH_GEN, which CTest sets) once.
"""

import argparse
import contextlib
import functools
import json
import importlib
import importlib.util
import io
import os
import sys
import tempfile
import time
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


@functools.lru_cache(maxsize=1)
def generated_dmr():
    """The DMR call as the scene builds it (dmr_call(), from the DMR test generator): (keyed, frames, expected)."""
    gen = os.environ.get("DSD_NEO_TEST_DMR_SYNTH_GEN")
    if not gen:
        raise AssertionError("set DSD_NEO_TEST_DMR_SYNTH_GEN to dsd-neo_test_dmr_synth_gen (CTest sets it)")
    bench.reception.ARGS = argparse.Namespace(dsd=None, dmr_synth_gen=gen)
    channel = bench.Channel(1, "dmr")
    with tempfile.TemporaryDirectory(prefix="dmr-call-") as work_dir:
        bench.dmr_call(channel, work_dir)
    return channel.keyed, list(channel.frames), list(channel.expected)


def dmr_channel(index, starts):
    """A DMR channel whose call is the generator's (dmr_call()), scheduled at starts."""
    channel = bench.Channel(index, "dmr")
    channel.keyed, frames, expected = generated_dmr()
    channel.frames, channel.expected = list(frames), list(expected)
    channel.starts = bench.np.array(starts)
    return channel


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
        out = bench.silent_by_mode(record, self.channels, [])
        self.assertEqual(len(out), 1, f"one decoder set ran every row, the report has {sorted(out)}")
        self.assertEqual(set(out), {bench.UNTYPED_MODE})
        self.assertEqual(out[bench.UNTYPED_MODE]["visits"], 8)

    def test_typed_visits_are_each_rows_mode(self):
        record = visits_record("typed", ("nxdn48", "dpmr"), self.channels)
        out = bench.silent_by_mode(record, self.channels, [])
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


def ms_burst_mask(seconds):
    """The DMR MS burst train's on-mask as tools/scan_quiet_reception.py dmr_ms_source() lays it out, every burst
    keyed: dibits 12-143 of every even 144-dibit period after the 960-dibit silent lead, at 48 kHz."""
    rx = bench.reception
    sps = rx.RATE_HZ // 4800
    symbol = (bench.np.arange(round(seconds * rx.RATE_HZ)) + sps // 2) // sps - rx.DMR_LEAD_SYMBOLS
    period = bench.np.floor_divide(symbol, rx.DMR_SLOT_SYMBOLS)
    return (symbol >= 0) & (period % 2 == 0) & (bench.np.mod(symbol, rx.DMR_SLOT_SYMBOLS) >= 12)


# The DMR call's keyed air: its first burst's region B starts 972 symbols in, less half a symbol (202.4 ms), and its
# up-ramp 1.5 ms before that; the burst train is keyed to the end of the call's CALL_S.
DMR_KEYED_S = (972 * 10 - 5) / 48000 - 0.0015


class KeyedAir(unittest.TestCase):
    """A call is on air while its carrier is keyed, not from its scheduled start: a DMR call keys about 200.9 ms after
    it, at the end of the generator's silent lead (issue #834). The DMR channel here is the one the scene builds, from
    the DMR test generator. A skip is scored against the one call keyed at the skip, keyed from its key-up up to, not
    including, its key-down: keyed at the landing too, it was keyed through the reading; keyed up after the landing,
    it was keyed after landing."""

    def setUp(self):
        self.silent = bench.Channel(0, "silent")
        self.dmr = dmr_channel(1, [10.0, 30.0])
        self.key_up, self.key_down = self.dmr.air(10.0)

    def visit(self, landed, left):
        """A visit to the DMR channel from landed to left, after one to the silent channel and before another."""
        return [(self.silent, 0.0, landed), (self.dmr, landed, left), (self.silent, left, 40.0)]

    def skip(self, landed, at):
        return bench.skips_on_air([(at, float(self.dmr.frequency))], self.visit(landed, at))

    def test_the_ms_burst_trains_keyed_air(self):
        on, off = bench.keyed_span(ms_burst_mask(10.0), bench.reception.RATE_HZ)
        self.assertAlmostEqual(on, DMR_KEYED_S, delta=1e-9)
        self.assertAlmostEqual(on, 0.2009, delta=0.0001)
        self.assertAlmostEqual(off, bench.CALL_S, delta=1e-9)

    def test_the_generated_call_keys_after_its_lead(self):
        self.assertAlmostEqual(self.dmr.keyed[0], DMR_KEYED_S, delta=1e-9)
        self.assertAlmostEqual(self.dmr.keyed[1], bench.CALL_S, delta=1e-9)

    def test_a_continuous_call_is_keyed_throughout(self):
        self.assertEqual(bench.keyed_span(None, bench.reception.RATE_HZ), (0.0, bench.CALL_S))

    def test_a_skip_in_the_silent_lead_is_not_on_air(self):
        # (a) Landed at the call's scheduled start, skipped 150 ms later: the carrier had not keyed yet.
        self.assertEqual(self.skip(10.0, 10.15), [0, 0])

    def test_a_skip_in_the_silent_lead_is_not_keyed_after_landing(self):
        # (a) The scheduled start falls between the landing and the skip, but the key-up comes after the skip.
        self.assertEqual(self.skip(9.9, 10.15), [0, 0])

    def test_a_call_keyed_through_the_whole_visit_is_on_air(self):
        # (b) Landed after the key-up, skipped before the call's end: a call read as silent.
        self.assertEqual(self.skip(10.3, 11.0), [1, 0])

    def test_a_call_keyed_at_the_landing_instant_is_keyed_through(self):
        self.assertEqual(self.skip(self.key_up, 11.0), [1, 0])

    def test_a_key_up_between_the_landing_and_the_skip_is_keyed_after_landing(self):
        # (c) Landed in the silent lead (after the scheduled start), the carrier keyed before the skip.
        self.assertEqual(self.skip(10.1, 10.5), [0, 1])

    def test_a_key_up_at_the_skip_is_keyed_after_landing(self):
        # Keyed from its key-up on: a skip at that instant is a skip of a call keyed after landing.
        self.assertEqual(self.skip(10.1, self.key_up), [0, 1])

    def test_a_skip_after_the_key_down_is_not_on_air(self):
        # Landed in the silent lead (10.1 s), the call keyed from about 10.201 s to 14.0 s: a skip at 14.1 s is of a
        # channel whose transmitter is off, though the key-up came between the landing and the skip.
        self.assertEqual(self.skip(10.1, 14.1), [0, 0])

    def test_a_skip_at_the_key_down_is_not_on_air(self):
        # The key-down instant is the first one off the air.
        self.assertEqual(self.skip(10.3, self.key_down), [0, 0])

    def test_a_skip_just_before_the_key_down_is_on_air(self):
        self.assertEqual(self.skip(10.3, self.key_down - 0.001), [1, 0])

    def test_the_call_on_air_is_the_keyed_one(self):
        self.assertIsNone(self.dmr.call_at(10.15), "in the silent lead")
        self.assertEqual(self.dmr.call_at(10.25), 10.0)
        self.assertIsNone(self.dmr.call_at(self.key_down), "at the key-down")
        self.assertIsNone(self.dmr.call_at(10.0 + bench.CALL_S + 0.05), "after the call")

    def left(self, landed, left):
        record = {"tunes": [(t0, 0.0, c.frequency, 0) for c, t0, _t1 in self.visit(landed, left)], "end_s": 40.0}
        record["tunes"].append((45.0, 0.0, self.dmr.frequency, 0))
        return bench.left_mid_call(record, [self.silent, self.dmr])

    def test_a_visit_wholly_in_the_silent_lead_did_not_leave_mid_call(self):
        out = self.left(9.5, 10.1)
        self.assertEqual(out.get("dmr"), 0, f"left before the key-up: {out}")
        self.assertIsNone(out.get("dmr in its first 200 ms"), out)

    def test_a_visit_keyed_through_that_leaves_soon_after_the_key_up_is_at_its_onset(self):
        # (b) The visit sat on the keyed call and left 149 ms after the key-up: mid-call, at its onset.
        out = self.left(10.21, DMR_KEYED_S + 10.149)
        self.assertEqual(out.get("dmr"), 1, out)
        self.assertEqual(out.get("dmr in its first 200 ms"), 1, f"149 ms after the key-up: {out}")

    def test_a_call_is_tuned_only_while_keyed(self):
        # A visit wholly inside the silent lead did not sit on the call.
        record = run_record([(0.0, 100.0, self.silent), (9.5, 109.5, self.dmr), (10.15, 110.15, self.silent)])
        record["tunes"].append((20.0, 120.0, self.dmr.frequency, 0))
        self.dmr.frames, self.dmr.expected = [], []
        with tempfile.TemporaryDirectory() as out_dir:
            Path(out_dir, "dsd.log").write_text("", encoding="utf-8")
            result = bench.score(record, [self.silent, self.dmr], out_dir)
        self.assertEqual([row["tuned"] for row in result["calls"]["dmr"]], [False, True])


def run_record(tunes, end_s=40.0):
    """A saved run of a typed scan: tunes as (scene s, monotonic s, channel), the scene served from monotonic 100 s."""
    record = {"scan": "typed", "skip": "on", "level": "strong", "seed": 1, "jitter_s": 0.0, "end_s": end_s}
    record.update({"wall_s": end_s, "process_cpu_s": None, "load_average_end": None, "wall_offset_s": 0.0})
    record["scene_started"] = 100.0
    record["tunes"] = [(scene, mono, c.frequency, 1024000) for scene, mono, c in tunes]
    return record


class SceneClock(unittest.TestCase):
    """dsd-neo's stamped lines go on the scene clock through the retunes the scene logged, not by wall time from the
    run's start: when the host stalls, the paced server falls behind the wall clock until it catches up (issue #834)."""

    def setUp(self):
        self.silent = bench.Channel(0, "silent")
        self.p25 = bench.Channel(1, "p25")

    def score_skip(self, tunes, starts, skip_mono):
        """The skips_on_air of one run whose log holds one skip of the P25 channel, stamped skip_mono."""
        self.p25.starts = bench.np.array(starts)
        record = run_record(tunes)
        with tempfile.TemporaryDirectory() as out_dir:
            line = f"{skip_mono:.3f} NOTICE: scan: skipped quiet row 2 {self.p25.frequency / 1e6:.6f} MHz: -97.2 dBFS\n"
            Path(out_dir, "dsd.log").write_text(line, encoding="utf-8")
            return bench.score(record, [self.silent, self.p25], out_dir)["skips_on_air"]

    def test_a_skip_before_a_stalled_retune_is_before_a_call_that_starts_after_it(self):
        # The server stalled 0.4 s before the retune off the channel: by wall time the skip, 20 ms before that retune,
        # would land 0.38 s after it, after the call's start at 20.2 s, and read as a call keyed after landing.
        tunes = [(0.0, 100.0, self.silent), (10.0, 110.0, self.p25), (20.0, 120.4, self.silent)]
        tunes.append((30.0, 130.4, self.p25))
        self.assertEqual(self.score_skip(tunes, [20.2], 120.38), [0, 0])

    def test_a_skip_before_a_stalled_retune_is_inside_the_call_on_air(self):
        # The call is on air from 8.1 to 12.1 s; the skip, 20 ms before a retune at 12.0 s that the stall put 0.4 s
        # late, was inside it: by wall time it would land after the call's end.
        tunes = [(0.0, 100.0, self.silent), (10.0, 110.0, self.p25), (12.0, 112.4, self.silent)]
        tunes.append((30.0, 130.4, self.p25))
        self.assertEqual(self.score_skip(tunes, [8.1], 112.38), [1, 0])

    def test_lines_between_retunes(self):
        record = run_record([(0.0, 100.0, self.silent), (10.0, 110.0, self.p25), (20.0, 120.4, self.silent)])
        clock = bench.scene_clock(record)
        self.assertAlmostEqual(clock(110.0), 10.0, delta=1e-9)
        self.assertAlmostEqual(clock(105.0), 5.0, delta=1e-9, msg="an interval with no stall runs at the wall's pace")
        self.assertAlmostEqual(clock(120.4), 20.0, delta=1e-9)
        self.assertLessEqual(clock(120.39), 20.0, "never past the next retune")
        self.assertAlmostEqual(clock(120.39), 20.0, delta=0.02, msg="a line just before a stalled retune is at it")
        self.assertAlmostEqual(clock(121.0), 20.6, delta=1e-9, msg="after the last retune, at the wall's pace")

    def test_a_scene_that_caught_up_puts_a_line_before_the_next_retune_at_it(self):
        # The server was 0.3 s behind at the landing and had caught up by the next retune.
        record = run_record([(0.0, 100.0, self.silent), (10.0, 110.3, self.p25), (12.0, 112.0, self.silent)])
        self.assertAlmostEqual(bench.scene_clock(record)(111.98), 12.0, delta=0.03)


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
        out = bench.silent_by_mode(record, channels, [])
        self.assertEqual(set(out), {bench.UNTYPED_MODE}, f"every row ran -ft, the report has {sorted(out)}")
        self.assertEqual(out[bench.UNTYPED_MODE]["visits"], 8)

    def test_a_typed_frontend_run_is_each_rows_mode(self):
        args, _message = parse("--serve", "5000", "--dmr-synth-gen", "gen", "--out", "out", "--scan", "typed")
        record = bench.frontend_record(args)
        self.assertEqual(record["scan_variant"], "typed")
        channels = [bench.Channel(i, "silent") for i in range(4)]
        record.update(visits_record("frontend", ("nxdn48", "dpmr"), channels))
        out = bench.silent_by_mode(record, channels, [])
        self.assertEqual(set(out), {"nxdn48", "dpmr"})


class SummaryLabels(unittest.TestCase):
    """The scorer knows the landing and the skip, not when the reading's first sample was taken: the second bucket of
    skips on air is named for what it measures, a call keyed after landing."""

    def test_the_summary_names_the_second_bucket_keyed_after_landing(self):
        header = bench.summary_table([])
        self.assertIn("skips (keyed through / keyed after landing)", header)
        self.assertNotIn("starting", header)


def logcat_line(device_s, text):
    """One adb logcat -v epoch line from the frontend, stamped device_s on the device's clock."""
    return f"{device_s:.3f}  1234  5678 I dsd-neo : {text}\n"


class FrontendDeviceClock(unittest.TestCase):
    """A frontend's recording journal is stamped by the device, as its logcat is: --analyze puts both on the scene clock
    with the device's shift, estimated from its retunes, and saves the shift so --rescore scores the journal the same
    way. Here the device runs 5 s ahead of the host."""

    AHEAD_S = 5.0

    def setUp(self):
        self.silent = bench.Channel(0, "silent")
        self.p25 = bench.Channel(1, "p25")
        self.p25.starts = bench.np.array([20.0])
        self.dir = tempfile.TemporaryDirectory(prefix="frontend-")
        self.out = self.dir.name
        tunes = [(0.0, 100.0, self.silent), (10.0, 110.0, self.p25), (25.0, 125.0, self.silent)]
        tunes.append((30.0, 130.0, self.p25))
        self.record = run_record(tunes)
        self.record.update({"scan": "frontend", "scan_variant": "typed", "silent": 1})
        with open(os.path.join(self.out, "run.json"), "w", encoding="utf-8") as handle:
            json.dump(self.record, handle)
        logcat = "".join(
            logcat_line(mono + self.AHEAD_S, f"Retune applied: {centre} Hz")
            for _scene, mono, centre, _rate in self.record["tunes"][1:]
        )
        self.logcat = os.path.join(self.out, "logcat.txt")
        Path(self.logcat).write_text(logcat, encoding="utf-8")
        # A recording of the call (keyed 20 to 24 s scene time, host monotonic 120 to 124 s) that the device started at
        # its own 125 s, the host's 120 s: without the device's shift it lands at scene 25 s, after the call ended.
        entry = {"type": "publish", "system": bench.P25_SYSTEM, "start": 120 + int(self.AHEAD_S), "duration_ms": 3000}
        Path(self.out, "calls.jsonl").write_text(json.dumps(entry) + "\n", encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def analyze(self):
        args = argparse.Namespace(analyze=self.logcat, out=self.out)
        with contextlib.redirect_stdout(io.StringIO()):
            bench.analyze(args, dict(self.record), [self.silent, self.p25])
        with open(os.path.join(self.out, "result.json"), encoding="utf-8") as handle:
            return json.load(handle)

    def test_analyze_puts_the_journal_on_the_device_shift(self):
        result = self.analyze()
        self.assertEqual([row["caught"] for row in result["calls"]["p25"]], [True], result["calls"])

    def test_rescore_keeps_the_device_shift(self):
        self.analyze()
        with open(os.path.join(self.out, "run.json"), encoding="utf-8") as handle:
            saved = json.load(handle)
        self.assertAlmostEqual(saved.get("device_clock_shift_s", 0.0), self.AHEAD_S, delta=1e-6)
        result = bench.score(saved, [self.silent, self.p25], self.out)
        self.assertEqual([row["caught"] for row in result["calls"]["p25"]], [True], result["calls"])

    def test_rescore_of_a_frontend_run_never_analyzed_is_refused(self):
        # Its journal's device shift is only in its logcat: scored with none, a recording can fall outside its call.
        argv = ["bench_scan_skip_silent.py", "--rescore", "--dmr-synth-gen", "gen", "--out", self.out]
        with unittest.mock.patch.object(sys, "argv", argv), self.assertRaises(SystemExit) as stop:
            bench.main()
        self.assertIn("--analyze", str(stop.exception.code))


def alternating_record(seconds=40.0, every_s=1.0):
    """A frontend run whose scan alternates between a silent and a P25 channel every every_s from monotonic 100 s
    (scene time is monotonic less 100 s), so each channel is revisited every 2 * every_s; the P25 call is keyed from
    scene 20 s to 24 s. Returns (record, silent, p25)."""
    silent, p25 = bench.Channel(0, "silent"), bench.Channel(1, "p25")
    p25.starts = bench.np.array([20.0])
    count = int(seconds / every_s)
    record = run_record([(k * every_s, 100.0 + k * every_s, (silent, p25)[k % 2]) for k in range(count)], seconds)
    record.update({"scan": "frontend", "scan_variant": "typed", "silent": 1})
    return record, silent, p25


class DeviceClockFit(unittest.TestCase):
    """--analyze fits the device's clock shift by aligning its logcat's retunes, in order, with the scene's tune log,
    and takes the shift from the aligned pairs. A channel revisited every 2 s matches a retune laps away just as well by
    frequency and time alone, so nearest-visit matching measured no shift for a device 6 s ahead. With no retune
    evidence, or an alignment that another lap fits as well, it refuses and leaves the run's files as they were."""

    AHEAD_S = 6.0

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix="device-clock-")
        self.out = self.dir.name

    def tearDown(self):
        self.dir.cleanup()

    def write_run(self, record, retunes, recording_host_s):
        """run.json, an old dsd.log, the logcat (retunes as (host monotonic s, Hz), logged AHEAD_S ahead) and a
        journal holding one 1 s P25 recording the device started at host recording_host_s."""
        record = dict(record, **{bench.DEVICE_SHIFT: 1.25})  # a shift an earlier --analyze measured
        Path(self.out, "run.json").write_text(json.dumps(record), encoding="utf-8")
        Path(self.out, "dsd.log").write_text("100.000 an earlier analysis\n", encoding="utf-8")
        logcat = "".join(logcat_line(t + self.AHEAD_S, f"Retune applied: {hz} Hz") for t, hz in retunes)
        Path(self.out, "logcat.txt").write_text(logcat, encoding="utf-8")
        start = int(recording_host_s + self.AHEAD_S)
        entry = {"type": "publish", "system": bench.P25_SYSTEM, "start": start, "duration_ms": 1000}
        Path(self.out, "calls.jsonl").write_text(json.dumps(entry) + "\n", encoding="utf-8")
        return record

    def analyze(self, record, channels):
        args = argparse.Namespace(analyze=os.path.join(self.out, "logcat.txt"), out=self.out)
        with contextlib.redirect_stdout(io.StringIO()):
            bench.analyze(args, dict(record), channels)
        with open(os.path.join(self.out, "result.json"), encoding="utf-8") as handle:
            return json.load(handle)

    def saved(self):
        with open(os.path.join(self.out, "run.json"), encoding="utf-8") as handle:
            return json.load(handle)

    def refused(self, record, channels):
        """The message analyze() refused with; the run's run.json and dsd.log are as they were."""
        before = {name: Path(self.out, name).read_text(encoding="utf-8") for name in ("run.json", "dsd.log")}
        with self.assertRaises(SystemExit) as stop:
            self.analyze(record, channels)
        self.assertNotIn(stop.exception.code, (0, None), "a refusal exits non-zero")
        for name, text in before.items():
            self.assertEqual(Path(self.out, name).read_text(encoding="utf-8"), text, f"{name} was replaced")
        self.assertFalse(Path(self.out, "result.json").exists(), "nothing was scored")
        return str(stop.exception.code)

    def test_a_channel_revisited_every_two_seconds_fits_the_true_shift(self):
        record, silent, p25 = alternating_record()
        retunes = [(mono, centre) for _scene, mono, centre, _rate in record["tunes"]]
        record = self.write_run(record, retunes, 121.0)  # on air at scene 21 s, inside the call
        result = self.analyze(record, [silent, p25])
        self.assertAlmostEqual(self.saved()[bench.DEVICE_SHIFT], self.AHEAD_S, delta=1e-6)
        self.assertEqual([row["caught"] for row in result["calls"]["p25"]], [True], result["calls"])

    def test_no_retunes_is_refused(self):
        record, silent, p25 = alternating_record()
        record = self.write_run(record, [], 121.0)
        self.assertIn("retune", self.refused(record, [silent, p25]))

    def test_retunes_off_the_scene_are_refused(self):
        record, silent, p25 = alternating_record()
        retunes = [(mono, centre + 7 * bench.SPACING_HZ) for _scene, mono, centre, _rate in record["tunes"]]
        record = self.write_run(record, retunes, 121.0)
        self.assertIn("retune", self.refused(record, [silent, p25]))

    def test_an_alignment_another_lap_fits_as_well_is_refused(self):
        # The logcat holds only the middle of the run: shifted a lap (2 s) either way, its retunes match the scene's
        # tune log as often and as evenly, so the shift cannot be told.
        record, silent, p25 = alternating_record()
        retunes = [(mono, centre) for _scene, mono, centre, _rate in record["tunes"] if 110.0 <= mono < 130.0]
        record = self.write_run(record, retunes, 121.0)
        self.assertIn("ambiguous", self.refused(record, [silent, p25]))

    def test_too_few_matching_retunes_are_refused(self):
        record, silent, p25 = alternating_record()
        retunes = [(mono, centre) for _scene, mono, centre, _rate in record["tunes"][:2]]
        record = self.write_run(record, retunes, 121.0)
        self.assertIn("retune", self.refused(record, [silent, p25]))


class WholeSecondStamps(unittest.TestCase):
    """The recording journal and the frame log stamp whole wall-clock seconds: an event stamped s happened in
    [s, s + 1), which during the scene's catch-up can span more than the scorer's 1.5 s allowance. Both ends of that
    second go on the scene clock and the event is matched over the interval between them. Here the scene caught up 3 s
    in the host's second from 120 to 121 (retunes at host/scene 120/18 and 121/21)."""

    def setUp(self):
        self.silent = bench.Channel(0, "silent")
        self.dir = tempfile.TemporaryDirectory(prefix="whole-second-")
        self.out = self.dir.name
        Path(self.out, "dsd.log").write_text("", encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def record(self, active):
        tunes = [(0.0, 100.0, self.silent), (18.0, 120.0, active), (21.0, 121.0, self.silent), (30.0, 130.0, active)]
        return run_record(tunes)

    def test_a_recording_stamped_in_a_catch_up_second_is_matched(self):
        # Started at host 120.8 (scene 20.4), stamped 120 (scene 18): its call keys up at 20 s.
        p25 = bench.Channel(1, "p25")
        p25.starts = bench.np.array([20.0])
        record = self.record(p25)
        record.update({"scan": "frontend", "scan_variant": "typed", bench.DEVICE_SHIFT: 0.0})
        entry = {"type": "publish", "system": bench.P25_SYSTEM, "start": 120, "duration_ms": 600}
        Path(self.out, "calls.jsonl").write_text(json.dumps(entry) + "\n", encoding="utf-8")
        result = bench.score(record, [self.silent, p25], self.out)
        self.assertEqual([row["caught"] for row in result["calls"]["p25"]], [True], result["calls"])

    def test_a_frame_stamped_in_a_catch_up_second_is_matched(self):
        # A DMR frame decoded at host 120.9 (scene 20.7) and stamped 120 (scene 18): its call keys up at about 20.2 s.
        dmr = dmr_channel(1, [20.0])
        record = self.record(dmr)
        base = time.mktime((2026, 1, 1, 12, 0, 0, 0, 0, -1))
        record["wall_offset_s"] = base - 100.0  # wall clock base + 20 s is host monotonic 120 s
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(base + 20.0))
        line = f"{stamp} FRAME AMBE slot=1 data={dmr.expected[0]} err=[0] [0]\n"
        Path(self.out, "frames.log").write_text(line, encoding="ascii")
        result = bench.score(record, [self.silent, dmr], self.out)
        self.assertEqual([row["caught"] for row in result["calls"]["dmr"]], [True], result["calls"])


class KeyedAtKeyUp(unittest.TestCase):
    """A call is keyed from its key-up as air() computes it. In the default seed-1 schedule the first DMR call starts at
    15.966666666666667 s; taking the lead back off its key-up rounds just below that start, so a lookup by scheduled
    start found no call keyed at the key-up instant."""

    def setUp(self):
        self.silent = bench.Channel(0, "silent")
        self.dmr = dmr_channel(1, bench.schedule(1, bench.ACTIVE.index("dmr"), 0.0))
        self.start = float(self.dmr.starts[0])
        self.key_up = self.dmr.air(self.start)[0]

    def test_the_first_call_starts_off_the_grid(self):
        self.assertAlmostEqual(self.start, 15.966666666666667, delta=1e-12)

    def test_every_call_is_keyed_at_its_key_up(self):
        missed = [s for s in self.dmr.starts[:500] if self.dmr.call_at(self.dmr.air(float(s))[0]) != float(s)]
        self.assertEqual(missed, [])

    def test_a_key_up_at_the_skip_is_keyed_after_landing(self):
        landed = self.start + 0.1  # in the call's silent lead
        visits = [(self.silent, 0.0, landed), (self.dmr, landed, self.key_up), (self.silent, self.key_up, 40.0)]
        self.assertEqual(bench.skips_on_air([(self.key_up, float(self.dmr.frequency))], visits), [0, 1])

    def test_the_key_down_stays_off_the_air(self):
        self.assertIsNone(self.dmr.call_at(self.dmr.air(self.start)[1]))


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
