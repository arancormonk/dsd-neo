#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Design gate for the PCM noise squelch (`--squelch noise[+N]` on audio input, issue #628).

The question: on audio input, where the FM discriminator ran outside DSD-neo (a scanner's discriminator tap, an SDR
program's or rtl_fm's FM output), can a quieting statistic on the noise above the voice band open on carriers and stay
shut on noise, with a reference learned from the input itself, for every source chain such audio comes through? And
can the squelch tell a source that low-passed its audio (nothing above voice to measure) from one that did not?

What the radio squelch has and this one does not (tools/noise_squelch_model.py): the channel taps and the
discriminator's scale. The audio arrives at an unknown gain, through an unknown channel filter, de-emphasis, audio
filters and sound card, so no calibration can say what noise alone reads. Two facts make a learned reference work:

  - The discriminator sees phase only, so what noise alone puts above voice does not depend on the RF noise level: it
    belongs to the source chain (its gain, filters and channel width), the same on every channel and over time.
  - A carrier can only quiet the noise above voice. Noise alone is the loudest the band ever reads on a fixed chain.

Statistic (the radio design's combine, on a learned reference): the band B = [3.8 kHz, hi], hi = min(0.45 x the
native rate, TOP), cut into K sub-bands of about SUB Hz plus SETS - 1 staggered sets of K - 1 between them (set j
shifted by j / SETS of a sub-band), each a Butterworth band-pass of prototype order 4, and a voice band-pass
[400, 2600] Hz with four more over its parts (VOICE_PARTS_HZ). Per 40 ms window taken every 20 ms at sample-exact
boundaries it measures each band-pass's mean power p_k, the voice band's V and its parts' VP, and the input's mean
square E. Against a learned reference r_k, Q = max(Q_sum, Q_max - GUARD) over the band-passes that take part (below).

Learning (the safety model, docs/testing.md "PCM noise squelch design gate"). No single window tells noise from a
carrier, and a steady stretch can be a carrier, so the learner relies only on what a source chain allows:

  - A window joins a steady stretch when the last M windows' above-band powers A = sum p_k (k < K) sit within
    STEADY_DB of their median. The stretch's levels are the means over those M windows.
  - LEARNING (no stretch yet): the gate is closed. The first steady stretch becomes the reference: PROVISIONAL. If it
    was a carrier the reference is too low and only mutes; the first noise corrects it.
  - A later steady stretch STEP dB or more away from the current one is a transition. Both bands moving by the same
    amount (within GAIN_TOL, the voice band steady on both sides) is a gain change, a volume or AGC step: the reference
    moves by the mean step of the next GAIN_HOLD_WINDOWS windows if most of them keep noise's spectrum (SHAPE_*,
    below). Otherwise the shape changed: a carrier keyed or dropped. When the band got louder, that side is noise and
    becomes the reference if it came up within STEP of the reference; the first such transition confirms the reference
    (KNOWN).
  - Noise at a lower gain (the source turned down in noise, during a transmission or across a pause): a run more than
    STEADY_DB under the reference has noise's spectrum (SHAPE_*: the voice band and each of its parts
    (VOICE_PARTS_HZ) moved as far as the band's sub-bands, by their median, and the sub-bands not tilted, the tilt a
    line across those within TILT_TRIM_DB of that move). A stretch's runs gather that evidence at their level (one that
    moves STEADY_DB starts over), and once LOWER_HOLD_WINDOWS or more of them hold it in most windows with the voice
    band steady, moved STEADY_DB or more on average, the reference moves by their mean step. A carrier's noise falls
    toward the low voice parts and a tone or speech fills some parts and not others, so neither gathers it, and the
    evidence is kept rather than tried afresh, so a carrier that only sometimes matches noise never gathers enough.
  - A downward step (and the stale rule's) keeps the reference from before it (from before the first, on a step
    already held; the cache keeps it too) until a new one replaces it. REFUTE_MIN of the next REFUTE_RUNS runs the gate
    keeps shut against the stepped reference with a voice band clearly louder than noise's (by REFUTE_FACTOR times the
    tolerances: a carrier relaying noise that matched noise's spectrum, now carrying speech), or more than STEADY_DB
    above it without noise's spectrum (no carrier reads louder than noise at its gain), refute the steps: that
    reference comes back. The stretch steps down
    again only on REFUTED_HOLD_WINDOWS of evidence with no such modulation, doubled for each further refutation in
    it. While a step is held, only a stretch with noise's spectrum replaces the reference or is tracked.
  - A steady stretch at or louder than the reference less STEADY_DB is noise: the reference tracks it with a 1 s
    time constant, so slow drift never reads as quieting.
  - NO_BAND: a steady stretch whose voice band is steady too, with the voice band more than RATIO_MAX dB per Hz above
    the band, for NO_BAND_S. Discriminator noise through any de-emphasis keeps the two within about 25 dB; a source
    that low-passed its audio leaves the band at its stopband or floor, 40 dB and more under. A steady stretch under
    the ratio leaves NO_BAND again. The squelch is then off (unavailable).
  - A window of exact zeros closes the gate and ends the stretch, as a restart does; nothing is learned from it.

Gate: closed while LEARNING; against the reference it opens at Q >= N and closes under max(N - 3, 1.5) dB.

Source chains (what reaches DSD-neo as int16): SDR-program taps (a FIR channel filter of 12.5, 16 or 25 kHz at the
program's demod rate, de-emphasis none/50/75/750 us, resampled to the output rate), rtl_fm (boxcar decimation, no
de-emphasis or `-E deemp`), a hardware scanner's tap (a +/-7.5 kHz IF filter, AC coupling, a sound card's anti-alias
filter and floor), floors and above-band tones added after the source's filters, and sources that low-passed their
audio (steep FIRs at 3.0, 3.4 and 4.0 kHz, one then amplified until it clips, and a gentle Butterworth whose verdict
may go either way). Rates under 48 kHz that divide it (8 to 24 kHz) reach the monitor through the production staging
interpolator (src/dsp/pcm_input_staging.c, dsd_resampler_design(): 16 taps a phase, Hamming-windowed sinc, cutoff
0.45 / L); others (44.1 and 96 kHz) run at their own rate.

Gate per chain (MARGIN_DB and FALSE_OPEN_TARGET as tools/noise_squelch_model.py):
  - With a band: a reference within LEARN_TARGET_S of noise; noise reaches N = 3 in at most FALSE_OPEN_TARGET of
    evaluations once learned; wanted modulation (the radio gate's tone grid at the source's rated deviation, centred
    and at its Carson edges, and real NFM speech remodulated at high CNR) keeps its 1st-percentile Q >= 30 + 6 dB
    where the chain's floor leaves that much room; quieting rises with CNR and holds within LEVEL_TOL_DB across
    volumes in the unclipped range; and every scenario keeps each false open on noise to FALSE_EVENT_S, opens on
    every carrier heard after the first noise, and never reaches NO_BAND on noise.
  - Low-passed: NO_BAND within NO_BAND_TARGET_S of noise, the gate shut on noise until then.

Scenarios: keyed traffic (speech, a dead carrier, weak carriers), starting mid-carrier, a continuous carrier, volume
steps down and up in noise and under a carrier, across a pause and in a carrier's last 60 ms, a 3 dB step at N = 3, a
low-deviation tone fading through 30, 20 and 12 dB CNR, a carrier relaying noise and then speech, a post-demod audio
AGC, slow gain drift, clipping, impulses, an above-band tone, a neighbour appearing and vanishing inside a wide source
filter, digital-silence pauses, and a scan between two passbands with the per-passband reference cache.

Candidates: the band (TOP, SUB, GUARD, SETS) with the default learner, then the learner's parameters (M, STEP,
GAIN_TOL, RATIO_MAX, NO_BAND_S) on the chosen band (--grid). The chosen design (src/dsp/pcm_noise_squelch.c) is
top6.5:s200:g6:x3: a 6.5 kHz top keeps the band inside a 12.5 kHz source's channel (a higher top reaches past a narrow
source's edge, where a carrier quiets the noise to nothing and a dead carrier reads far more quieting than a modulated
one); 200 Hz sub-bands in three staggered sets give a strong tone's lines room to miss one band-pass on 16 and 25 kHz
sources at 5 kHz deviation, which 300 Hz ones do not (31.8 dB); a 6 dB guard keeps noise at N = 3 under 1e-4 with
that many band-passes, which 4 and 5 dB do not. docs/testing.md "PCM noise squelch design gate" has the figures.

Usage:
    python3 tools/pcm_noise_squelch_model.py [--quick] [--out DIR] [--jobs N] [--only TEXT] [--variants LIST]
                                             [--grid LABEL]

