#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/bench_signal_search_tcp.py's rig bookkeeping (issue #748), which ENGINE_SIGNAL_SEARCH_TETRA_RIG shares.

- A transmitter's landing is the first status of a live visit to it once the tune has landed (identifying, listening
  or held), not the tuning status the engine publishes with the hit when it queues the tune, nor an ended visit.
- A rig folder is made absolute before the child is told its paths, since the child runs inside it: a relative --out
  once sent it looking for results/results/stop.
- A folder that already holds a rig journal is refused: the rig would reopen it and count the earlier run's rows.
- The load average is optional metadata: None where the platform has none (Windows), not an AttributeError.
- A transmitter loops its content seamlessly: a block that crosses the end of the content continues from its start,
  not with noise for the rest of the block.

These run in seconds and never start a decoder or serve a scene.
"""

import importlib
import importlib.util
import json
import os
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

if importlib.util.find_spec("numpy") is None:
    # The tool needs numpy; without it this is a skip CTest reports (SKIP_RETURN_CODE 77), not a pass.
    print(f"SKIP: tools/bench_signal_search_tcp.py needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tools"))

bench = importlib.import_module("bench_signal_search_tcp")

FREQ = 450200000


def status(phase, hit_id=1, frequency=FREQ, ended=0):
    hit = {"hit_id": hit_id, "frequency_hz": frequency, "ended": ended, "confirmed": 0}
    return (json.dumps({"rig": "status", "t_ms": 0, "phase": phase, "pass": 1, "hit": hit}) + "\n").encode()


class Scene:
    """The scene time read_rig() stamps each line with."""

    sample_time = 0.0


def feed(lines):
    """Run read_rig() over (scene time, raw line) pairs; returns the transmitter it marked."""
    scene = Scene()
    tx = bench.Transmitter("tx", FREQ)

    def pipe():
        for t, raw in lines:
            scene.sample_time = t
            yield raw

    out = []
    bench.read_rig(pipe(), out, scene, [tx])
    return tx, out


class Landing(unittest.TestCase):
    def test_tuning_is_not_a_landing(self):
        tx, _ = feed([(1.0, status(2)), (1.4, status(3))])
        self.assertEqual(tx.landed, 1.4)

    def test_listening_and_held_land(self):
        for phase in (4, 5):
            tx, _ = feed([(0.5, status(2)), (2.0, status(phase))])
            self.assertEqual(tx.landed, 2.0, phase)

    def test_an_ended_visit_is_no_landing(self):
        tx, _ = feed([(1.0, status(3, ended=1)), (3.0, status(3, hit_id=2))])
        self.assertEqual(tx.landed, 3.0)

    def test_other_frequencies_and_phases_do_not_land(self):
        tx, _ = feed([(1.0, status(3, frequency=FREQ + 100000)), (2.0, status(6)), (2.5, status(1, hit_id=0))])
        self.assertIsNone(tx.landed)

    def test_a_line_left_open_by_the_decoder_still_lands(self):
        tx, out = feed([(1.0, b"Sync: +TETRA NDB " + status(3))])
        self.assertEqual(tx.landed, 1.0)
        self.assertEqual(len(out), 1)


class Folder(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="bench-search-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        cwd = os.getcwd()
        self.addCleanup(os.chdir, cwd)
        os.chdir(self.root)

    def test_a_relative_folder_is_made_absolute(self):
        work = bench.prepare_rig_folder(Path("results"))
        self.assertTrue(work.is_absolute())
        self.assertEqual(work, self.root / "results")
        self.assertTrue(work.is_dir())

    def test_the_child_gets_absolute_paths(self):
        """run_rig() with a relative folder: the child runs in it and is told where its journal and stop file are."""
        seen = {}

        class FakeScene:
            port = 1
            sample_time = 0.0
            tunes = []
            thread = unittest.mock.Mock()

            def __init__(self, *_args, **_kwargs):
                pass

            def close(self):
                pass

        class FakeProcess:
            returncode = 0
            stdout = iter(())

            def __init__(self, cmd, **kwargs):
                seen.update(kwargs)

            def poll(self):
                return 0

            def wait(self, timeout=None):
                return 0

        scan_bench = importlib.import_module("bench_scan_skip_silent")
        with unittest.mock.patch.object(scan_bench, "Scene", FakeScene), unittest.mock.patch.object(
            bench.subprocess, "Popen", FakeProcess
        ):
            bench.run_rig("rig", [], [("r", 1, 2, 1, 1, 0, 1, 1)], 48, Path("results"), 1.0)
        work = self.root / "results"
        self.assertEqual(Path(seen["cwd"]), work)
        self.assertEqual(seen["env"]["DSD_SEARCH_RIG_STORE"], str(work / "history.jsonl"))
        self.assertEqual(seen["env"]["DSD_SEARCH_RIG_STOP"], str(work / "stop"))

    def test_an_earlier_journal_is_refused(self):
        for name in (bench.RIG_JOURNAL, bench.RIG_JOURNAL + ".v2"):
            folder = self.root / f"run-{len(name)}"
            folder.mkdir()
            (folder / name).write_bytes(b"")
            with self.assertRaises(ValueError) as refused:
                bench.prepare_rig_folder(folder)
            self.assertIn(name, str(refused.exception))

    def test_other_files_are_kept(self):
        folder = self.root / "kept"
        folder.mkdir()
        (folder / "result.json").write_text("{}", encoding="utf-8")
        self.assertEqual(bench.prepare_rig_folder(folder), folder)

    def test_rig_main_refuses_an_earlier_journal(self):
        folder = self.root / "out"
        folder.mkdir()
        (folder / bench.RIG_JOURNAL).write_bytes(b"")
        args = unittest.mock.Mock(out=Path("out"))
        with unittest.mock.patch.object(bench, "run_rig") as run_rig, unittest.mock.patch("sys.stderr"):
            self.assertEqual(bench.rig_main(args), 2)
        run_rig.assert_not_called()


class SilentRng:
    """A scene noise source that adds nothing, so a block holds only the transmitter."""

    def standard_normal(self, n, dtype):
        return bench.np.zeros(n, dtype=dtype)


class Loop(unittest.TestCase):
    RATE = 1152000

    def setUp(self):
        self.scan_bench = importlib.import_module("bench_scan_skip_silent")
        np = bench.np
        # Content at the stream rate, so no resampling: unit magnitude, a different phase on every sample, ending
        # 1000 samples into a block.
        self.length = 3 * self.scan_bench.BLOCK + 1000
        phases = np.random.default_rng(1).uniform(-np.pi, np.pi, self.length)
        self.content = np.exp(1j * phases)
        self.tx = bench.Transmitter("tx", FREQ, samples=(self.content, self.RATE), snr_db=40.0)

    def block_at(self, first):
        """Scene.block() of a silent scene centred on the transmitter, @p first samples into a loop of its content."""
        np = bench.np
        scene = unittest.mock.Mock(rng=SilentRng(), channels=[self.tx], sample_index=0)
        scene.sample_time = (2 * self.length + first) / self.RATE
        raw = self.scan_bench.Scene.block(scene, FREQ, self.RATE)
        out = np.frombuffer(raw, dtype="u1").astype(np.float64) - 127.5
        return out[::2] + 1j * out[1::2]

    def test_a_block_across_the_end_continues_from_the_start(self):
        np = bench.np
        block = self.scan_bench.BLOCK
        first = 3 * block
        iq = self.block_at(first)
        # Every sample carries the transmitter (about 59 steps here), none is the scene's silence.
        self.assertGreater(float(np.min(np.abs(iq))), 30.0)
        # And in its phase: the last 1000 samples of the content, then its first block - 1000.
        expected = np.concatenate([self.content[first:], self.content[: block - (self.length - first)]])
        error = np.angle(iq * np.conj(expected))
        self.assertLess(float(np.max(np.abs(error))), 0.05)

    def test_a_block_at_the_end_starts_the_next_loop(self):
        np = bench.np
        iq = self.block_at(self.length)
        expected = self.content[: self.scan_bench.BLOCK]
        self.assertLess(float(np.max(np.abs(np.angle(iq * np.conj(expected))))), 0.05)

    def test_shorter_than_a_block(self):
        np = bench.np
        short = self.content[:1000]
        self.tx = bench.Transmitter("tx", FREQ, samples=(short, self.RATE), snr_db=40.0)
        self.length = len(short)
        iq = self.block_at(400)
        expected = short[(400 + np.arange(self.scan_bench.BLOCK)) % len(short)]
        self.assertLess(float(np.max(np.abs(np.angle(iq * np.conj(expected))))), 0.05)


class LoadAverage(unittest.TestCase):
    def test_present(self):
        with unittest.mock.patch.object(bench.os, "getloadavg", create=True, return_value=(1.0, 2.0, 3.0)):
            self.assertEqual(bench.load_average(), [1.0, 2.0, 3.0])

    def test_missing(self):
        with unittest.mock.patch.object(bench, "os") as fake_os:
            del fake_os.getloadavg
            self.assertIsNone(bench.load_average())

    def test_unavailable(self):
        with unittest.mock.patch.object(bench.os, "getloadavg", create=True, side_effect=OSError):
            self.assertIsNone(bench.load_average())


if __name__ == "__main__":
    unittest.main()
