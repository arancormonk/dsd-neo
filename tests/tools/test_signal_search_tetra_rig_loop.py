#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The TETRA search rig's transmitters loop their content seamlessly (issue #748).

tests/signal_search_tetra_rig_check.py plays each transmitter through the paced scene of
tools/bench_scan_skip_silent.py, which reads a whole block from where the transmitter's call_at() says the block starts.
A block that crosses the end of the content continues from its start (LoopedTransmitter.at_rate()), not with noise for
the rest of the block, and the block after the end starts the next loop at its first sample. The end-to-end rig checks
detections and departure times, not the samples at that boundary, so these check them.

These run in seconds and never start a decoder or serve a scene.
"""

import importlib
import importlib.util
import os
import sys
import unittest
import unittest.mock
from pathlib import Path

if importlib.util.find_spec("numpy") is None:
    # The rig needs numpy; without it this is a skip CTest reports (SKIP_RETURN_CODE 77), not a pass.
    print(f"SKIP: tests/signal_search_tetra_rig_check.py needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tests"))

rig = importlib.import_module("signal_search_tetra_rig_check")
np = rig.np

FREQ = 425000000


class SilentRng:
    """A scene noise source that adds nothing, so a block holds only the transmitter."""

    def standard_normal(self, n, dtype):
        return np.zeros(n, dtype=dtype)


class Loop(unittest.TestCase):
    RATE = rig.CAPTURE_HZ

    def transmitter(self, content):
        return rig.LoopedTransmitter(
            "tx", FREQ, None, snr_db=40.0, on_s=None, step=12500, fixture_dir=None, samples=(content, self.RATE)
        )

    def setUp(self):
        # Content at the stream rate, so no resampling: unit magnitude, a different phase on every sample, ending
        # 1000 samples into a block.
        self.length = 3 * rig.BLOCK + 1000
        phases = np.random.default_rng(1).uniform(-np.pi, np.pi, self.length)
        self.content = np.exp(1j * phases)
        self.tx = self.transmitter(self.content)

    def block_at(self, first):
        """Scene.block() of a silent scene centred on the transmitter, @p first samples into a loop of its content."""
        scene = unittest.mock.Mock(rng=SilentRng(), channels=[self.tx], sample_index=0)
        scene.sample_time = (2 * self.length + first) / self.RATE
        raw = rig.scan_bench.Scene.block(scene, FREQ, self.RATE)
        out = np.frombuffer(raw, dtype="u1").astype(np.float64) - 127.5
        return out[::2] + 1j * out[1::2]

    def assert_phases(self, iq, expected):
        self.assertLess(float(np.max(np.abs(np.angle(iq * np.conj(expected))))), 0.05)

    def test_a_block_across_the_end_continues_from_the_start(self):
        block = rig.BLOCK
        first = 3 * block
        iq = self.block_at(first)
        # Every sample carries the transmitter (about 59 steps here), none is the scene's silence.
        self.assertGreater(float(np.min(np.abs(iq))), 30.0)
        # And in its phase: the last 1000 samples of the content, then its first block - 1000.
        self.assert_phases(iq, np.concatenate([self.content[first:], self.content[: block - (self.length - first)]]))

    def test_a_block_at_the_end_starts_the_next_loop(self):
        self.assert_phases(self.block_at(self.length), self.content[: rig.BLOCK])

    def test_shorter_than_a_block(self):
        short = self.content[:1000]
        self.tx = self.transmitter(short)
        self.length = len(short)
        iq = self.block_at(400)
        self.assert_phases(iq, short[(400 + np.arange(rig.BLOCK)) % len(short)])


if __name__ == "__main__":
    unittest.main()