The full run takes about 25 minutes on 6 workers (run it under a memory cap: each worker holds a few hundred MB);
--quick (30 s of noise, a 100 Hz tone grid) about 3 minutes. Offline; CI runs only its report test.
"""

from __future__ import annotations

import argparse
import dataclasses
import itertools
import json
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from scipy import signal as sps

sys.path.insert(0, str(Path(__file__).resolve().parent))
import squelch_model as sm  # noqa: E402

BAND_LO_HZ = 3800.0
MIN_BAND_HZ = 1200.0
SUB_BAND_HZ = 300.0
MAX_SUB_BANDS = 15
NYQUIST_FRACTION = 0.45
ORDER = 4
GUARD_DB = 4.0
VOICE_LO_HZ = 400.0
VOICE_HI_HZ = 2600.0
HALF_PER_S = 50  # 20 ms halves; a window is two of them
MONITOR_HZ = 48000
STAGING_CAP = 6  # DSD_OPTS_INPUT_UPSAMPLE_STAGING_CAP
TAPS_PER_PHASE = 16  # kDefaultTapsPerPhase (src/dsp/resampler.cpp)
MIN_POWER = 1e-30
Q_CAP_DB = 200.0
STEADY_DB = 1.5
# A quieter stretch is noise at a lower gain (the source turned down in noise, during a transmission or across a pause)
# only with noise's spectrum against the reference: its voice-to-band ratio within SHAPE_TOL_DB (noise at another gain
# keeps it within +/-1 dB, a dead carrier reads 6.5-11.5 dB lower, speech and tones swing or sit higher), each part of
# the voice band (VOICE_PARTS_HZ) moved within SHAPE_PART_TOL_DB as far as the band (a quieted carrier's noise falls
# toward the low parts, 14 dB and more at 400-800 Hz; a tone or speech fills some parts and not others), and its
# sub-bands under the reference tilted by at most SHAPE_TILT_DB end to end. The band's move is its sub-bands' median
# and their spread is not asked: a spur added after the source's volume stands out of its sub-bands once the noise is
# turned down. A gain step (a transition from noise)
# is taken after GAIN_HOLD_WINDOWS, any other after LOWER_HOLD_WINDOWS, most of them with that spectrum and the voice
# band steady: speech does not keep it.
SHAPE_TOL_DB = 1.5
SHAPE_TILT_DB = 2.5
TILT_TRIM_DB = 3.0
VOICE_PARTS_HZ = ((400.0, 800.0), (800.0, 1300.0), (1300.0, 1900.0), (1900.0, 2600.0))
SHAPE_PART_TOL_DB = 2.5
LOWER_HOLD_WINDOWS = 20
# A downward step is refuted (undone) by REFUTE_MIN of the last REFUTE_RUNS runs at or above the stepped reference whose
# voice band is clearly louder than noise's (by more than REFUTE_FACTOR times the shape tolerances), or that read more
# than STEADY_DB above it without noise's spectrum. After a refutation
# the stretch steps down again only on REFUTED_HOLD_WINDOWS runs of evidence with no such run among them, doubled for
# each further refutation in the stretch.
REFUTE_RUNS = 10
REFUTE_MIN = 8
REFUTE_FACTOR = 2.0
REFUTED_HOLD_WINDOWS = 30
TRACK_S = 1.0
N_MIN_DB = 3
N_MAX_DB = 30
N_DEFAULT_DB = 10
CLOSE_HYSTERESIS_DB = 3.0
CLOSE_FLOOR_DB = 1.5
MARGIN_DB = 6.0
FALSE_OPEN_TARGET = 1e-4
FALSE_EVENT_S = 1.0
LEARN_TARGET_S = 0.2
NO_BAND_TARGET_S = 2.0
LEVEL_TOL_DB = 0.2
DROPOUT_TARGET = 0.01
STRONG_CNR_DB = 100.0
# A floor this loud leaves the band too little room for the wanted-modulation criterion; such chains report it.
HEADROOM_FLOOR_DBFS = -80.0
# A dead carrier and a modulated one at the same CNR read within this much of each other (CNR_AGREE_DB range).
AGREE_DB = 3.0
CNR_AGREE_DB = (6.0, 20.0)
# Windows this long after a segment starts straddle the change (a 40 ms window and a hop): not scored.
SETTLE_SCORE_S = 0.06
# Scenarios whose dropouts are physics, not the learner: reported, not gated.
DIAGNOSTIC_DROPOUTS = ("agc", "impulses", "clip")
# A carrier that relays noise at the source's own noise level reads as noise turned down; the speech after it plays
# once it refutes the step (REFUTE_RUNS), so the scenario bounds that speech's dropout instead of DROPOUT_TARGET.
RELAY_DROPOUT_TARGET = 0.15
RELAY_RMS = 0.35  # a relayed segment's modulation: band-limited noise (300-3000 Hz) at this RMS
# Scenarios whose false opens are bounded by the stale-quieting rule rather than FALSE_EVENT_S.
STALE_BOUNDED = ("clip",)
# Band candidates: (top Hz or None for 0.45 x the native rate, sub-band Hz, guard dB). The radio design's 300 Hz
# sub-bands with a 4 dB guard at every top, then narrower sub-bands and a wider guard at the tops that keep the band
# inside a 12.5 kHz source's channel.
BAND_VARIANTS = (
    (6500.0, 200.0, 6.0, 3),
    (6000.0, 300.0, 4.0),
    (6500.0, 300.0, 4.0),
    (7000.0, 300.0, 4.0),
    (8000.0, 300.0, 4.0),
    (10000.0, 300.0, 4.0),
    (None, 300.0, 4.0),
    (6000.0, 250.0, 4.0),
    (6500.0, 250.0, 4.0),
    (7000.0, 250.0, 4.0),
    (6500.0, 250.0, 5.0),
    (6500.0, 200.0, 5.0),
)
# Samples at full scale (reported per window; the learner does not use them).
CLIP_LEVEL = 32767.0
# The dead-carrier and modulated-carrier agreement holds for sources whose channel reaches the band's top; a narrower
# channel's edge sits inside the band, where a carrier quiets the noise past the edge to nothing.
AGREE_MIN_WIDTH_HZ = 12500.0
DEFAULT_PARAMS = {"m": 4, "step": 4.0, "gain": 1.5, "ratio": 30.0, "no_band_s": 1.0}
# A quieter step that looks like a gain change is confirmed only after this many windows of it, most with noise's
# spectrum and the voice band steady: a carrier keyed whose speech happens to drop the voice band by the same amount
# does not hold that.
GAIN_HOLD_WINDOWS = 10
# A stretch at a new level acts only once it held for this many steady windows in a row: an impulse or two cannot
# make one.
CONFIRM_WINDOWS = 3
GAIN_STEADY_FRACTION = 0.75
# Stale quieting: the gate open this long on one steady stretch whose voice band held stationary (no speech) in at
# least STALE_STEADY_FRACTION of its windows reads a level that dropped, not a carrier (the source's volume lowered out
# of clipping, its audio low-pass switched on): the stretch becomes the reference, as a held step. A dead or steady-tone
# carrier is muted after it too, until the speech it carries refutes the step.
STALE_S = 5.0
STALE_STEADY_FRACTION = 0.9
GRID = {
    "m": (3, 4, 6),
    "step": (3.0, 4.0, 6.0),
    "gain": (1.0, 1.5, 2.0),
    "ratio": (25.0, 30.0, 35.0),
    "no_band_s": (0.5, 1.0),
}
TONES_HZ = {
    "full": (*(float(f) for f in range(300, 800, 5)), *(float(f) for f in range(800, 3001, 25))),
    "quick": tuple(float(f) for f in range(300, 3001, 100)),
}
NOISE_SECONDS = {"full": 600.0, "quick": 30.0}
NOISE_CHUNK_S = 20.0
# Wanted-modulation segments rendered (and measured) together, after a lead of noise the reference is learned from;
# rendering them in chunks keeps a source's memory bounded.
WANTED_CHUNK = 24
WANTED_LEAD_S = 0.5
# rtl_fm decimates by boxcar averaging; the model runs it at this multiple of the output rate (its own capture rate is
# higher still, but the boxcar's response near the band is what matters).
RTLFM_OVERSAMPLE = 4
TONE_SECONDS = {"full": 0.5, "quick": 0.15}
SPEECH_SECONDS = {"full": 4.0, "quick": 1.0}
CNRS_DB = (-3.0, 0.0, 3.0, 6.0, 10.0, 15.0, 20.0, 30.0)
VOLUMES_DBFS = (-35.0, -20.0, -14.0)
NOISE_DBFS = -20.0
SETTLE_S = 0.03


# --------------------------------------------------------------------------------------------- the band


def band_edges(native_hz: int, top_hz: float | None) -> tuple[float, float] | None:
    """(lo, hi) of the measurement band for a source at native_hz, or None when less than MIN_BAND_HZ fits."""
    hi = NYQUIST_FRACTION * native_hz
    if top_hz is not None:
        hi = min(hi, top_hz)
    if hi - BAND_LO_HZ < MIN_BAND_HZ:
        return None
    return BAND_LO_HZ, hi


def sub_band_count(lo: float, hi: float, sub_hz: float = SUB_BAND_HZ) -> int:
    return max(1, min(MAX_SUB_BANDS, int((hi - lo) // sub_hz)))


def staging_factor(native_hz: int) -> int:
    """dsd_opts_input_upsample_factor(): the staged interpolation from native_hz to the monitor's 48 kHz, 1 when the
    native rate runs as it is."""
    if native_hz < MONITOR_HZ and MONITOR_HZ % native_hz == 0 and MONITOR_HZ // native_hz <= STAGING_CAP:
        return MONITOR_HZ // native_hz
    return 1


def staging_taps(factor: int) -> np.ndarray:
    """dsd_resampler_design(L = factor, M = 1) as one FIR at the output rate (resampler_design_taps())."""
    total = TAPS_PER_PHASE * factor
    mid = (total - 1) // 2
    fc = NYQUIST_FRACTION / factor
    n = np.arange(total)
    w = 0.54 - 0.46 * np.cos(2.0 * math.pi * n / (total - 1))
    h = 2.0 * fc * np.sinc(2.0 * fc * (n - mid))
    return (h * w / np.sum(h * w) * factor).astype(np.float32)


def stage_up(x: np.ndarray, factor: int) -> np.ndarray:
    """The staged interpolation: zero-stuffing by factor through staging_taps(), primed with the first sample."""
    if factor <= 1:
        return x.astype(np.float64)
    taps = staging_taps(factor)
    prime = np.full(len(taps) // factor + 1, x[0] if len(x) else 0.0)
    up = np.zeros((len(prime) + len(x)) * factor, dtype=np.float32)
    up[::factor] = np.concatenate((prime, x)).astype(np.float32)
    y = sps.lfilter(taps, 1.0, up)
    return y[len(prime) * factor :].astype(np.float64)


class SquelchPlan:
    """The band-passes for one source rate and band variant (top, sub-band width, guard)."""

    def __init__(
        self,
        native_hz: int,
        top_hz: float | None,
        sub_hz: float = SUB_BAND_HZ,
        guard_db: float = GUARD_DB,
        sets: int = 2,
    ):
        self.native = native_hz
        self.guard = guard_db
        self.label = f"top{'nyq' if top_hz is None else f'{top_hz / 1000.0:g}'}:s{sub_hz:.0f}:g{guard_db:g}"
        if sets != 2:
            self.label += f":x{sets}"
        self.fs = MONITOR_HZ if staging_factor(native_hz) > 1 else native_hz
        self.band = band_edges(native_hz, top_hz)
        self.top = top_hz
        if self.band is None:
            return
        lo, hi = self.band
        self.k = sub_band_count(lo, hi, sub_hz)
        step = (hi - lo) / self.k
        edges = [(lo + i * step, lo + (i + 1) * step) for i in range(self.k)]
        # Staggered sets: sets - 1 more, each shifted by j / sets of a sub-band, so a line on a boundary of one set sits
        # inside a band-pass of another.
        for j in range(1, sets):
            edges += [(lo + (i + j / sets) * step, lo + (i + 1 + j / sets) * step) for i in range(self.k - 1)]
        self.sos = [sps.butter(ORDER, e, btype="bandpass", fs=self.fs, output="sos") for e in edges]
        self.step = step
        self.bw_db = 10.0 * math.log10(step)
        self.voice_bw_db = 10.0 * math.log10(VOICE_HI_HZ - VOICE_LO_HZ)
        self.voice = sps.butter(ORDER, [VOICE_LO_HZ, VOICE_HI_HZ], btype="bandpass", fs=self.fs, output="sos")
        self.voice_parts = [sps.butter(ORDER, e, btype="bandpass", fs=self.fs, output="sos") for e in VOICE_PARTS_HZ]
        # Per Hz: voice over band = (V / A) x (|B| / |voice|).
        self.ratio_offset_db = 10.0 * math.log10((hi - lo) / (VOICE_HI_HZ - VOICE_LO_HZ))


def half_bounds(fs: int, count: int) -> np.ndarray:
    k = np.arange(0, count * HALF_PER_S // fs + 2, dtype=np.int64)
    b = (k * fs) // HALF_PER_S
    return b[b <= count]


def measure(plan: SquelchPlan, y: np.ndarray) -> dict:
    """Per 40 ms window (hop 20 ms): band powers P (bands x W), voice V, the voice band's parts VP (parts x W), mean
    square E, and each window's end sample."""
    b = half_bounds(plan.fs, len(y))

    def halves(x):
        c = np.concatenate(([0.0], np.cumsum(x.astype(np.float64) ** 2)))
        return c[b[1:]] - c[b[:-1]]

    n = np.diff(b).astype(np.float64)
    nw = n[1:] + n[:-1]

    def win(h):
        return (h[..., 1:] + h[..., :-1]) / nw

    p = np.vstack([win(halves(sps.sosfilt(s, y))) for s in plan.sos])
    c = np.concatenate(([0], np.cumsum(np.abs(y) >= CLIP_LEVEL)))
    clipped = c[b[1:]] - c[b[:-1]]
    return {
        "P": p,
        "V": win(halves(sps.sosfilt(plan.voice, y))),
        "VP": np.vstack([win(halves(sps.sosfilt(s, y))) for s in plan.voice_parts]),
        "E": win(halves(y)),
        "C": (clipped[1:] + clipped[:-1]) / nw,
        "end": b[2:],
    }


