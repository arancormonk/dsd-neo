#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/bench_signal_search_tcp.py's rig bookkeeping (issue #748), which ENGINE_SIGNAL_SEARCH_TETRA_RIG shares.

- A transmitter's landing is the first status of a live visit to it once the tune has landed (identifying, listening
  or held), not the tuning status the engine publishes with the hit when it queues the tune, nor an ended visit.
- A rig folder is made absolute before the child is told its paths, since the child runs inside it: a relative --out
  once sent it looking for results/results/stop.

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


if __name__ == "__main__":
    unittest.main()
