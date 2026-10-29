#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The synthetic TETRA fixtures' signal model in tools/build_iq_fixtures.py (numpy).

- The TETRA modulator shapes its symbols with ETSI EN 300 392-2's root-raised-cosine (roll-off 0.35): its impulse
  response, read through the modulator's own pulse shaping, matches the closed form computed here independently.
- simulcast_two_ray() rotates its second ray at the sample rate it is given, 48 kHz by default.
- The control-block CRC-16 is computed directly, as the search over every candidate found it.
- The uplink fixture's bursts have the control and normal uplink burst layouts and carry their SSIs in the MAC vectors.
- Its sampling-phase variants move only the bursts, by their offset in samples, over the same noise.
- The stress set's 144 kHz round trip leaves an unstressed call alone and keeps a neighbour's set ratio.
"""

import math
import os
from pathlib import Path
import sys
import unittest

import numpy as np

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "tools"))

import build_iq_fixtures as fixtures  # noqa: E402

ALPHA = 0.35
HALF_SPAN_SYMBOLS = 8


def rrc_closed_form(t, alpha):
    """The root-raised-cosine pulse at t symbol periods, peak-unnormalized, with its two removable singularities."""
    if t == 0.0:
        return 1.0 - alpha + 4.0 * alpha / math.pi
    if abs(abs(4.0 * alpha * t) - 1.0) < 1e-12:
        return (alpha / math.sqrt(2.0)) * ((1.0 + 2.0 / math.pi) * math.sin(math.pi / (4.0 * alpha))
                                           + (1.0 - 2.0 / math.pi) * math.cos(math.pi / (4.0 * alpha)))
    numerator = math.sin(math.pi * t * (1.0 - alpha)) + 4.0 * alpha * t * math.cos(math.pi * t * (1.0 + alpha))
    return numerator / (math.pi * t * (1.0 - (4.0 * alpha * t) ** 2))


class TetraPulseShape(unittest.TestCase):
    def impulse_response(self, sps):
        """One unit symbol impulse, centred in a quiet stretch, through the modulator's pulse shaping."""
        half_span = HALF_SPAN_SYMBOLS * sps
        impulses = np.zeros(4 * half_span + 1, dtype=np.complex128)
        centre = 2 * half_span
        impulses[centre] = 1.0
        shaped = fixtures.tetra_pulse_shape(impulses, sps)
        self.assertEqual(len(shaped), len(impulses))
        return shaped, centre, half_span

    def test_impulse_response_is_the_rrc_closed_form(self):
        # sps 7 lands a sample on |4 alpha t| = 1 (t = 5/7 symbol), the second limit.
        for sps in (2, 3, 4, 7, 8):
            with self.subTest(sps=sps):
                shaped, centre, half_span = self.impulse_response(sps)
                for offset in range(-half_span, half_span + 1):
                    want = rrc_closed_form(offset / sps, ALPHA)
                    got = shaped[centre + offset]
                    self.assertAlmostEqual(got.real, want, delta=1e-9, msg=f"sps {sps} offset {offset}")
                    self.assertAlmostEqual(got.imag, 0.0, delta=1e-12)
                # The pulse spans 8 symbols each side and no further.
                self.assertTrue(np.all(shaped[: centre - half_span] == 0))
                self.assertTrue(np.all(shaped[centre + half_span + 1:] == 0))

    def test_it_is_not_the_raised_cosine(self):
        """The old full raised cosine (alpha 0.2) crossed zero at every other symbol; the root does not."""
        shaped, centre, _ = self.impulse_response(4)
        self.assertGreater(abs(shaped[centre + 4].real), 1e-3)

    def test_modulator_runs_the_pulse(self):
        """modulate_tetra() is its symbol impulses through tetra_pulse_shape()."""
        dibits = [0, 1, 2, 3, 3, 2, 1, 0] * 8
        sps = 4
        out = fixtures.modulate_tetra(dibits, sample_rate=72000)
        # Rebuild the impulse train the modulator makes: its fixed acquisition preamble, then the dibits.
        rng = np.random.default_rng(0x54455452)
        everything = rng.integers(0, 4, 2000).tolist() + dibits
        impulses = np.zeros(len(everything) * sps, dtype=np.complex128)
        phase = 0.0
        for symbol, dibit in enumerate(everything):
            phase += fixtures.TETRA_PHASE_STEP[dibit]
            impulses[symbol * sps] = complex(math.cos(phase), math.sin(phase))
        np.testing.assert_allclose(out, fixtures.tetra_pulse_shape(impulses, sps), rtol=0.0, atol=1e-12)