# --------------------------------------------------------------------------------------------- the learner


LEARNING, PROVISIONAL, KNOWN, NO_BAND = 0, 1, 2, 3
STATE_NAMES = ("learning", "provisional", "known", "no band")


def close_db(n_db: float) -> float:
    return max(n_db - CLOSE_HYSTERESIS_DB, CLOSE_FLOOR_DB)


def noise_step(
    plan: SquelchPlan,
    sp: np.ndarray,
    sv: float,
    svp: np.ndarray,
    ref: np.ndarray,
    v_ref: float,
    vp_ref: np.ndarray,
    part,
) -> float | None:
    """The step (dB) by which a stretch (sp, sv, svp) sits under the reference when it has noise's spectrum against it
    (SHAPE_*), else None. The band's step is its participating sub-bands' median move, which a spur in one or two of
    them does not shift; the voice band and each of its parts must move as far, and the sub-bands must not tilt. Noise
    at another gain moves everything alike; a carrier's noise falls toward the low voice parts and rises toward the
    band's top, and a tone or speech fills some parts and not others."""
    k = plan.k
    sel = np.ones(k, dtype=bool) if part is None else np.asarray(part[:k], dtype=bool)
    if np.count_nonzero(sel) < 3:
        return None
    d = 10.0 * np.log10(np.maximum(sp[:k][sel], MIN_POWER) / np.maximum(ref[:k][sel], MIN_POWER))
    step = float(np.median(d))
    if abs((sv - v_ref) - step) > SHAPE_TOL_DB:
        return None
    if float(np.max(np.abs((svp - vp_ref) - step))) > SHAPE_PART_TOL_DB:
        return None
    # The tilt is a least-squares line across the sub-bands within TILT_TRIM_DB of the median move: a spur in one or two
    # of them, at an edge as anywhere, is left out, while a carrier's noise rises smoothly and is not.
    x = np.flatnonzero(sel).astype(float)
    keep = np.abs(d - step) <= TILT_TRIM_DB
    if np.count_nonzero(keep) >= 3:
        x, d = x[keep], d[keep]
    xm = x - np.mean(x)
    tilt = float(np.sum(xm * (d - np.mean(d))) / np.sum(xm * xm)) * (k - 1)
    return step if abs(tilt) <= SHAPE_TILT_DB else None


def voice_louder(
    plan: SquelchPlan,
    sp: np.ndarray,
    sv: float,
    svp: np.ndarray,
    ref: np.ndarray,
    v_ref: float,
    vp_ref: np.ndarray,
    part,
) -> bool:
    """Whether a stretch's voice band is clearly louder than noise's against the reference (modulation: speech or a
    tone): the voice band above the band's move by more than REFUTE_FACTOR x SHAPE_TOL_DB, or a part by more than
    REFUTE_FACTOR x SHAPE_PART_TOL_DB. Speech misses by 5-20 dB; noise at another gain never by that much, whatever its
    tilt reads on a narrow band, and a carrier's own noise reads lower, not louder."""
    k = plan.k
    sel = np.ones(k, dtype=bool) if part is None else np.asarray(part[:k], dtype=bool)
    if np.count_nonzero(sel) < 3:
        return False
    d = 10.0 * np.log10(np.maximum(sp[:k][sel], MIN_POWER) / np.maximum(ref[:k][sel], MIN_POWER))
    step = float(np.median(d))
    dv = (sv - v_ref) - step
    dp = (svp - vp_ref) - step
    return dv > REFUTE_FACTOR * SHAPE_TOL_DB or float(np.max(dp)) > REFUTE_FACTOR * SHAPE_PART_TOL_DB


def participating(plan: SquelchPlan, p: np.ndarray, v_db: float, ratio_max: float) -> np.ndarray:
    """Which band-passes carry discriminator noise as measured by p (powers) against the voice band's v_db: a sub-band
    whose power per Hz sits more than ratio_max under the voice band's holds only a stopband or a floor. A staggered
    band-pass takes part when both sub-bands it straddles do."""
    k = plan.k
    per_hz = 10.0 * np.log10(np.maximum(p, MIN_POWER)) - plan.bw_db
    ok = (v_db - plan.voice_bw_db) - per_hz[:k] <= ratio_max
    stag = ok[:-1] & ok[1:] if k > 1 else np.zeros(0, dtype=bool)
    sets = 1 + (len(plan.sos) - k) // max(1, k - 1) if k > 1 else 1
    return np.concatenate((ok, *([stag] * (sets - 1))))


def usable_hz(plan: SquelchPlan, part: np.ndarray) -> float:
    return float(np.sum(part[: plan.k])) * plan.step


def window_q(plan: SquelchPlan, ref: np.ndarray, p: np.ndarray, part: np.ndarray | None = None) -> np.ndarray:
    """Q per window (columns of p) against the reference ref (one per band-pass), over the participating band-passes
    (all when part is None)."""
    k = plan.k
    if part is None:
        part = np.ones(len(ref), dtype=bool)
    prim = part[:k]
    if not np.any(prim):
        return np.zeros(p.shape[1])
    qk = 10.0 * np.log10(ref[part][:, None] / np.maximum(p[part], MIN_POWER))
    qs = 10.0 * np.log10(np.sum(ref[:k][prim]) / np.maximum(np.sum(p[:k][prim], axis=0), MIN_POWER))
    return np.clip(np.maximum(qs, np.max(qk, axis=0) - plan.guard), -Q_CAP_DB, Q_CAP_DB)


