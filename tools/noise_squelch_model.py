#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Design gate for the NFM noise squelch (`--squelch noise[+N]`, issue #518 follow-up).

The question: does a quieting statistic on the FM discriminator's output read about 0 dB on noise whatever its level,
rise with the carrier-to-noise ratio, and stay well above the highest threshold offered while a strong carrier carries
wanted modulation, at every channel plan the analog FM monitor runs? Where it does not, `noise` runs as `auto`.

Statistic. The discriminator output d (dsd_fm_demod(): the phase step per sample, its small-angle series or atan2f)
goes through band-passes above the voice band. A band's quieting over a window is Q = 10 log10(P_ref / P), P its mean
output power and P_ref the same for complex Gaussian noise alone through the plan (a calibration, fixed seed): noise
reads about 0 dB at any level, since the discriminator sees phase only, and a carrier quiets the band by roughly its
CNR. Wanted modulation puts its own products in the band -- the channel filter truncates the FM sidebands, so a strong
tone's 2nd and 3rd harmonics land above voice as discrete lines -- and those cap Q for a strong carrier.

Candidates (all on the band B = [3.8 kHz, hi], hi = min(edge - 800 Hz, 0.45 fs), edge the channel taps' -1 dB point,
Butterworth band-passes of prototype order 4, 40 ms windows taken every 20 ms):
  single      one band-pass over B: Q of the whole band.
  split-max   B cut into K equal sub-bands (K = floor(|B| / 500 Hz), 1..9): Q of the best-quieted sub-band. A line
              lands in one sub-band and the others still show the carrier's quieting, but the best of K noisy readings
              sits above 0 dB on noise.
  split-guardG the same sub-bands: Q = max(Q_sum, Q_max - G dB), Q_sum from the sub-bands' summed powers (the whole
              band, as tight on noise as `single`) and Q_max the best sub-band's (it only wins when one sub-band is
              clearly quieter than the whole band: a strong carrier with a line elsewhere); G = 3 or 4.
  high1200    the first plan's band, [hi - 1200 Hz, hi], one band-pass (for comparison).

The chosen design (src/dsp/nfm_noise_squelch.c) is split-guard4: every plan with a band passes the gate, and the
4 dB guard keeps the best of nine noisy sub-band readings under the whole band's on noise.

Gate (per plan, for the chosen candidate): a band of at least 1200 Hz fits (8 and 10 kHz channels have none, and run
the auto squelch); wanted modulation -- a tone sweep 300-3000 Hz at the plan's rated deviation at every offset its
Carson bandwidth allows, and real NFM speech at rated deviation -- keeps the 1st-percentile Q at least MARGIN_DB above
the highest threshold offered; noise reaches the lowest threshold in at most FALSE_OPEN_TARGET of evaluations. The
report also lists stress cases outside the gate (a carrier 1 kHz off centre past its Carson bandwidth, 1.5x rated
deviation) and the real captures themselves.

Plans come from tools/squelch_model.py (its tap harness and rate chains), FM only: AM has no discriminator.

Usage:
    python3 tools/noise_squelch_model.py [--quick] [--out DIR] [--build-dir build/dev-debug] [--jobs N] [--only TEXT]