class SimulcastTwoRay(unittest.TestCase):
    def rotation_step(self, **kwargs):
        samples = np.ones(64, dtype=np.complex128)
        second = fixtures.simulcast_two_ray(samples, 0, 1.0, 3.0, 0.0, **kwargs) - samples
        steps = np.angle(second[1:] / second[:-1])
        self.assertTrue(np.allclose(steps, steps[0], rtol=0.0, atol=1e-12))
        return float(steps[0])

    def test_phase_increment_follows_the_sample_rate(self):
        self.assertAlmostEqual(self.rotation_step(sample_rate=54000), 2.0 * math.pi * 3.0 / 54000.0, delta=1e-15)

    def test_default_rate_stays_48k(self):
        self.assertAlmostEqual(self.rotation_step(), 2.0 * math.pi * 3.0 / 48000.0, delta=1e-15)
        self.assertEqual(fixtures.SAMPLE_RATE_HZ, 48000)


class TetraControlCrc(unittest.TestCase):
    def test_protect_is_the_searched_crc(self):
        """The CRC-16 computed directly is the one a search over all 65536 candidates finds first (the residue 0x1D0F)."""
        info = [1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0]
        searched = None
        for candidate in range(0x10000):
            trial = info + [(candidate >> (15 - bit)) & 1 for bit in range(16)]
            if fixtures.tetra_crc16(trial) == 0x1D0F:
                searched = trial
                break
        self.assertEqual(fixtures.tetra_crc16_protect(info), searched)


class TetraUplinkBursts(unittest.TestCase):
    """The uplink fixture's bursts: the frame layout the frame sync matches (EN 300 392-2 clause 9.4.4.2)."""

    def test_control_uplink_burst_layout(self):
        dibits = fixtures.tetra_ul_burst_dibits("cb", 2001)
        self.assertEqual(len(dibits), 103)
        self.assertEqual("".join(str(d) for d in dibits[44:59]), fixtures.TETRA_ETS_SYNC)
        self.assertEqual(dibits[:2], [3, 0])
        self.assertEqual(dibits[-2:], [3, 0])

    def test_normal_uplink_burst_layout(self):
        dibits = fixtures.tetra_ul_burst_dibits("nub", 2002)
        self.assertEqual(len(dibits), 231)
        self.assertEqual("".join(str(d) for d in dibits[110:121]), fixtures.TETRA_NTS_SYNC)

    def test_mac_vectors(self):
        """The MAC-ACCESS and MAC-DATA blocks are whole and carry the burst's SSI where tetra_ul_read() reads it."""
        access = fixtures.tetra_ul_access_bits(0x123456)
        self.assertEqual(len(access), 92)
        self.assertEqual(int("".join(str(b) for b in access[5:29]), 2), 0x123456)
        data = fixtures.tetra_ul_data_bits(0x00ABCD)
        self.assertEqual(len(data), 268)
        self.assertEqual(int("".join(str(b) for b in data[6:30]), 2), 0x00ABCD)

    def test_ssis_follow_the_table(self):
        _samples, bursts = fixtures.tetra_uplink_samples()
        self.assertEqual(len(bursts), len(fixtures.TETRA_UPLINK_BURSTS))
        for index, (kind, ssi) in enumerate(bursts):
            self.assertEqual(kind, fixtures.TETRA_UPLINK_BURSTS[index][0])
            self.assertEqual(ssi, fixtures.TETRA_UPLINK_SSI_BASE + index)