def run_learner(plan: SquelchPlan, w: dict, params: dict, n_db: float, keys=None, restarts=None, init=None) -> dict:
    """The learner and gate over measured windows w. keys (one per window, optional) names the source context each
    window belongs to (the per-passband cache); restarts (a set of window indices) restart the windows there. init
    seeds (state, ref, v_ref, vp_ref). Returns per-window state, gate and Q, and the final (state, ref, v_ref,
    vp_ref)."""
    p_all, v_all, vp_all, e_all = w["P"], w["V"], w["VP"], w["E"]
    nw = p_all.shape[1]
    k = plan.k
    m = int(params["m"])
    step_db = float(params["step"])
    gain_tol = float(params["gain"])
    ratio_max = float(params["ratio"])
    no_band_windows = int(round(float(params["no_band_s"]) * HALF_PER_S))
    stale_windows = int(round(STALE_S * HALF_PER_S))
    alpha = 1.0 / (TRACK_S * HALF_PER_S)
    a_db = 10.0 * np.log10(np.maximum(np.sum(p_all[:k], axis=0), MIN_POWER))
    v_db = 10.0 * np.log10(np.maximum(v_all, MIN_POWER))
    silent = e_all <= MIN_POWER

    def total_db(x):
        return 10.0 * math.log10(max(float(np.sum(x[:k])), MIN_POWER))

    if init is None:
        state, ref, v_ref, vp_ref = LEARNING, None, 0.0, None
    else:
        state, ref, v_ref = init[0], None if init[1] is None else init[1].copy(), init[2]
        vp_ref = None if init[3] is None else init[3].copy()
    part = None if ref is None else participating(plan, ref, v_ref, ratio_max)
    # The reference before the last downward step, held until a new reference replaces it or a carrier at the stepped
    # reference refutes the step (it then comes back).
    prior = None
    cache: dict = {}
    key = None if keys is None else keys[0]
    # The steady stretch: its anchor level (dB), its voice-band power sum and count, whether it sat at the reference
    # when it began, and a pending gain step (dB) with its windows and voice-steady count.
    stretch = None
    run = 0  # windows since the last silent window or restart
    nb = 0
    cand_a, cand_n = None, 0  # a new level being confirmed
    exit_n = 0  # steady windows in a row that show a band, in NO_BAND
    gate = False
    out_state = np.zeros(nw, dtype=np.int8)
    out_gate = np.zeros(nw, dtype=bool)
    out_q = np.full(nw, np.nan)
    for i in range(nw):
        if keys is not None and keys[i] != key:
            if state != LEARNING:
                held_ref = None if ref is None else ref.copy()
                held_prior = None if prior is None else (prior[0].copy(), prior[1], prior[2].copy())
                cache[key] = (state, held_ref, v_ref, None if vp_ref is None else vp_ref.copy(), held_prior)
            key = keys[i]
            state, ref, v_ref, vp_ref, prior = cache.get(key, (LEARNING, None, 0.0, None, None))
            ref = None if ref is None else ref.copy()
            vp_ref = None if vp_ref is None else vp_ref.copy()
            prior = None if prior is None else (prior[0].copy(), prior[1], prior[2].copy())
            part = None if ref is None else participating(plan, ref, v_ref, ratio_max)
            stretch, run, nb, gate = None, 0, 0, False
            cand_a, cand_n, exit_n = None, 0, 0
        if restarts is not None and i in restarts:
            stretch, run, nb, gate = None, 0, 0, False
            cand_a, cand_n, exit_n = None, 0, 0
        if silent[i]:
            # A gap ends the stretch, as a restart does: the next transmission inherits no open time or pending step.
            run, nb, gate = 0, 0, False
            cand_n, exit_n = 0, 0
            stretch = None
            out_state[i] = state
            continue
        run += 1
        if run >= m:
            lo = i - m + 1
            sp = np.mean(p_all[:, lo : i + 1], axis=1)
            sa = total_db(sp)
            sv = 10.0 * math.log10(max(float(np.mean(v_all[lo : i + 1])), MIN_POWER))
            svp = 10.0 * np.log10(np.maximum(np.mean(vp_all[:, lo : i + 1], axis=1), MIN_POWER))
            seg_v = v_db[lo : i + 1]
            v_steady = bool(np.max(np.abs(seg_v - np.median(seg_v))) <= STEADY_DB)
            ra = None if ref is None else total_db(ref)
            # No-band evidence: at the learned ceiling (or before anything was learned), the voice band stationary
            # and too little of the band carrying anything within ratio_max of it per Hz.
            thin = usable_hz(plan, participating(plan, sp, sv, ratio_max)) < MIN_BAND_HZ
            at_ceiling = ref is None or sa >= ra - step_db
            nb = nb + 1 if (v_steady and thin and at_ceiling) else 0
            if nb >= no_band_windows:
                state = NO_BAND
            seg_a = a_db[lo : i + 1]
            steady = bool(np.max(np.abs(seg_a - np.median(seg_a))) <= STEADY_DB)
            # Leaving NO_BAND: a band (and a stationary voice band) for no_band_s in a row.
            exit_n = exit_n + 1 if (state == NO_BAND and steady and v_steady and not thin) else 0
            if exit_n >= no_band_windows:
                ref, v_ref, vp_ref, state, prior = sp.copy(), sv, svp.copy(), PROVISIONAL, None
                stretch = None
                cand_a, cand_n, exit_n = sa, CONFIRM_WINDOWS, 0
            new = steady and (stretch is None or abs(sa - stretch["a"]) >= step_db)
            if new:
                if cand_a is not None and abs(sa - cand_a) < step_db:
                    cand_n += 1
                else:
                    cand_a, cand_n = sa, 1
            else:
                cand_a, cand_n = None, 0
            if steady and not (new and cand_n < CONFIRM_WINDOWS):
                # The run against the reference: noise's spectrum and its step, or not.
                nstep = None
                speech = False
                if state in (PROVISIONAL, KNOWN):
                    nstep = noise_step(plan, sp, sv, svp, ref, v_ref, vp_ref, part)
                    speech = voice_louder(plan, sp, sv, svp, ref, v_ref, vp_ref, part)
                shaped = nstep is not None
                if new:
                    pending = None
                    cand_a, cand_n = None, 0
                    if state == LEARNING:
                        ref, v_ref, vp_ref, state, prior = sp.copy(), sv, svp.copy(), PROVISIONAL, None
                    elif state == NO_BAND:
                        pass
                    elif stretch is None:
                        if sa > total_db(ref) + step_db and (prior is None or shaped):
                            # A first stretch after a restart louder than the reference is noise; while a downward step
                            # is held, only with noise's spectrum (a carrier there refutes the step).
                            ref, v_ref, vp_ref, prior = sp.copy(), sv, svp.copy(), None
                    else:
                        da = sa - stretch["a"]
                        dv = sv - 10.0 * math.log10(max(stretch["v"] / stretch["n"], MIN_POWER))
                        gain_like = abs(da - dv) <= gain_tol
                        ra_now = total_db(ref)
                        if da > 0.0 and sa >= ra_now - step_db and (prior is None or shaped):
                            # The band got louder, up at the reference: that side is noise. While a downward step is
                            # held, only with noise's spectrum: a carrier there does not replace the reference, it
                            # refutes the step.
                            ref, v_ref, vp_ref, prior = sp.copy(), sv, svp.copy(), None
                            if not gain_like and state == PROVISIONAL:
                                state = KNOWN
                        elif da > 0.0:
                            # Louder but well under the reference: the carrier's modulation or level changed (speech
                            # after a pause, a fading carrier), and the reference stays, unless the stretch is noise
                            # come back at a lower gain (below).
                            pass
                        elif gain_like and v_steady and stretch["at_ref"] and stretch["vs"] * 2 >= stretch["n"]:
                            # Quieter, from noise, both bands by the same amount: a gain step, once it holds with
                            # noise's spectrum.
                            pending = da
                        elif state == PROVISIONAL:
                            state = KNOWN
                    at_ref = ref is not None and sa >= total_db(ref) - step_db
                    stretch = {"a": sa, "v": 0.0, "n": 0, "vs": 0, "at_ref": at_ref, "pend": pending, "pn": 0, "ps": 0,
                               "ssum": 0.0, "on": 0, "os": 0, "acc": None, "an": 0, "as_": 0, "asum": 0.0,
                               "refutes": 0, "ring": []}
                    part = None if ref is None else participating(plan, ref, v_ref, ratio_max)
                stretch["v"] += 10.0 ** (sv / 10.0)
                stretch["n"] += 1
                stretch["vs"] += 1 if v_steady else 0
                ra = None if ref is None else total_db(ref)
                # A held downward step is refuted by a carrier the gate keeps shut against the stepped reference (a run
                # less than N dB under it): REFUTE_MIN of the last REFUTE_RUNS such runs with a voice band clearly
                # louder than noise's (noise turned down keeps noise's; a carrier relaying noise that matched it carries
                # speech), or more than STEADY_DB above it without noise's spectrum (noise is the ceiling: no carrier
                # reads louder than noise at its gain, and noise turned back up keeps its spectrum). A carrier strong
                # enough to open the gate refutes nothing. The reference from before the first held step comes back,
                # and the stretch steps down again only on a longer hold of evidence.
                if prior is not None and ra is not None and sa > ra - n_db:
                    ring = stretch["ring"]
                    ring.append(speech or (not shaped and sa > ra + STEADY_DB))
                    del ring[:-REFUTE_RUNS]
                    if len(ring) == REFUTE_RUNS and sum(ring) >= REFUTE_MIN:
                        ref, v_ref, vp_ref = prior[0].copy(), prior[1], prior[2].copy()
                        prior = None
                        stretch.update(ring=[], pend=None, acc=None)
                        stretch["refutes"] += 1
                        stretch["at_ref"] = sa >= total_db(ref) - step_db
                        part = participating(plan, ref, v_ref, ratio_max)
                        ra = total_db(ref)
                        shaped = False
                        speech = True
                # Noise come back at a lower gain (the source turned down in noise, during a transmission or across a
                # pause): the stretch's runs under the reference's tracking range gather evidence at their level, and
                # the step is taken once LOWER_HOLD_WINDOWS or more of them hold most with noise's spectrum and the
                # voice band steady, moved STEADY_DB or more on average. The evidence is kept, not tried afresh: a
                # carrier that only sometimes matches noise never gathers enough. A level that moves STEADY_DB starts
                # over.
                # After a refutation, modulation in the evidence starts it over and the hold doubles each time.
                refutes = stretch["refutes"]
                hold = LOWER_HOLD_WINDOWS if not refutes else REFUTED_HOLD_WINDOWS << min(refutes - 1, 3)
                if stretch["pend"] is None and refutes and speech:
                    stretch["acc"] = None
                elif stretch["pend"] is None and ra is not None and sa < ra - STEADY_DB:
                    if stretch["acc"] is None or abs(sa - stretch["acc"]) > STEADY_DB:
                        stretch.update(acc=sa, an=0, as_=0, asum=0.0)
                    stretch["an"] += 1
                    if v_steady and shaped:
                        stretch["as_"] += 1
                        stretch["asum"] += nstep
                    mean = stretch["asum"] / stretch["as_"] if stretch["as_"] else 0.0
                    if (
                        stretch["an"] >= hold
                        and stretch["as_"] >= GAIN_STEADY_FRACTION * stretch["an"]
                        and mean <= -STEADY_DB
                    ):
                        # A step on a held one keeps the reference from before the first.
                        prior = prior if prior is not None else (ref.copy(), v_ref, vp_ref.copy())
                        ref = ref * 10.0 ** (mean / 10.0)
                        v_ref += mean
                        vp_ref = vp_ref + mean
                        stretch.update(at_ref=True, ring=[], acc=None)
                        part = participating(plan, ref, v_ref, ratio_max)
                        if state == PROVISIONAL:
                            # Noise after a carrier, at its lower gain: the carrier keyed is confirmed.
                            state = KNOWN
                # A gain step straight out of noise, from its transition: taken after GAIN_HOLD_WINDOWS by the mean step
                # of its runs if most held noise's spectrum with the voice band steady; otherwise a carrier keyed.
                if stretch["pend"] is not None:
                    stretch["pn"] += 1
                    if v_steady and shaped:
                        stretch["ps"] += 1
                        stretch["ssum"] += nstep
                    if stretch["pn"] >= GAIN_HOLD_WINDOWS:
                        mean = stretch["ssum"] / stretch["ps"] if stretch["ps"] else 0.0
                        if stretch["ps"] >= GAIN_STEADY_FRACTION * stretch["pn"] and mean <= -STEADY_DB:
                            # The reference moves by the mean step; the one it held is kept in case a carrier refutes
                            # the step (the one from before the first, on a step already held).
                            prior = prior if prior is not None else (ref.copy(), v_ref, vp_ref.copy())
                            ref = ref * 10.0 ** (mean / 10.0)
                            v_ref += mean
                            vp_ref = vp_ref + mean
                            stretch.update(at_ref=True, ring=[], acc=None)
                            part = participating(plan, ref, v_ref, ratio_max)
                        elif state == PROVISIONAL:
                            state = KNOWN
                        stretch["pend"] = None
                # Stale quieting: open on this stretch for STALE_S with the voice band stationary throughout.
                if gate and state in (PROVISIONAL, KNOWN) and stretch is not None:
                    stretch["on"] += 1
                    stretch["os"] += 1 if v_steady else 0
                    if stretch["on"] >= stale_windows and stretch["os"] >= STALE_STEADY_FRACTION * stretch["on"]:
                        # The stretch becomes the reference as a held step: a carrier taken so is refuted, and the
                        # reference before it comes back, once it shows modulation.
                        prior = prior if prior is not None else (ref.copy(), v_ref, vp_ref.copy())
                        ref, v_ref, vp_ref = sp.copy(), sv, svp.copy()
                        stretch["on"] = stretch["os"] = 0
                        stretch["at_ref"] = True
                        stretch["ring"] = []
                        part = participating(plan, ref, v_ref, ratio_max)
                # A run at or above the reference less STEADY_DB tracks it (slow drift); while a downward step is held,
                # only one with noise's spectrum.
                if state in (PROVISIONAL, KNOWN) and stretch["pend"] is None and (prior is None or shaped):
                    ra = total_db(ref)
                    if sa >= ra - STEADY_DB:
                        ref = ref + alpha * (sp - ref)
                        v_ref += alpha * (sv - v_ref)
                        vp_ref = vp_ref + alpha * (svp - vp_ref)
                        stretch["a"] += alpha * (sa - stretch["a"])
                        part = participating(plan, ref, v_ref, ratio_max)
        if state in (PROVISIONAL, KNOWN) and usable_hz(plan, part) < MIN_BAND_HZ:
            # A reference with too little usable band (a source low-passed into the band's first sub-bands) does not
            # gate: shut, as while learning, until the no-band evidence arrives.
            gate = False
        elif state in (PROVISIONAL, KNOWN):
            q = float(window_q(plan, ref, p_all[:, i : i + 1], part)[0])
            out_q[i] = q
            if gate:
                gate = q >= close_db(n_db)
            else:
                gate = q >= n_db
        elif state == NO_BAND:
            gate = True
        else:
            gate = False
        out_state[i] = state
        out_gate[i] = gate
    return {"state": out_state, "gate": out_gate, "q": out_q, "final": (state, ref, v_ref, vp_ref)}