"""

from __future__ import annotations

import argparse
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

EDGE_DB = -1.0
EDGE_STEP_HZ = 10.0
EDGE_GUARD_HZ = 800.0
BAND_LO_HZ = 3800.0
MIN_BAND_HZ = 1200.0
SUB_BAND_HZ = 500.0
MAX_SUB_BANDS = 9
NYQUIST_FRACTION = 0.45
ORDER = 4
HIGH_BAND_HZ = 1200.0
GUARDS_DB = (3.0, 4.0)
HALF_PER_S = 50  # 20 ms halves; a window is two of them
N_MIN_DB = 3
N_MAX_DB = 30
N_DEFAULT_DB = 10
CLOSE_HYSTERESIS_DB = 3.0
CLOSE_FLOOR_DB = 1.5
MARGIN_DB = 6.0
FALSE_OPEN_TARGET = 1e-4
THRESHOLDS_DB = (3, 4, 5, 6, 10)
CHOSEN = "split-guard4"
CANDIDATES = ("single", "split-max", "split-guard3", "split-guard4", "high1200")
TONES_HZ = tuple(float(f) for f in range(300, 3001, 100))
OFFSETS_HZ = (0.0, 500.0, -500.0, 1000.0, -1000.0, 2500.0, -2500.0)
STRESS_OFFSET_HZ = 1000.0
STRESS_DEVIATION = 1.5
CNRS_DB = (-3.0, 0.0, 3.0, 6.0, 8.0, 10.0, 12.0, 15.0, 20.0, 30.0)
NOISE_FREE_CNR_DB = 100.0
NOISE_SECONDS = {"full": 600.0, "quick": 30.0}
NOISE_CHUNK_S = 20.0
TONE_SECONDS = {"full": 0.5, "quick": 0.15}
SPEECH_SECONDS = {"full": 4.0, "quick": 1.0}
PREF_SECONDS = 2.0  # what the C calibration runs per plan
SETTLE_S = 0.03  # filter start-up excluded from every measurement


# --------------------------------------------------------------------------------------------- the plan's band


def passband_edge_hz(taps: np.ndarray, fs: float) -> float:
    """The first EDGE_STEP_HZ grid frequency where the (symmetric) taps' response is EDGE_DB under its DC gain; fs/2
    when there is none (no channel filter)."""
    taps = np.asarray(taps, dtype=np.float64)
    centre = (len(taps) - 1) / 2.0
    n = np.arange(len(taps)) - centre
    dc = float(np.sum(taps))
    limit = 10.0 ** (EDGE_DB / 20.0)
    f = 0.0
    while f < fs / 2.0:
        if float(np.sum(taps * np.cos(2.0 * math.pi * f * n / fs))) < limit * dc:
            return f
        f += EDGE_STEP_HZ
    return fs / 2.0


def plan_band(taps: np.ndarray, fs: float) -> tuple[float, float, float] | None:
    """(lo, hi, edge) of the measurement band, or None when less than MIN_BAND_HZ fits above voice."""
    edge = passband_edge_hz(taps, fs)
    hi = min(edge - EDGE_GUARD_HZ, NYQUIST_FRACTION * fs)
    if hi - BAND_LO_HZ < MIN_BAND_HZ:
        return None
    return BAND_LO_HZ, hi, edge


def sub_band_count(lo: float, hi: float) -> int:
    return max(1, min(MAX_SUB_BANDS, int((hi - lo) // SUB_BAND_HZ)))


# --------------------------------------------------------------------------------------------- signal helpers


def disc(z: np.ndarray) -> np.ndarray:
    """dsd_fm_demod() in float32: z_n conj(z_{n-1}), then the small-angle series or atan2f."""
    z = z.astype(np.complex64)
    p = z[1:] * np.conj(z[:-1])
    re = np.real(p).astype(np.float32)
    im = np.imag(p).astype(np.float32)
    out = np.arctan2(im, re).astype(np.float32)
    small = (re > np.float32(1e-7)) & (np.abs(im) <= np.float32(0.35) * re)
    x = im[small] / re[small]
    x2 = x * x
    out[small] = x * (np.float32(1.0) + x2 * (np.float32(-1.0 / 3.0) + x2 * np.float32(0.2)))
    return out


def hb_noise(rng: np.random.Generator, count: int, hb: np.ndarray | None) -> np.ndarray:
    """count samples of the channel filter's input for white complex noise at the half-band input."""
    if hb is None:
        return sm.white_noise(rng, count)
    wide = sm.white_noise(rng, 2 * count + len(hb) - 1)
    return np.asarray(sps.oaconvolve(wide, hb.astype(np.float32), mode="valid"))[::2][:count].astype(np.complex64)