class TetraUplinkPhaseOffsets(unittest.TestCase):
    """The uplink fixture at every sampling phase (TETRA_UPLINK_PHASE_OFFSETS)."""

    def test_offsets_and_names(self):
        self.assertEqual(fixtures.TETRA_UPLINK_PHASE_OFFSETS, (1, 2, 3, 4, 5, 6, 7))
        names = fixtures.derived_fixture_names()
        for offset in fixtures.TETRA_UPLINK_PHASE_OFFSETS:
            self.assertIn(f"tetra_uplink_bursts_o{offset}_72k", names)

    def test_burst_starts(self):
        """Each burst starts its guard into its slot, offset samples later on a variant; the short-tail cut ends its tail
        after the last one."""
        sps = fixtures.TETRA_UPLINK_RATE_HZ // fixtures.TETRA_SYMBOL_RATE
        starts = fixtures.tetra_uplink_burst_starts()
        lead = int(round(fixtures.TETRA_UPLINK_LEAD_S * fixtures.TETRA_UPLINK_RATE_HZ))
        self.assertEqual(starts[0], lead + fixtures.TETRA_UPLINK_GUARD_SYMBOLS * sps)
        self.assertEqual(len(starts), len(fixtures.TETRA_UPLINK_BURSTS))
        self.assertEqual(fixtures.tetra_uplink_burst_starts(3), [start + 3 for start in starts])
        self.assertIn(fixtures.TETRA_UPLINK_SHORT_TAIL_NAME, fixtures.derived_fixture_names())
        # The first burst's energy begins where its start says (its pulse reaches 8 symbols ahead of it).
        samples, _bursts = fixtures.tetra_uplink_samples()
        noise = float(np.mean(np.abs(samples[: lead // 2]) ** 2))
        burst = float(np.mean(np.abs(samples[starts[0] : starts[0] + 100 * sps]) ** 2))
        self.assertGreater(burst, 10.0 * noise)

    def test_offset_moves_only_the_bursts(self):
        base, base_bursts = fixtures.tetra_uplink_samples()
        power = np.abs(base) ** 2
        for offset in (1, 3, 7):
            moved, bursts = fixtures.tetra_uplink_samples(offset)
            self.assertEqual(len(moved), len(base))
            self.assertEqual(bursts, base_bursts)
            # The noise is the same draw, so the difference is the bursts alone: zero wherever neither copy has a
            # burst, as in the lead-in and the tail.
            difference = moved - base
            lead = int(round(fixtures.TETRA_UPLINK_LEAD_S * fixtures.TETRA_UPLINK_RATE_HZ))
            tail = int(round(fixtures.TETRA_UPLINK_TAIL_S * fixtures.TETRA_UPLINK_RATE_HZ))
            self.assertFalse(np.any(difference[: lead // 2]))
            self.assertFalse(np.any(difference[len(base) - tail // 2 :]))
            # The bursts' energy arrives offset samples later.
            moved_power = np.abs(moved) ** 2
            lags = list(range(-8, 9))
            scores = [float(np.dot(power[8:-8], moved_power[8 + lag : len(moved_power) - 8 + lag])) for lag in lags]
            self.assertEqual(lags[int(np.argmax(scores))], offset)


class TetraStress(unittest.TestCase):
    def test_unstressed_call_passes_through(self):
        """At 0 Hz with no neighbour and no noise the 144 kHz round trip gives the call back."""
        rng = np.random.default_rng(1)
        wanted = rng.normal(0.0, 0.1, 7200) + 1j * rng.normal(0.0, 0.1, 7200)
        out, realized = fixtures.tetra_stress_samples(wanted, None, None, 0.0, 0.0, None)
        self.assertIsNone(realized)
        np.testing.assert_allclose(out, wanted, rtol=0.0, atol=1e-12)

    def test_realized_ratio_on_frequency(self):
        """A neighbour set +20 dB 25 kHz away keeps about that ratio through the decimation (it lies inside 36 kHz)."""
        rng = np.random.default_rng(2)
        wanted = fixtures.modulate_tetra(rng.integers(0, 4, 4000).tolist(), sample_rate=72000)[:14400]
        _out, realized = fixtures.tetra_stress_samples(wanted, None, 20.0, -25000.0, 0.0, None)
        self.assertAlmostEqual(realized, 20.0, delta=0.5)


if __name__ == "__main__":
    unittest.main()