# --------------------------------------------------------------------------------------------- source chains


@dataclasses.dataclass(frozen=True)
class Chain:
    name: str
    native: int  # the rate the source writes
    kind: str = "sdr"  # sdr | rtlfm | hw
    width: float = 12500.0  # the source's channel width (Hz)
    deemph_us: float = 0.0
    lpf: tuple | None = None  # ("fir", Hz) or ("butter", Hz)
    floor_dbfs: float | None = None
    tone: tuple | None = None  # (Hz, dBFS) an above-band tone added after the source's filters
    noise_dbfs: float = NOISE_DBFS
    expect: str = "band"  # band | no_band | either
    clips: bool = False  # noise driven into clipping: false opens bounded by the stale rule

    @property
    def demod_hz(self) -> int:
        if self.kind == "rtlfm":
            return self.native
        return 96000 if self.native > MONITOR_HZ else MONITOR_HZ


def chains() -> list[Chain]:
    out = [
        Chain("sdr-12k5-none@48000", 48000),
        Chain("sdr-10k-none@48000", 48000, width=10000.0),
        Chain("sdr-12k5-50us@48000", 48000, deemph_us=50.0),
        Chain("sdr-12k5-75us@48000", 48000, deemph_us=75.0),
        Chain("sdr-12k5-750us@48000", 48000, deemph_us=750.0),
        Chain("sdr-16k-75us@48000", 48000, width=16000.0, deemph_us=75.0),
        Chain("sdr-25k-none@48000", 48000, width=25000.0),
        Chain("sdr-12k5-none@44100", 44100),
        Chain("sdr-12k5-75us@24000", 24000, deemph_us=75.0),
        Chain("sdr-12k5-none@16000", 16000),
        Chain("sdr-12k5-none@96000", 96000),
        Chain("rtlfm@48000", 48000, kind="rtlfm"),
        Chain("rtlfm@24000", 24000, kind="rtlfm"),
        Chain("rtlfm@12000", 12000, kind="rtlfm"),
        Chain("rtlfm-deemp@24000", 24000, kind="rtlfm", deemph_us=75.0),
        Chain("hw-tap@48000", 48000, kind="hw", width=15000.0, floor_dbfs=-90.0),
        Chain("hw-tap@44100", 44100, kind="hw", width=15000.0, floor_dbfs=-90.0),
        Chain("sdr-12k5-none@48000-floor70", 48000, floor_dbfs=-70.0, noise_dbfs=-30.0),
        Chain("sdr-12k5-none@48000-tone5k", 48000, tone=(5000.0, -60.0)),
        Chain("sdr-12k5-none@48000-tone6k1", 48000, tone=(6100.0, -60.0)),
        Chain("lp-fir3000@48000", 48000, lpf=("fir", 3000.0), expect="no_band"),
        Chain("lp-fir3400-75us@48000", 48000, deemph_us=75.0, lpf=("fir", 3400.0), expect="no_band"),
        Chain("lp-fir4000@48000", 48000, lpf=("fir", 4000.0), expect="either"),
        Chain("lp-fir3400@16000", 16000, lpf=("fir", 3400.0), expect="no_band"),
        Chain("lp-fir3400-hot@48000", 48000, lpf=("fir", 3400.0), noise_dbfs=-1.0, expect="either", clips=True),
        Chain("lp-butter3000@48000", 48000, lpf=("butter", 3000.0), expect="either"),
        Chain("sdr-12k5-none@9600", 9600),
        Chain("sdr-12k5-none@8000", 8000),
    ]
    return out


@dataclasses.dataclass
class Seg:
    seconds: float
    kind: str = "noise"  # noise | speech | tone | dead | relay | zero
    cnr_db: float = 0.0
    gain_db: float = 0.0
    tone_hz: float = 1000.0
    offset_hz: float = 0.0
    neighbour_db: float | None = None  # a speech neighbour at +9 kHz, CNR in dB
    dev_hz: float | None = None  # the carrier's deviation, when not the source's rated one
    impulses: float = 0.0  # impulses a second, post-demod
    key: str = "a"


_SPEECH: dict = {}


def relay_audio(fs: int, n: int, rng: np.random.Generator) -> np.ndarray:
    """A repeater relaying a weak user's noise: Gaussian noise through a land-mobile 300-3000 Hz audio band at
    RELAY_RMS."""
    x = rng.standard_normal(n + 4096)
    y = sps.sosfilt(sps.butter(4, [300.0, 3000.0], btype="bandpass", fs=fs, output="sos"), x)[4096:]
    return y * (RELAY_RMS / max(float(np.sqrt(np.mean(y * y))), 1e-12))


def speech_audio(fs: int, n: int, rng: np.random.Generator) -> np.ndarray:
    if fs not in _SPEECH:
        _SPEECH[fs] = np.concatenate([sm.source_at_rate(name, "audio", fs) for name in sm.REAL_FM])
    src = _SPEECH[fs]
    start = int(rng.integers(0, len(src)))
    return np.resize(np.roll(src, -start), n)