def half_bounds(fs: int, count: int) -> np.ndarray:
    """Sample-exact 20 ms boundaries floor(k fs / 50) inside count samples."""
    k = np.arange(0, count * HALF_PER_S // fs + 1, dtype=np.int64)
    b = (k * fs) // HALF_PER_S
    return b[b <= count]


def half_sums(y: np.ndarray, bounds: np.ndarray) -> np.ndarray:
    c = np.concatenate(([0.0], np.cumsum(y.astype(np.float64) ** 2)))
    return c[bounds[1:]] - c[bounds[:-1]]


class Plan:
    """One discriminator plan: channel taps, half-band, rate, its band and every candidate's band-passes."""

    def __init__(self, job: dict):
        self.fs = int(job["fs"])
        self.taps = np.asarray(job["taps"], dtype=np.float64)
        self.hb = None if job["hb"] is None else np.asarray(job["hb"], dtype=np.float64)
        self.width = job["width"]
        self.key = job["key"]
        self.settle = int(SETTLE_S * self.fs)
        self.band = plan_band(self.taps, self.fs)
        self.filters: dict[str, list] = {}
        if self.band is not None:
            lo, hi, _ = self.band
            k = sub_band_count(lo, hi)
            step = (hi - lo) / k
            subs = [self.bp(lo + i * step, lo + (i + 1) * step) for i in range(k)]
            self.filters = {"single": [self.bp(lo, hi)], "subs": subs}
            if hi - HIGH_BAND_HZ >= BAND_LO_HZ - 200.0:
                self.filters["high1200"] = [self.bp(hi - HIGH_BAND_HZ, hi)]
        rng = np.random.default_rng(sm.job_seed("noise_sq_pch", self.key))
        probe = sm.channel_filter(hb_noise(rng, self.fs + len(self.taps), self.hb), self.taps)
        self.p_noise_ch = float(np.mean(np.abs(probe) ** 2))

    def bp(self, lo: float, hi: float):
        return sps.butter(ORDER, [lo, hi], btype="bandpass", fs=self.fs, output="sos")

    def demod(self, sig: np.ndarray | None, count: int, cnr_db: float, seed, hb: bool = True, tilt: float = 0.0):
        """The discriminator output for count samples of sig (unit-power baseband, None for noise alone) at cnr_db."""
        rng = np.random.default_rng(sm.job_seed("noise_sq", self.key, seed))
        x = hb_noise(rng, count, self.hb if hb else None)
        if tilt != 0.0:
            corner = min(self.width / 2.0, NYQUIST_FRACTION * self.fs)
            b, a = sps.butter(1, corner, btype="low" if tilt > 0 else "high", fs=self.fs)
            x = np.asarray(sps.lfilter(b, a, x)).astype(np.complex64)
        if sig is not None:
            x = x + sig[:count].astype(np.complex64) * np.float32(math.sqrt(self.p_noise_ch * 10.0 ** (cnr_db / 10.0)))
        return disc(sm.channel_filter(x, self.taps))

    def halves(self, d: np.ndarray) -> dict[str, np.ndarray]:
        """Per filter group, the 20 ms half-window powers (rows: band-passes) after the settle time."""
        bounds = half_bounds(self.fs, len(d) - self.settle)
        out = {}
        for name, group in self.filters.items():
            out[name] = np.vstack([half_sums(np.asarray(sps.sosfilt(sos, d))[self.settle :], bounds) for sos in group])
        out["_n"] = np.diff(bounds).astype(np.float64)
        return out

    @staticmethod
    def window_q(h: dict, refs: dict) -> dict[str, np.ndarray]:
        """Each candidate's Q for every 40 ms window (two consecutive halves), one per 20 ms."""
        n = h["_n"][1:] + h["_n"][:-1]

        def pw(rows):
            return (rows[:, 1:] + rows[:, :-1]) / n

        q = {}
        p_single = pw(h["single"])[0]
        q["single"] = 10.0 * np.log10(refs["single"][0] / np.maximum(p_single, 1e-30))
        p_subs = pw(h["subs"])
        r_subs = np.asarray(refs["subs"])[:, None]
        q_each = 10.0 * np.log10(r_subs / np.maximum(p_subs, 1e-30))
        q_max = np.max(q_each, axis=0)
        q_sum = 10.0 * np.log10(np.sum(r_subs) / np.maximum(np.sum(p_subs, axis=0), 1e-30))
        q["split-max"] = q_max
        for g in GUARDS_DB:
            q[f"split-guard{g:.0f}"] = np.maximum(q_sum, q_max - g)
        if "high1200" in h:
            q["high1200"] = 10.0 * np.log10(refs["high1200"][0] / np.maximum(pw(h["high1200"])[0], 1e-30))
        return q

    def refs_of(self, d: np.ndarray) -> dict[str, list[float]]:
        out = {}
        for name, group in self.filters.items():
            out[name] = [
                float(np.mean(np.asarray(sps.sosfilt(sos, d))[self.settle :].astype(np.float64) ** 2)) for sos in group
            ]
        return out


def close_db(n_db: float) -> float:
    return max(n_db - CLOSE_HYSTERESIS_DB, CLOSE_FLOOR_DB)


# --------------------------------------------------------------------------------------------- one plan


def run_plan(job: dict) -> dict:
    t0 = time.time()
    plan = Plan(job)
    fs, width = plan.fs, plan.width
    mode = job["mode"]
    deviation = sm.rated_deviation_hz(width)
    out = {"key": plan.key, "fs": fs, "width": width, "deviation": deviation, "candidates": {}}
    if plan.band is None:
        out["edge_hz"] = passband_edge_hz(plan.taps, fs)
        out["why"] = (
            f"less than {MIN_BAND_HZ:.0f} Hz fits between {BAND_LO_HZ:.0f} Hz and the channel edge less "
            f"{EDGE_GUARD_HZ:.0f} Hz"
        )
        return out
    lo, hi, edge = plan.band
    out.update({"band": [lo, hi], "edge_hz": edge, "sub_bands": len(plan.filters["subs"])})

    # P_ref: a 2 s calibration through the half-band and the channel taps, as the C code runs it; one without the
    # half-band for comparison; and the references the long run measures.
    span = int(PREF_SECONDS * fs) + len(plan.taps) + plan.settle
    refs = plan.refs_of(plan.demod(None, span, 0.0, "pref"))
    refs_nohb = plan.refs_of(plan.demod(None, span, 0.0, "pref", hb=False))

    # Noise alone, in independent chunks.
    noise_s = NOISE_SECONDS[mode]
    chunks = max(1, round(noise_s / NOISE_CHUNK_S))
    chunk_n = int(noise_s / chunks * fs) + len(plan.taps) + plan.settle
    qn = {c: [] for c in CANDIDATES}
    long_sum: dict[str, np.ndarray] = {}
    long_n = 0.0
    for k in range(chunks):
        h = plan.halves(plan.demod(None, chunk_n, 0.0, ("noise", k)))
        for name in plan.filters:
            long_sum[name] = long_sum.get(name, 0.0) + np.sum(h[name], axis=1)
        long_n += float(np.sum(h["_n"]))
        for c, q in Plan.window_q(h, refs).items():
            qn[c].append(q)
    long_ref = {name: (long_sum[name] / long_n).tolist() for name in plan.filters}
    out["pref_error_db"] = {
        "c_2s": max(abs(10.0 * math.log10(a / b)) for name in plan.filters for a, b in zip(refs[name], long_ref[name])),
        "2s_no_hb": max(
            abs(10.0 * math.log10(a / b)) for name in plan.filters for a, b in zip(refs_nohb[name], long_ref[name])
        ),
    }
    tilt_q = {}
    for t in (1.0, -1.0):
        q = Plan.window_q(plan.halves(plan.demod(None, int(10.0 * fs), 0.0, ("tilt", t), tilt=t)), refs)
        tilt_q["lp" if t > 0 else "hp"] = {c: float(np.median(v)) for c, v in q.items()}
    for c in CANDIDATES:
        if not qn[c]:
            continue
        q = np.concatenate(qn[c])
        out["candidates"][c] = {
            "noise_evaluations": len(q),
            "noise_q": {str(p): float(np.percentile(q, p)) for p in (1, 50, 99, 99.99)},
            "noise_q_max": float(np.max(q)),
            "false_open": {str(n): float(np.mean(q >= n)) for n in THRESHOLDS_DB},
            "noise_holds_open": {str(n): float(np.mean(q >= close_db(n))) for n in (N_MIN_DB, 6, N_DEFAULT_DB)},
            "tilt_median_q": {k: v[c] for k, v in tilt_q.items()},
            "mod": {},
        }

    def measure(kind: str, label: str, d: np.ndarray):
        for c, q in Plan.window_q(plan.halves(d), refs).items():
            out["candidates"][c]["mod"].setdefault(kind, {})[label] = {
                "p1": float(np.percentile(q, 1)),
                "min": float(np.min(q)),
                "median": float(np.median(q)),
            }

    tone_n = int(TONE_SECONDS[mode] * fs) + len(plan.taps) + plan.settle
    t = np.arange(tone_n) / fs

    def fm(audio_dev_hz: np.ndarray, offset: float) -> np.ndarray:
        return np.exp(1j * (2.0 * math.pi * np.cumsum(offset + audio_dev_hz) / fs))

    half = width / 2.0
    gate_offsets = [o for o in OFFSETS_HZ if abs(o) + deviation + sm.VOICE_TOP_HZ <= half + 1e-9]
    for off in gate_offsets:
        for f in TONES_HZ:
            sig = fm(deviation * np.sin(2.0 * math.pi * f * t), off)
            measure(
                "gate",
                f"tone {f:.0f} Hz at {off:+.0f} Hz",
                plan.demod(sig, tone_n, NOISE_FREE_CNR_DB, ("tone", off, f)),
            )
    speech_n = int(SPEECH_SECONDS[mode] * fs) + len(plan.taps) + plan.settle
    for name in sm.REAL_FM:
        audio = np.resize(sm.source_at_rate(name, "audio", fs), speech_n)
        measure(
            "gate",
            f"speech {name}",
            plan.demod(fm(deviation * audio, 0.0), speech_n, NOISE_FREE_CNR_DB, ("speech", name)),
        )
    for f in (300.0, 1000.0, 2000.0, 3000.0):
        for off in (STRESS_OFFSET_HZ, -STRESS_OFFSET_HZ):
            if off not in gate_offsets:
                sig = fm(deviation * np.sin(2.0 * math.pi * f * t), off)
                measure(
                    "stress",
                    f"tone {f:.0f} Hz at {off:+.0f} Hz",
                    plan.demod(sig, tone_n, NOISE_FREE_CNR_DB, ("stress", off, f)),
                )
        sig = fm(STRESS_DEVIATION * deviation * np.sin(2.0 * math.pi * f * t), 0.0)
        measure(
            "stress",
            f"tone {f:.0f} Hz x{STRESS_DEVIATION:g} deviation",
            plan.demod(sig, tone_n, NOISE_FREE_CNR_DB, ("over", f)),
        )
    for cnr in CNRS_DB:
        sig = fm(deviation * np.sin(2.0 * math.pi * 1000.0 * t), 0.0)
        measure("cnr_tone", f"{cnr:+.0f}", plan.demod(sig, tone_n, cnr, ("cnr", cnr)))
        measure(
            "cnr_dead", f"{cnr:+.0f}", plan.demod(np.ones(tone_n, dtype=np.complex64), tone_n, cnr, ("cnr_dead", cnr))
        )
    if width >= 2.0 * sm.REAL_CHANNEL_HALF_HZ:
        for name in sm.REAL_FM:
            iq = sm.source_at_rate(name, "iq", fs).astype(np.complex64)
            measure("real", name, disc(sm.channel_filter(iq, plan.taps)))
    out["seconds"] = time.time() - t0
    return out


# --------------------------------------------------------------------------------------------- verdicts and report


def verdict(res: dict, cand: str) -> dict | None:
    c = res["candidates"].get(cand)
    if c is None:
        return None
    gate = c["mod"].get("gate", {})
    tones = [(v["p1"], k) for k, v in gate.items() if k.startswith("tone")]
    speech = [(v["p1"], k) for k, v in gate.items() if k.startswith("speech")]
    worst = min(tones + speech)
    fo = c["false_open"][str(N_MIN_DB)]
    return {
        "tone_p1_min": min(tones)[0] if tones else math.inf,
        "tone_worst": min(tones)[1] if tones else "",
        "speech_p1_min": min(speech)[0] if speech else math.inf,
        "mod_p1_min": worst[0],
        "mod_worst": worst[1],
        "false_open_nmin": fo,
        "supported": worst[0] >= N_MAX_DB + MARGIN_DB and fo <= FALSE_OPEN_TARGET,
    }


def fmt_rate(v: float) -> str:
    return "0" if v == 0.0 else f"{v:.1e}"


def row(cells) -> str:
    return "| " + " | ".join(str(c) for c in cells) + " |"


CANDIDATE_HEADER = (
    "candidate",
    "plans passing",
    "of plans with a band",
    "worst wanted p1 (dB)",
    "worst tone p1",
    "worst speech p1",
    "worst noise false-open at N=3",
    "worst noise Q max",
)
PLAN_HEADER = (
    "width Hz",
    "rate Hz",
    "band Hz (sub-bands)",
    "wanted p1 min (worst)",
    "speech p1 min",
    "stress p1 min (worst)",
    "real p1/median",
    "noise false-open N=3/4/5",
    "noise Q max",
    "verdict",
)


def candidate_row(results: list[dict], cand: str) -> str | None:
    vs = [(r, verdict(r, cand)) for r in results]
    vs = [(r, v) for r, v in vs if v is not None]
    if not vs:
        return None
    return row(
        (
            cand,
            sum(1 for _, v in vs if v["supported"]),
            len(vs),
            f"{min(v['mod_p1_min'] for _, v in vs):.1f}",
            f"{min(v['tone_p1_min'] for _, v in vs):.1f}",
            f"{min(v['speech_p1_min'] for _, v in vs):.1f}",
            fmt_rate(max(v["false_open_nmin"] for _, v in vs)),
            f"{max(r['candidates'][cand]['noise_q_max'] for r, _ in vs):.2f}",
        )
    )


def chosen_summary(results: list[dict], chosen: list) -> list[str]:
    with_band = [(r, v) for r, v in chosen if v is not None]
    passing = sum(1 for _, v in with_band if v["supported"])
    no_band = sorted({(r["width"], r["fs"]) for r, v in chosen if v is None})
    errs = [r["pref_error_db"] for r, _ in with_band]
    tilts = [r["candidates"][CHOSEN]["tilt_median_q"] for r, _ in with_band]
    tilt_lo = min(min(t.values()) for t in tilts)
    tilt_hi = max(max(t.values()) for t in tilts)
    holds = [r["candidates"][CHOSEN]["noise_holds_open"] for r, _ in with_band]
    hold_txt = ", ".join(f"{fmt_rate(max(h[str(n)] for h in holds))} (N={n})" for n in (N_MIN_DB, 6, N_DEFAULT_DB))
    cnr_rows: dict[float, list[float]] = {}
    for r, _ in with_band:
        for k, v in r["candidates"][CHOSEN]["mod"]["cnr_tone"].items():
            cnr_rows.setdefault(float(k), []).append(v["median"])
    cnr_txt = "; ".join(f"{k:+.0f} dB: {min(v):.1f}..{max(v):.1f}" for k, v in sorted(cnr_rows.items()))
    return [
        f"- Plans with a band: {len(with_band)} of {len(results)}; passing: {passing}.",
        "- No band (noise runs as auto): " + (", ".join(f"{w} Hz at {fs} Hz" for w, fs in no_band) or "none") + ".",
        (
            f"- Calibration: 2 s of noise through the half-band and the channel taps is within "
            f"{max(e['c_2s'] for e in errs):.2f} dB of the long-run reference in every band (skipping the half-band: "
            f"{max(e['2s_no_hb'] for e in errs):.2f} dB)."
        ),
        f"- A receiver slope (one pole at the channel edge) moves noise's median Q to {tilt_lo:+.2f}..{tilt_hi:+.2f}.",
        f"- Noise holds an open gate (Q at or over the close threshold) in at most {hold_txt} of evaluations.",
        f"- Median Q by CNR (1 kHz tone at rated deviation), min..max over plans: {cnr_txt}.",
    ]


def plan_row(r: dict, v: dict | None) -> str:
    if v is None:
        return row((r["width"], r["fs"], f"none (edge {r['edge_hz']:.0f})", "", "", "", "", "", "", "auto"))
    c = r["candidates"][CHOSEN]
    st = c["mod"].get("stress", {})
    st_min = min(((x["p1"], k) for k, x in st.items()), default=(math.nan, ""))
    real = c["mod"].get("real", {})
    return row(
        (
            r["width"],
            r["fs"],
            f"{r['band'][0]:.0f}-{r['band'][1]:.0f} ({r['sub_bands']})",
            f"{v['mod_p1_min']:.1f} ({v['mod_worst']})",
            f"{v['speech_p1_min']:.1f}",
            f"{st_min[0]:.1f} ({st_min[1]})",
            ", ".join(f"{x['p1']:.0f}/{x['median']:.0f}" for x in real.values()),
            "/".join(fmt_rate(c["false_open"][str(n)]) for n in (3, 4, 5)),
            f"{c['noise_q_max']:.2f}",
            "noise" if v["supported"] else "FAIL",
        )
    )


def report(results: list[dict], members: dict, out_dir: Path, started: float, mode: str) -> str:
    lines = ["# NFM noise squelch design gate", ""]
    lines.append(
        f"`tools/noise_squelch_model.py` ({mode} run): {len(results)} distinct plans, {time.time() - started:.0f} s."
    )
    lines.append("")
    lines.append(
        f"Gate: wanted modulation keeps its 1st-percentile Q >= {N_MAX_DB} + {MARGIN_DB:.0f} dB (N up to {N_MAX_DB}); "
        f"noise reaches N = {N_MIN_DB} in <= {FALSE_OPEN_TARGET:g} of 20 ms evaluations."
    )
    lines += ["", "## Candidates", "", row(CANDIDATE_HEADER), row(["---"] * len(CANDIDATE_HEADER))]
    for cand in CANDIDATES:
        line = candidate_row(results, cand)
        if line is not None:
            lines.append(line)
    chosen = [(r, verdict(r, CHOSEN)) for r in results]
    lines += ["", f"## Chosen: {CHOSEN}", ""]
    lines += chosen_summary(results, chosen)
    lines += ["", "## Per plan", "", row(PLAN_HEADER), row(["---"] * len(PLAN_HEADER))]
    for r, v in sorted(chosen, key=lambda rv: (rv[0]["width"], rv[0]["fs"])):
        lines.append(plan_row(r, v))
    lines += ["", "Plans (width at rate: the chains that run them):", ""]
    for r in sorted(results, key=lambda r: (r["width"], r["fs"])):
        lines.append(f"- {r['width']} Hz at {r['fs']} Hz: " + "; ".join(members[r["key"]]))
    text = "\n".join(lines) + "\n"
    (out_dir / "noise_squelch_report.md").write_text(text)
    (out_dir / "noise_squelch_results.json").write_text(json.dumps({"results": results}, indent=1))
    return text


def unique_jobs(combos: list[dict], hb_taps: dict, mode: str) -> tuple[list[dict], dict]:
    jobs, members = {}, {}
    for combo in combos:
        plan = combo["plan"]
        key = f"{combo['fkey']}:{combo['protected_hz']}"
        members.setdefault(key, []).append(sm.combo_id(combo))
        jobs.setdefault(
            key,
            {
                "key": key,
                "taps": np.asarray(combo["taps"]).tolist(),
                "hb": None if hb_taps.get(plan.hb) is None else np.asarray(hb_taps[plan.hb]).tolist(),
                "fs": plan.channel_rate,
                "width": combo["protected_hz"],
                "mode": mode,
            },
        )
    return sorted(jobs.values(), key=lambda j: -j["fs"]), members


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    parser.add_argument("--quick", action="store_true", help="30 s of noise per plan and short tones (a smoke run)")
    parser.add_argument("--out", type=Path, default=sm.ROOT / "build" / "noise_squelch_model")
    parser.add_argument("--build-dir", type=Path, default=sm.ROOT / "build" / "dev-debug")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--only", help="run only plans whose key contains this text")
    args = parser.parse_args(argv)
    started = time.time()
    mode = "quick" if args.quick else "full"
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = sm.build_harness(args.build_dir.resolve(), out_dir)
    plans = sm.rate_plans()
    requests = {(p.rate_out, w, sm.FM_KIND) for p in plans for w in (*sm.NFM_WIDTHS_HZ, sm.NFM_WIDTH_DEFAULT_HZ, 0)}
    requests |= {(p.rate_out, w, sm.AM_KIND) for p in plans for w in sm.AM_WIDTHS_HZ}
    header, harness_plans = sm.run_harness(exe, sorted(requests))
    hb_taps = {name: np.asarray(header[name], dtype=np.float64) for name in ("hb15", "hb31")}
    combos = [c for c in sm.build_combos(plans, harness_plans, hb_taps) if c["runs"] and c["config"].kind == sm.FM_KIND]
    jobs, members = unique_jobs(combos, hb_taps, mode)
    if args.only:
        jobs = [j for j in jobs if args.only in j["key"]]
    print(f"{len(combos)} NFM combinations, {len(jobs)} distinct plans", flush=True)
    sm.real_sources()  # loaded once, before the workers fork
    results = []
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for res in pool.map(run_plan, jobs):
            results.append(res)
            print(f"  {res['width']} Hz at {res['fs']} Hz: {res.get('seconds', 0.0):.0f} s", flush=True)
    text = report(results, members, out_dir, started, mode)
    print(text[: text.find("## Per plan")])
    return 0


if __name__ == "__main__":
    sys.exit(main())