class Source:
    """One chain's synthesis: RF segments through its channel filter, discriminator, filters and volume to int16 at the
    native rate, then the monitor's staging."""

    def __init__(self, chain: Chain):
        self.chain = chain
        self.fd = chain.demod_hz
        self.hi_rate = chain.native * RTLFM_OVERSAMPLE if chain.kind == "rtlfm" else self.fd
        if chain.kind == "hw":
            self.ch_sos = sps.butter(6, chain.width / 2.0, fs=self.hi_rate, output="sos")
            self.ch_taps = None
        elif chain.kind == "rtlfm":
            self.ch_sos, self.ch_taps = None, None
        else:
            self.ch_sos = None
            self.ch_taps = sps.firwin(127 if self.fd <= MONITOR_HZ else 255, chain.width / 2.0, fs=self.fd)
        self.dev = sm.rated_deviation_hz(min(chain.width, 25000.0))
        rng = np.random.default_rng(sm.job_seed("pcm_sq_probe", chain.name))
        probe = self.baseband(sm.white_noise(rng, int(0.5 * self.hi_rate)).astype(np.complex128))
        self.p_noise = float(np.mean(np.abs(probe[len(probe) // 4 :]) ** 2))
        audio = self.audio_chain(self.disc(probe))
        self.cal = 10.0 ** (chain.noise_dbfs / 20.0) * 32768.0 / float(np.sqrt(np.mean(audio[len(audio) // 4 :] ** 2)))

    def baseband(self, x: np.ndarray) -> np.ndarray:
        if self.chain.kind == "rtlfm":
            # rtl_fm's boxcar: means of RTLFM_OVERSAMPLE samples at that multiple of the output rate.
            n = len(x) // RTLFM_OVERSAMPLE * RTLFM_OVERSAMPLE
            return x[:n].reshape(-1, RTLFM_OVERSAMPLE).mean(axis=1)
        if self.ch_sos is not None:
            return sps.sosfilt(self.ch_sos, x)
        return sps.oaconvolve(x, self.ch_taps, mode="full")[: len(x)]

    def disc(self, z: np.ndarray) -> np.ndarray:
        d = np.angle(z[1:] * np.conj(z[:-1]))
        return np.concatenate(([0.0], d))

    def audio_chain(self, d: np.ndarray) -> np.ndarray:
        c = self.chain
        rate = self.fd
        if c.deemph_us > 0.0:
            pole = math.exp(-1.0 / (rate * c.deemph_us * 1e-6))
            d = sps.lfilter([1.0 - pole], [1.0, -pole], d)
        if c.lpf is not None:
            if c.lpf[0] == "fir":
                d = sps.lfilter(sps.firwin(511, c.lpf[1], fs=rate, window=("kaiser", 8.0)), 1.0, d)
            else:
                d = sps.sosfilt(sps.butter(4, c.lpf[1], fs=rate, output="sos"), d)
        if c.kind == "hw":
            d = sps.sosfilt(sps.butter(1, 20.0, btype="high", fs=rate, output="sos"), d)
            d = sps.lfilter(sps.firwin(127, 0.42 * rate, fs=rate), 1.0, d)
        if c.native != rate:
            g = math.gcd(c.native, rate)
            d = sps.resample_poly(d, c.native // g, rate // g)
        return d

    def render(self, segs: list[Seg], seed, agc: bool = False, drift_db_s: float = 0.0) -> tuple[np.ndarray, list]:
        """Monitor-rate float samples (int16 scale) and each segment's [start, end) in them."""
        c = self.chain
        rng = np.random.default_rng(sm.job_seed("pcm_sq", c.name, seed))
        pieces = []
        for s in segs:
            n = int(round(s.seconds * self.hi_rate))
            x = sm.white_noise(rng, n).astype(np.complex128)
            t = np.arange(n) / self.hi_rate
            if s.kind in ("speech", "tone", "dead", "relay"):
                if s.kind == "speech":
                    au = speech_audio(self.hi_rate, n, rng)
                elif s.kind == "relay":
                    au = relay_audio(self.hi_rate, n, rng)
                elif s.kind == "tone":
                    au = np.sin(2.0 * math.pi * s.tone_hz * t + rng.uniform(0.0, 2.0 * math.pi))
                else:
                    au = np.zeros(n)
                amp = math.sqrt(self.p_noise * 10.0 ** (s.cnr_db / 10.0))
                dev = self.dev if s.dev_hz is None else s.dev_hz
                ph = 2.0 * math.pi * np.cumsum(s.offset_hz + dev * au) / self.hi_rate
                x = x + amp * np.exp(1j * (ph + rng.uniform(0.0, 2.0 * math.pi)))
            if s.neighbour_db is not None:
                amp = math.sqrt(self.p_noise * 10.0 ** (s.neighbour_db / 10.0))
                au = speech_audio(self.hi_rate, n, rng)
                ph = 2.0 * math.pi * np.cumsum(9000.0 + self.dev * au) / self.hi_rate
                x = x + amp * np.exp(1j * ph)
            pieces.append(x)
        z = self.baseband(np.concatenate(pieces))
        audio = self.audio_chain(self.disc(z)) * self.cal
        bounds = np.concatenate(([0], np.cumsum([int(round(s.seconds * c.native)) for s in segs])))
        audio = np.resize(audio, int(bounds[-1]))
        gain = np.concatenate(
            [np.full(int(bounds[i + 1] - bounds[i]), 10.0 ** (s.gain_db / 20.0)) for i, s in enumerate(segs)]
        )
        if drift_db_s != 0.0:
            gain = gain * 10.0 ** (drift_db_s * np.arange(len(gain)) / c.native / 20.0)
        if agc:
            # A post-demod audio AGC: 50 ms attack, 500 ms release, to the noise level.
            env = np.abs(audio)
            level = np.empty_like(env)
            acc = float(np.mean(env[: c.native // 10]))
            att, rel = 1.0 - math.exp(-1.0 / (0.05 * c.native)), 1.0 - math.exp(-1.0 / (0.5 * c.native))
            for i in range(len(env)):
                acc += (att if env[i] > acc else rel) * (env[i] - acc)
                level[i] = acc
            audio = audio * (float(np.median(level)) / np.maximum(level, 1e-3))
        y = audio * gain
        if c.floor_dbfs is not None:
            y = y + rng.standard_normal(len(y)) * 10.0 ** (c.floor_dbfs / 20.0) * 32768.0
        if c.tone is not None:
            y = y + 10.0 ** (c.tone[1] / 20.0) * 32768.0 * math.sqrt(2.0) * np.sin(
                2.0 * math.pi * c.tone[0] * np.arange(len(y)) / c.native
            )
        for i, s in enumerate(segs):
            a, b = int(bounds[i]), int(bounds[i + 1])
            if s.impulses > 0.0:
                count = int(s.impulses * s.seconds)
                at = rng.integers(a, b, count)
                y[at] += rng.choice((-1.0, 1.0), count) * 10000.0
            if s.kind == "zero":
                y[a:b] = 0.0
        y = np.clip(np.round(y), -32768.0, 32767.0)
        factor = staging_factor(c.native)
        return stage_up(y, factor), [(int(bounds[i]) * factor, int(bounds[i + 1]) * factor) for i in range(len(segs))]


# --------------------------------------------------------------------------------------------- scenarios


def scenarios() -> dict[str, dict]:
    S = Seg
    return {
        "keyed": {
            "segs": [S(2), S(2, "speech", 20), S(1.5), S(1.5, "dead", 15), S(1.5), S(2, "speech", 8), S(1.5),
                     S(1.5, "tone", 25), S(1.5)],
        },
        "mid-carrier": {"segs": [S(2, "speech", 25), S(2), S(1.5, "speech", 15), S(1.5)]},
        "mid-dead": {"segs": [S(2, "dead", 25), S(2), S(1.5, "speech", 15), S(1.5)]},
        "continuous": {"segs": [S(4, "tone", 25)], "continuous": True},
        "volume-down": {
            "segs": [S(2), S(2, gain_db=-6), S(2, gain_db=-12), S(2, gain_db=-20), S(1.5, "speech", 15, -20),
                     S(1.5, gain_db=-20)],
        },
        "volume-up": {
            "segs": [S(2, gain_db=-20), S(2, gain_db=-8), S(2), S(1.5, "speech", 15), S(1.5)],
        },
        "volume-carrier": {
            "segs": [S(2), S(1, "speech", 20), S(1, "speech", 20, -12), S(2, gain_db=-12), S(1, "speech", 20, -12),
                     S(1, "speech", 20), S(2)],
        },
        "agc": {"segs": [S(2), S(2, "speech", 20), S(1.5), S(2, "speech", 12), S(1.5)], "agc": True},
        "drift": {"segs": [S(3), S(1.5, "speech", 15), S(3), S(1.5, "speech", 15), S(3)], "drift": -1.0},
        "clip": {
            "segs": [S(2), S(2, "speech", 20), S(1.5), S(1.5, gain_db=12), S(1.5, "speech", 20, 12), S(1.5, gain_db=12),
                     S(7, gain_db=0), S(1.5, "speech", 20), S(1.5)],
        },
        "impulses": {"segs": [S(2, impulses=2.0), S(2, "speech", 20, impulses=2.0), S(2, impulses=2.0)]},
        "neighbour": {
            "segs": [S(2), S(3, neighbour_db=20.0), S(2), S(1.5, "speech", 15), S(1.5)],
            "chains": ("sdr-25k-none@48000",),
        },
        "pauses": {"segs": [S(2), S(1, "zero"), S(1), S(1.5, "speech", 15), S(0.5, "zero"), S(1.5)]},
        # A low-deviation tone fading through 20 dB CNR: its noise rises toward noise's level above voice, and the
        # tone fills the voice band to noise's ratio; it is no lowered noise.
        "fading-tone": {
            "segs": [S(2), S(1.5, "tone", 30, dev_hz=200.0), S(2, "tone", 20, dev_hz=200.0),
                     S(2, "tone", 12, dev_hz=200.0), S(1.5)],
        },
        # The source turned down across a pause, and during a carrier's last 60 ms.
        "pause-volume": {
            "segs": [S(2), S(1, "speech", 20), S(0.2, "zero"), S(3, gain_db=-12), S(1.5, "speech", 20, -12),
                     S(1.5, gain_db=-12)],
        },
        # A repeater relaying a weak user's noise at the source's own noise level, then that user's speech: the noise
        # may read as noise turned down; the speech refutes that and plays.
        "relay-speech": {
            "segs": [S(2), S(3, "relay", 20, dev_hz=500.0), S(3, "speech", 20), S(1.5, "relay", 20, dev_hz=500.0),
                     S(2, "speech", 20), S(1.5)],
        },
        # The source turned down 3 dB, under the 4 dB a transition needs, at N = 3.
        "small-step": {"segs": [S(2), S(4, gain_db=-3), S(1.5, "speech", 20, -3), S(1.5, gain_db=-3)], "n_db": 3},
        "unkey-volume": {
            "segs": [S(2), S(1, "speech", 20), S(0.06, "speech", 20, -30), S(3, gain_db=-30),
                     S(1.5, "speech", 20, -30), S(1.5, gain_db=-30)],
        },
        "passbands": {
            "segs": [S(2, key="a"), S(1.5, "speech", 15, key="a"), S(1.5, key="b"), S(1.5, "speech", 15, key="b"),
                     S(1.5, key="a"), S(1.5, "speech", 20, key="a")],
            "restart_on_key": True,
        },
    }


# --------------------------------------------------------------------------------------------- one chain


def score_scenario(name: str, sc: dict, res: dict, w: dict, spans: list, fs: int) -> dict:
    segs = sc["segs"]
    gate, state = res["gate"], res["state"]
    end = w["end"]
    settle = int(SETTLE_SCORE_S * fs)
    first_noise = next((i for i, s in enumerate(segs) if s.kind == "noise"), None)
    noise_open_s = 0.0
    worst_event_s = 0.0
    dropout = []
    learn_s = math.nan
    no_band_s = math.nan
    no_band_on_noise = False
    noise_gain = None
    for i, s in enumerate(segs):
        a, b = spans[i]
        idx = np.where((end > a + settle) & (end <= b))[0]
        if not len(idx):
            continue
        if s.kind == "noise" and s.neighbour_db is None:
            g = gate[idx] & (state[idx] != NO_BAND)
            noise_open_s += float(np.sum(g)) / HALF_PER_S
            runs = np.diff(np.concatenate(([0], g.astype(np.int8), [0])))
            starts, stops = np.where(runs == 1)[0], np.where(runs == -1)[0]
            if len(starts):
                worst_event_s = max(worst_event_s, float(np.max(stops - starts)) / HALF_PER_S)
            if np.any(state[idx] == NO_BAND):
                no_band_on_noise = True
                if math.isnan(no_band_s) and first_noise is not None:
                    at = idx[np.argmax(state[idx] == NO_BAND)]
                    no_band_s = float((end[at] - spans[first_noise][0]) / fs)
            if i == first_noise:
                learned = np.where(state[idx] != LEARNING)[0]
                learn_s = float((end[idx[learned[0]]] - a) / fs) if len(learned) else math.inf
            noise_gain = s.gain_db
        elif s.kind in ("speech", "dead", "tone") and first_noise is not None and i > first_noise:
            heard = idx[end[idx] > a + int(0.1 * fs)]
            # Only a carrier at the reference's own volume: one keyed at another volume reads that much more or less.
            if len(heard) and s.cnr_db >= N_DEFAULT_DB + 6.0 and s.gain_db == noise_gain:
                dropout.append(float(np.mean(~gate[heard])))
    return {
        "noise_open_s": noise_open_s,
        "worst_event_s": worst_event_s,
        "dropout": max(dropout) if dropout else 0.0,
        "learn_s": learn_s,
        "no_band_s": no_band_s,
        "no_band_on_noise": no_band_on_noise,
        "final_state": STATE_NAMES[int(state[-1])],
    }


def keys_and_restarts(sc: dict, w: dict, spans: list):
    if not sc.get("restart_on_key"):
        return None, None
    end = w["end"]
    keys = []
    for e in end:
        seg = next((i for i, (a, b) in enumerate(spans) if a < e <= b), len(spans) - 1)
        keys.append(sc["segs"][seg].key)
    restarts = {i for i in range(1, len(keys)) if keys[i] != keys[i - 1]}
    return keys, restarts


def learned_ref(plan: SquelchPlan, w: dict, params: dict):
    """The reference and its participating band-passes the learner holds after w, or (None, None)."""
    res = run_learner(plan, w, params, float(N_DEFAULT_DB))
    st, ref, v_ref, _ = res["final"]
    if st not in (PROVISIONAL, KNOWN):
        return None, None
    return ref, participating(plan, ref, v_ref, float(params["ratio"]))


def run_chain(job: dict) -> dict:
    t0 = time.time()
    chain = next(c for c in chains() if c.name == job["chain"])
    mode = job["mode"]
    out = {
        "chain": chain.name,
        "native_hz": chain.native,
        "monitor_hz": MONITOR_HZ if staging_factor(chain.native) > 1 else chain.native,
        "expect": chain.expect,
        "floor_dbfs": chain.floor_dbfs,
        "candidates": {},
    }
    if band_edges(chain.native, None) is None:
        out["band"] = None
        out["why"] = (
            f"less than {MIN_BAND_HZ:.0f} Hz fits between {BAND_LO_HZ:.0f} Hz and {NYQUIST_FRACTION} of the native rate"
        )
        return out
    src = Source(chain)
    fs = MONITOR_HZ if staging_factor(chain.native) > 1 else chain.native
    plans = []
    for variant in job.get("variants") or BAND_VARIANTS:
        plan = SquelchPlan(chain.native, variant[0], variant[1], variant[2], variant[3] if len(variant) > 3 else 2)
        if plan.band is not None:
            plans.append(plan)
    # Every signal is rendered once, measured through every plan, and dropped: only windows and spans are kept.
    meas: dict = {plan.label: {"scen": {}, "noise": [], "wanted": [], "cnr": {}, "level": {}} for plan in plans}
    spans: dict = {"scen": {}, "wanted": [], "cnr": {}, "level": {}}

    def keep(kind: str, name, y: np.ndarray):
        for plan in plans:
            w = measure(plan, y)
            if kind in ("noise", "wanted"):
                meas[plan.label][kind].append(w)
            else:
                meas[plan.label][kind][name] = w

    scen = {k: v for k, v in scenarios().items() if chain.name in v.get("chains", (chain.name,))}
    for name, sc in scen.items():
        y, sp = src.render(sc["segs"], name, agc=sc.get("agc", False), drift_db_s=sc.get("drift", 0.0))
        spans["scen"][name] = sp
        keep("scen", name, y)
    noise_s = NOISE_SECONDS[mode]
    chunks = max(1, round(noise_s / NOISE_CHUNK_S))
    for k in range(chunks):
        keep("noise", k, src.render([Seg(noise_s / chunks + 0.5)], ("noise", k))[0])
    tone_n = TONE_SECONDS[mode]
    half = min(chain.width, 25000.0) / 2.0
    wanted = []
    for f in TONES_HZ[mode]:
        room = half - src.dev - f
        for off in [0.0] + ([room, -room] if room > 1.0 else []):
            seg = Seg(tone_n + 0.1, "tone", STRONG_CNR_DB, tone_hz=f, offset_hz=off)
            wanted.append((f"tone {f:.0f} Hz at {off:+.0f} Hz", seg))
    wanted.append(("speech", Seg(SPEECH_SECONDS[mode] + 0.1, "speech", STRONG_CNR_DB)))
    for c0 in range(0, len(wanted), WANTED_CHUNK):
        part = wanted[c0 : c0 + WANTED_CHUNK]
        y, sp = src.render([Seg(WANTED_LEAD_S)] + [seg for _, seg in part], ("wanted", c0))
        spans["wanted"].append(([label for label, _ in part], sp))
        keep("wanted", c0, y)
    for kind in ("tone", "dead"):
        y, sp = src.render([Seg(1.0)] + [Seg(0.6, kind, cnr) for cnr in CNRS_DB], ("cnr", kind))
        spans["cnr"][kind] = sp
        keep("cnr", kind, y)
    level_clipped = {}
    for vol in VOLUMES_DBFS:
        g = vol - chain.noise_dbfs
        y, sp = src.render([Seg(1.0, gain_db=g), Seg(0.6, "tone", 10.0, g), Seg(0.6, "tone", 20.0, g)], "level")
        spans["level"][str(vol)] = sp
        level_clipped[str(vol)] = bool(np.any(np.abs(y) >= CLIP_LEVEL))
        keep("level", str(vol), y)

    for plan in plans:
        grid = [DEFAULT_PARAMS]
        if job.get("grid") == plan.label:
            grid = [dict(zip(GRID, v)) for v in itertools.product(*GRID.values())]
        for params in grid:
            pkey = (
                f"{plan.label}:m{params['m']}:x{params['step']:g}:g{params['gain']:g}:r{params['ratio']:g}"
                f":t{params['no_band_s']:g}"
            )
            res = evaluate(plan, chain, params, meas[plan.label], spans, scen, fs)
            res["level_clipped"] = level_clipped
            res["verdict"] = chain_verdict(chain, res)
            out["candidates"][pkey] = res
        out.setdefault("bands", {})[plan.label] = [plan.band[0], plan.band[1], plan.k]
    out["band"] = list(band_edges(chain.native, None))
    out["seconds"] = time.time() - t0
    return out


def lead_windows(w: dict, count: int) -> dict:
    return {k: (v[:, :count] if k in ("P", "VP") else v[:count]) for k, v in w.items()}


def span_windows(w: dict, a: int, b: int, settle: int) -> np.ndarray:
    return np.where((w["end"] > a + settle) & (w["end"] <= b))[0]


def evaluate(plan: SquelchPlan, chain: Chain, params: dict, meas: dict, spans: dict, scen: dict, fs: int) -> dict:
    res = {"scenarios": {}}
    for name, sc in scen.items():
        w = meas["scen"][name]
        sp = spans["scen"][name]
        keys, restarts = keys_and_restarts(sc, w, sp)
        r = run_learner(plan, w, params, float(sc.get("n_db", N_DEFAULT_DB)), keys, restarts)
        res["scenarios"][name] = score_scenario(name, sc, r, w, sp, fs)
        if name == "keyed":
            r3 = run_learner(plan, w, params, float(N_MIN_DB), keys, restarts)
            res["scenarios"]["keyed@N3"] = score_scenario(name, sc, r3, w, sp, fs)
    # Noise alone at N = 3: windows whose Q reaches 3 once learned, and the share the gate is open.
    q_all, open_all, nb_any = [], [], False
    for w in meas["noise"]:
        r = run_learner(plan, w, params, float(N_MIN_DB))
        ok = r["state"] != LEARNING
        nb_any = nb_any or bool(np.any(r["state"] == NO_BAND))
        q_all.append(r["q"][ok & ~np.isnan(r["q"])])
        open_all.append(r["gate"][ok & (r["state"] != NO_BAND)])
    q = np.concatenate(q_all) if q_all else np.array([])
    res["noise"] = {
        "evaluations": int(len(q)),
        "false_open_n3": float(np.mean(q >= N_MIN_DB)) if len(q) else math.nan,
        "q_max": float(np.max(q)) if len(q) else math.nan,
        "open_n3": float(np.mean(np.concatenate(open_all))) if open_all else math.nan,
        "no_band": nb_any,
    }
    # Wanted modulation, each chunk against the reference learned from its leading noise.
    worst = (math.inf, "")
    lead = int(WANTED_LEAD_S * HALF_PER_S) - 1
    for w, (labels, sp) in zip(meas["wanted"], spans["wanted"]):
        ref, part = learned_ref(plan, lead_windows(w, lead), params)
        if ref is None:
            continue
        q = window_q(plan, ref, w["P"], part)
        for label, (a, b) in zip(labels, sp[1:]):
            idx = span_windows(w, a, b, int(0.08 * fs))
            if len(idx):
                p1 = float(np.percentile(q[idx], 1))
                if p1 < worst[0]:
                    worst = (p1, label)
    res["wanted_p1"] = worst[0]
    res["wanted_worst"] = worst[1]
    # Quieting by CNR (medians) and across volumes.
    res["cnr"] = {}
    for kind, w in meas["cnr"].items():
        ref, part = learned_ref(plan, lead_windows(w, 45), params)
        med = []
        for a, b in spans["cnr"][kind][1:]:
            idx = span_windows(w, a, b, int(0.1 * fs))
            ok = ref is not None and len(idx)
            med.append(float(np.median(window_q(plan, ref, w["P"][:, idx], part))) if ok else math.nan)
        res["cnr"][kind] = med
    lev = {}
    for vol, w in meas["level"].items():
        ref, part = learned_ref(plan, lead_windows(w, 45), params)
        lev[vol] = [
            float(np.median(window_q(plan, ref, w["P"][:, span_windows(w, a, b, int(0.1 * fs))], part)))
            if ref is not None
            else math.nan
            for a, b in spans["level"][vol][1:]
        ]
    res["level"] = lev
    return res


def chain_verdict(chain: Chain, res: dict) -> dict:
    sc = res["scenarios"]
    why = []
    if chain.expect in ("band", "either"):
        band_ok = True
        if res["noise"]["no_band"]:
            band_ok = False
            why.append("no band on noise")
        if chain.expect == "either":
            # Either verdict is right, as long as noise never holds the gate open past the bound.
            worst = max(x["worst_event_s"] for x in sc.values())
            fo = res["noise"]["false_open_n3"]
            bound = STALE_S + 0.2 if chain.clips else FALSE_EVENT_S
            ok = worst <= bound and (chain.clips or not fo > FALSE_OPEN_TARGET)
            verdict = "no band" if not band_ok else "band"
            return {"pass": ok, "why": f"{verdict} (either)" + ("" if ok else f"; noise opens {worst:.2f} s, {fo:.1e}")}
        learn = sc["keyed"]["learn_s"]
        if not learn <= LEARN_TARGET_S:
            why.append(f"learned in {learn:.2f} s")
        fo = res["noise"]["false_open_n3"]
        if not fo <= FALSE_OPEN_TARGET:
            why.append(f"noise false-open {fo:.1e}")
        headroom = chain.floor_dbfs is None or chain.floor_dbfs <= HEADROOM_FLOOR_DBFS
        if headroom and not res["wanted_p1"] >= N_MAX_DB + MARGIN_DB:
            why.append(f"wanted p1 {res['wanted_p1']:.1f} dB")
        for name, s in sc.items():
            bound = STALE_S + 0.2 if name in STALE_BOUNDED else FALSE_EVENT_S
            if s["worst_event_s"] > bound:
                why.append(f"{name}: open {s['worst_event_s']:.2f} s on noise")
            target = RELAY_DROPOUT_TARGET if name == "relay-speech" else DROPOUT_TARGET
            if s["dropout"] > target and name not in DIAGNOSTIC_DROPOUTS:
                why.append(f"{name}: dropout {s['dropout']:.2f}")
            if s["no_band_on_noise"] and chain.expect == "band":
                why.append(f"{name}: no band on noise")
        for kind, med in res["cnr"].items():
            m = [x for x in med if not math.isnan(x)]
            if any(b < a - 0.5 for a, b in zip(m, m[1:])):
                why.append(f"{kind} quieting not monotone in CNR")
        for cnr, qt, qd in zip(CNRS_DB, res["cnr"]["tone"], res["cnr"]["dead"]):
            if chain.width < AGREE_MIN_WIDTH_HZ:
                break
            if CNR_AGREE_DB[0] <= cnr <= CNR_AGREE_DB[1] and abs(qt - qd) > AGREE_DB:
                why.append(f"dead and tone carriers read {qd:.1f} and {qt:.1f} dB at {cnr:+.0f} dB CNR")
                break
        lev = res["level"]
        base = lev.get(str(NOISE_DBFS))
        if base and chain.floor_dbfs is None and chain.tone is None:
            for vol, med in lev.items():
                if res.get("level_clipped", {}).get(vol):
                    continue
                if any(abs(a - b) > LEVEL_TOL_DB for a, b in zip(med, base)):
                    why.append(f"level {vol} dBFS moves Q by more than {LEVEL_TOL_DB} dB")
        return {"pass": not why, "why": "; ".join(why)}
    # Low-passed: NO_BAND within the target, shut on noise until then.
    s = sc["keyed"]
    if s["noise_open_s"] > 0.0:
        why.append(f"open {s['noise_open_s']:.2f} s on noise")
    if not s["no_band_s"] <= NO_BAND_TARGET_S:
        why.append(f"no band after {s['no_band_s']:.2f} s" if not math.isnan(s["no_band_s"]) else "no band never")
    return {"pass": not why, "why": "; ".join(why)}


# --------------------------------------------------------------------------------------------- report


def fmt_rate(v: float) -> str:
    return "0" if v == 0.0 else f"{v:.1e}"


def row(cells) -> str:
    return "| " + " | ".join(str(c) for c in cells) + " |"


CHAIN_HEADER = (
    "source",
    "native Hz",
    "monitor Hz",
    "band Hz (sub-bands)",
    "learn s",
    "noise false-open N=3",
    "wanted p1 dB",
    "worst false open s",
    "verdict",
)


def candidate_table(results: list[dict]) -> list[str]:
    names = sorted({c for r in results for c in r["candidates"]})
    lines = [row(("candidate", "sources passing", "of sources measured", "worst false open s", "worst wanted p1 dB")),
             row(["---"] * 5)]
    for name in names:
        rs = [r["candidates"][name] for r in results if name in r["candidates"]]
        passing = sum(1 for c in rs if c["verdict"]["pass"])
        worst_open = max((s["worst_event_s"] for c in rs for s in c["scenarios"].values()), default=0.0)
        worst_p1 = min((c["wanted_p1"] for c in rs if not math.isinf(c["wanted_p1"])), default=math.inf)
        lines.append(row((name, passing, len(rs), f"{worst_open:.2f}", f"{worst_p1:.1f}")))
    return lines


def chosen_name(results: list[dict]) -> str | None:
    names = sorted({c for r in results for c in r["candidates"]})
    best = None
    for name in names:
        rs = [r["candidates"][name] for r in results if name in r["candidates"]]
        passing = sum(1 for c in rs if c["verdict"]["pass"])
        worst_open = max((s["worst_event_s"] for c in rs for s in c["scenarios"].values()), default=0.0)
        key = (passing, -worst_open)
        if best is None or key > best[0]:
            best = (key, name)
    return None if best is None else best[1]


def chain_row(r: dict, cand: str | None) -> str:
    if r.get("band") is None:
        return row((r["chain"], r["native_hz"], r["monitor_hz"], "none", "", "", "", "", "no room"))
    c = r["candidates"].get(cand) if cand else None
    if c is None:
        return row((r["chain"], r["native_hz"], r["monitor_hz"], "", "", "", "", "", "not measured"))
    lo, hi, k = r["bands"][cand.split(":m")[0]]
    worst = max((s["worst_event_s"] for s in c["scenarios"].values()), default=0.0)
    return row(
        (
            r["chain"],
            r["native_hz"],
            r["monitor_hz"],
            f"{lo:.0f}-{hi:.0f} ({k})",
            f"{c['scenarios']['keyed']['learn_s']:.2f}",
            fmt_rate(c["noise"]["false_open_n3"]),
            f"{c['wanted_p1']:.1f}",
            f"{worst:.2f}",
            ("pass" if c["verdict"]["pass"] else "FAIL") + (f" ({c['verdict']['why']})" if c["verdict"]["why"] else ""),
        )
    )


def report(results: list[dict], out_dir: Path, started: float, mode: str) -> str:
    lines = ["# PCM noise squelch design gate", ""]
    lines.append(
        f"`tools/pcm_noise_squelch_model.py` ({mode} run): {len(results)} sources, {time.time() - started:.0f} s."
    )
    lines.append("")
    with_band = [r for r in results if r.get("band") is not None]
    no_room = sorted(r["chain"] for r in results if r.get("band") is None)
    lines.append(f"Sources with a band: {len(with_band)} of {len(results)}.")
    lines.append("No room above voice: " + (", ".join(no_room) if no_room else "none") + ".")
    chosen = chosen_name(results)
    if with_band:
        lines += ["", "## Candidates", ""] + candidate_table(results)
        lines += ["", f"## Chosen: {chosen}", ""]
    lines += ["", "## Per source", "", row(CHAIN_HEADER), row(["---"] * len(CHAIN_HEADER))]
    for r in sorted(results, key=lambda r: r["chain"]):
        lines.append(chain_row(r, chosen))
    text = "\n".join(lines) + "\n"
    (out_dir / "pcm_noise_squelch_report.md").write_text(text)
    (out_dir / "pcm_noise_squelch_results.json").write_text(json.dumps({"results": results}, indent=1))
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    parser.add_argument("--quick", action="store_true", help="30 s of noise per source and a coarse tone grid")
    parser.add_argument("--out", type=Path, default=sm.ROOT / "build" / "pcm_noise_squelch_model")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--only", help="run only sources whose name contains this text")
    parser.add_argument("--grid", help="run the learner grid on this band variant (a label such as top6.5:s250:g4)")
    parser.add_argument("--variants", help="only these band variants, as top:sub:guard,... (top 'nyq' for 0.45 fs)")
    args = parser.parse_args(argv)
    started = time.time()
    mode = "quick" if args.quick else "full"
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    variants = None
    if args.variants:
        variants = []
        for v in args.variants.split(","):
            f = v.split(":")
            top = None if f[0] == "nyq" else float(f[0])
            variants.append((top, float(f[1]), float(f[2]), int(f[3]) if len(f) > 3 else 2))
    jobs = [
        {"chain": c.name, "mode": mode, "grid": args.grid, "variants": variants}
        for c in chains()
        if not args.only or args.only in c.name
    ]
    sm.real_sources()  # loaded once, before the workers fork
    results = []
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for res in pool.map(run_chain, jobs):
            results.append(res)
            print(f"  {res['chain']}: {res.get('seconds', 0.0):.0f} s", flush=True)
    text = report(results, out_dir, started, mode)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
