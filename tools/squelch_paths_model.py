#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Design gate for the auto squelch on two more paths (issue #625): EDACS analog voice and the M17 encoder's monitor.

The auto squelch's classifier was chosen by tools/squelch_model.py on the channel plans the analog monitor runs. Issue
#625 runs the same tracker, with its shipped thresholds unchanged, on two more paths:

  - EDACS analog voice. EDACS keeps the stream on the FSK discriminator at 9600 bit/s, two levels, so the plan is the
    PROVOICE profile's (designed within the 144-tap cap, or its 63-tap fallback), no channel filter at all (the channel
    LPF off: rate_in under 20 kHz, or DSD_NEO_CHANNEL_LPF=0), or WIDE where a live switch leaves that label in force.
    The FSK path never post-decimates, and EDACS needs a channel rate of at least 12 kHz: under it a 9600 bit/s
    symbol gets fewer than 1.25 samples, EDACS cannot decode its control channel, and it never reaches a voice channel
    (its presets run 24 kHz). EDACS voice runs on 25 kHz channels at up to 4.5-5 kHz deviation and on NPSPAC
    12.5 kHz channels at about 4 kHz, so peak deviations of 2.5, 4, 4.5 and 5 kHz are all counted as wanted carriers,
    past the PROVOICE filter's 6.25 kHz edge too: a call's voice must keep the gate open however hard the filter
    truncates it. Voice carries a 150 bit/s sub-audible LSD (modelled at 750 Hz deviation, an assumption), and the
    channel carries 9600 bit/s data and dotting at the start and end of a call (2.5-4.5 kHz, also an assumption): the
    data counts towards the floor-pollution target (a data burst read as noise would teach the floor carrier power), not
    towards the miss target (a missed burst only closes the gate a moment early or late).
  - The M17 encoder's monitor: the legacy WIDE profile plan (144-tap cap, 63-tap fallback) off the analog family, or no
    filter, at every rate chain, with the analog monitor's own NFM carrier set.

It evaluates the shipped classifier exactly (src/dsp/squelch_floor.c): 40 ms windows, X = (CV^2 - 1) sqrt(N_eff) and
Y = |mean(u_k conj(u_{k-L})) - beta| sqrt(N_eff) with the plan constants the C designs (tools/squelch_model_taps.cpp
prints them), CARRIER at X <= -7.0 or Y >= 3.3, NOISE at X >= -3.8 and Y <= 2.1, and a NOISE window whose 20 ms halves
differ by 3 dB or more read as UNDECIDED. The targets are tools/squelch_model.py's four, per window: noise read as
carrier <= 1e-4, a 6 dB wanted carrier not read as carrier <= 1e-2, a carrier (wanted or data) of 6 dB or more read as
noise <= 1e-4, noise not read as noise <= 0.25.

Timing checks run the C tracker itself (squelch_model_taps track) on each distinct plan:
  - learning: a fresh tracker on a wanted carrier at 6, 10 and 20 dB CNR (a call lands mid-carrier); the longest
    closed run from the call's first sample, the closed window its first decision takes included, must stay under
    the path's hold;
  - known floor: 1 s of noise, then the carrier at the margin + 3 dB (margins 3, 10 and 30); the longest closed run
    from the carrier's first sample must stay under the hold;
  - below the threshold: 1 s of noise, then a dead carrier whose power with the noise sits 3 dB under the opening
    level (margins 10 and 30; at 3 there is no carrier under it); the gate stays closed (open on at most 1 % of its
    samples);
  - closing delay: a 20 dB carrier dropping at 40 phases across a window, learning and with a known floor; the worst
    delay from the drop to the first closed flag must stay under L_MAX_MS, the bound EDACS budgets.
The holds: EDACS releases after a closed run of 4 triplets (4 x 2880 samples at its output rate) less L_MAX_MS, never
under EDACS_MIN_HOLD_MS (two windows); its output rate is taken as the channel rate (a replay unresampled, the
shortest hold) or 48 kHz on a device-forced chain, which is resampled there. The encoder's VOX releases after 11
closed 40 ms reads (440 ms).

Usage:
    python3 tools/squelch_paths_model.py [--quick] [--out DIR] [--build-dir build/dev-debug] [--jobs N]
                                         [--noise-windows N]

Writes report.md and report.json under DIR (default build/squelch_paths_model/) and prints the verdict; exits 1 when a
plan either path runs misses a target and is not one of ACCEPTED, the plans accepted with a reason. The full run takes
about 20 minutes on 14 workers; --quick about a minute, too few windows to resolve 1e-4. Offline; CI does not run it.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import scipy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import squelch_model as sm  # noqa: E402

# The shipped classifier (src/dsp/squelch_floor.c).
CARRIER_X = -7.0
CARRIER_Y = 3.3
NOISE_X = -3.8
NOISE_Y = 2.1
UNSTEADY_RATIO = 2.0  # a NOISE window whose 20 ms halves differ by 3 dB is UNDECIDED

UNDECIDED, NOISE, CARRIER = 0, 1, 2

PROFILE_WIDE = 0  # DSD_CH_LPF_PROFILE_WIDE
PROFILE_PROVOICE = 3  # DSD_CH_LPF_PROFILE_PROVOICE
PROFILE_NONE = -2  # the harness's "no channel filter"

EDACS_DEVIATIONS_HZ = (2500.0, 4000.0, 4500.0, 5000.0)
EDACS_DATA_DEVIATIONS_HZ = (2500.0, 3500.0, 4500.0)
EDACS_IN_CHANNEL_OFFSET_HZ = 1000.0
LSD_BPS = 150.0
LSD_DEVIATION_HZ = 750.0
DATA_BPS = 9600.0

# EDACS's hold (src/protocol/edacs/edacs-fme.c): four triplets of 2880 samples at its output rate, less the closing
# delay it budgets (L_MAX_MS, DSD_SQUELCH_CLOSE_DELAY_MS in include/dsd-neo/core/power.h). The encoder's VOX: 11 closed
# 40 ms reads.
EDACS_MIN_CHANNEL_RATE_HZ = 12000
EDACS_TRIPLET_SAMPLES = 2880
EDACS_HOLD_TRIPLETS = 4
L_MAX_MS = 85.0
EDACS_MIN_HOLD_MS = 80.0  # EDACS_ANALOG_GATE_MIN_HOLD_MS (src/protocol/edacs/edacs_internal.h)
VOX_HOLD_S = 0.44
TIMING_MARGINS_DB = (3, 10, 30)
BELOW_MARGINS_DB = (10, 30)
TIMING_LEARN_CNRS_DB = (6.0, 10.0, 20.0)
TIMING_DELAY_PHASES = 40
BELOW_OPEN_LIMIT = 0.01

# Plans accepted short of a target, with why. tools/squelch_model.py accepted the same forced filter (the channel LPF
# on at an 8 kHz-class rate, where its legacy design is all the channel there is) at 1.1 % for the analog monitor.
ACCEPTED = {
    ("encoder", "sddc-bw4k->7812", "legacy WIDE, DSD_NEO_CHANNEL_LPF=1"): (
        "the channel LPF forced on at a 7.8 kHz channel misses 6 dB carriers a little over 1 %, as the analog "
        "monitor's forced 8 kHz plan does (1.1 %, accepted by tools/squelch_model.py); its longest closed run inside "
        "a call is one window, far under the VOX's 440 ms"
    ),
}


# --------------------------------------------------------------------------------------------- plans


def edacs_output_rate(plan: sm.RatePlan) -> int:
    """The rate EDACS reads at: the channel rate, resampled to 48 kHz where a device forces another."""
    return plan.channel_rate if plan.source != "device" else 48000


def edacs_hold_s(plan: sm.RatePlan) -> float:
    hold = EDACS_HOLD_TRIPLETS * EDACS_TRIPLET_SAMPLES / edacs_output_rate(plan) - L_MAX_MS / 1000.0
    return max(hold, EDACS_MIN_HOLD_MS / 1000.0)


def path_filters(plans: list[sm.RatePlan]) -> list[dict]:
    """Every (path, chain, filter) the two paths run: the filter as a harness request."""
    out = []
    for plan in plans:
        hb = 15 if plan.hb == "hb15" else (31 if plan.hb == "hb31" else 0)
        enable_default = plan.rate_in >= sm.LPF_DEFAULT_ENABLE_RATE_IN_HZ
        for env in ("", "1", "0"):
            enable = enable_default if env == "" else env == "1"
            if env and enable == enable_default:
                continue  # the same filter as the default
            label_env = f", DSD_NEO_CHANNEL_LPF={env}" if env else ""
            if plan.post_downsample == 1 and plan.channel_rate >= EDACS_MIN_CHANNEL_RATE_HZ:
                if enable:
                    out.append(
                        {
                            "path": "edacs",
                            "plan": plan,
                            "profile": PROFILE_PROVOICE,
                            "hb": hb,
                            "env": env,
                            "label": f"PROVOICE{label_env}",
                        }
                    )
                    out.append(
                        {
                            "path": "edacs",
                            "plan": plan,
                            "profile": PROFILE_WIDE,
                            "hb": hb,
                            "env": env,
                            "label": f"WIDE (live switch){label_env}",
                        }
                    )
                else:
                    out.append(
                        {
                            "path": "edacs",
                            "plan": plan,
                            "profile": PROFILE_NONE,
                            "hb": hb,
                            "env": env,
                            "label": f"none (channel LPF off){label_env}",
                        }
                    )
            out.append(
                {
                    "path": "encoder",
                    "plan": plan,
                    "profile": PROFILE_WIDE if enable else PROFILE_NONE,
                    "hb": hb,
                    "env": env,
                    "label": ("legacy WIDE" if enable else "none (channel LPF off)") + label_env,
                }
            )
    return out


def run_profile_harness(exe: Path, filters: list[dict]) -> None:
    """Fill each filter's taps, protected width and C classifier plan from the harness."""
    requests = sorted({(f["plan"].rate_out, f["profile"], f["hb"], f["plan"].channel_rate) for f in filters})
    stdin = "".join(f"{rate} 0 0 {profile} {hb} {crate}\n" for rate, profile, hb, crate in requests)
    proc = subprocess.run([str(exe)], input=stdin, capture_output=True, text=True, check=True)
    recs = {}
    for line in proc.stdout.splitlines()[1:]:
        rec = json.loads(line)
        recs[(rec["rate_out"], rec["profile"], rec["hb"], rec["channel_rate"])] = rec
    for f in filters:
        plan = f["plan"]
        rec = recs[(plan.rate_out, f["profile"], f["hb"], plan.channel_rate)]
        taps = np.asarray(rec["taps"], dtype=np.float64)
        f["taps"] = taps if len(taps) else np.ones(1)
        f["taps_len"] = len(rec["taps"])
        f["floor_plan"] = rec["floor_plan"]
        if f["profile"] == PROFILE_PROVOICE and len(rec["taps"]) == 63:
            f["label"] = f["label"].replace("PROVOICE", "PROVOICE 63-tap fallback")
        if f["path"] == "encoder" and f["profile"] == PROFILE_WIDE and len(rec["taps"]) == 63:
            f["label"] = f["label"].replace("legacy WIDE", "legacy WIDE 63-tap fallback")
        if f["profile"] == PROFILE_NONE:
            f["protected_hz"] = float(plan.channel_rate)
        elif f["path"] == "encoder":
            f["protected_hz"] = float(rec["legacy_wide_width_hz"] * plan.post_downsample)
        else:
            f["protected_hz"] = 2.0 * float(rec["protected_edge_hz"])
        f["fkey"] = f"{plan.channel_rate}:{plan.hb}:{sm.taps_key(f['taps'])}"
        f["hold_s"] = edacs_hold_s(plan) if f["path"] == "edacs" else VOX_HOLD_S


# --------------------------------------------------------------------------------------------- signals


def edacs_variants(quick: bool) -> list[dict]:
    """EDACS's carrier set: wanted voice at each deviation and offset, sub-audible LSD, and 9600 bit/s data."""
    tone_s = 0.2 if quick else 0.6
    speech_s = 0.6 if quick else 4.0
    variants = []
    for offset in sm.OFFSETS_HZ:
        inch = abs(offset) <= EDACS_IN_CHANNEL_OFFSET_HZ
        variants.append(
            {
                "family": "cw",
                "mod": "none",
                "amount": 0.0,
                "offset": offset,
                "seconds": tone_s,
                "in_channel": inch,
                "wanted": True,
            }
        )
        variants.append(
            {
                "family": "lsd",
                "mod": "lsd",
                "amount": 0.0,
                "offset": offset,
                "seconds": speech_s,
                "in_channel": inch,
                "wanted": True,
            }
        )
        for dev in EDACS_DEVIATIONS_HZ:
            for tone in sm.TONES_HZ:
                variants.append(
                    {
                        "family": "fm",
                        "mod": f"tone{int(tone)}",
                        "amount": dev,
                        "offset": offset,
                        "seconds": tone_s,
                        "in_channel": inch,
                        "wanted": True,
                    }
                )
            for name in sm.REAL_FM:
                variants.append(
                    {
                        "family": "fm",
                        "mod": f"speech:{name}",
                        "amount": dev,
                        "offset": offset,
                        "seconds": speech_s,
                        "in_channel": inch,
                        "wanted": True,
                    }
                )
                variants.append(
                    {
                        "family": "lsd",
                        "mod": f"speech:{name}",
                        "amount": dev,
                        "offset": offset,
                        "seconds": speech_s,
                        "in_channel": inch,
                        "wanted": True,
                    }
                )
        for name in sm.REAL_FM:
            variants.append(
                {
                    "family": "real",
                    "mod": name,
                    "amount": 0.0,
                    "offset": offset,
                    "seconds": speech_s,
                    "in_channel": inch,
                    "wanted": True,
                }
            )
        for dev in EDACS_DATA_DEVIATIONS_HZ:
            for mod in ("data", "dotting"):
                variants.append(
                    {
                        "family": "data",
                        "mod": mod,
                        "amount": dev,
                        "offset": offset,
                        "seconds": tone_s,
                        "in_channel": inch,
                        "wanted": False,
                    }
                )
    return variants


def encoder_variants(protected_hz: float, quick: bool) -> list[dict]:
    """The analog monitor's NFM set (tools/squelch_model.py), every variant wanted."""
    out = []
    for v in sm.carrier_variants(sm.FM_KIND, protected_hz, quick):
        out.append({**v, "wanted": True})
    return out


def nrz(rng: np.random.Generator, bps: float, fs: int, count: int, alternating: bool) -> np.ndarray:
    """A +/-1 NRZ bit stream at bps sampled at fs, smoothed to its first null (a 0.5 BT Gaussian-like shaping)."""
    nbits = math.ceil(count * bps / fs) + 2
    bits = (np.arange(nbits) % 2) * 2.0 - 1.0 if alternating else rng.choice((-1.0, 1.0), nbits)
    idx = np.minimum((np.arange(count) * bps / fs).astype(np.int64), nbits - 1)
    raw = bits[idx]
    return sm.brickwall(raw, fs, 0.0, min(0.75 * bps, 0.45 * fs))


def synth(variant: dict, fs: int, count: int, rng: np.random.Generator) -> np.ndarray:
    """One variant's unit-amplitude complex baseband (tools/squelch_model.py's carriers, plus LSD and data)."""
    family = variant["family"]
    if family in ("cw", "fm", "real", "am"):
        return sm.synth_carrier(variant, fs, count, rng)
    theta0 = rng.uniform(0.0, 2.0 * math.pi)
    if family == "lsd":
        lsd = nrz(rng, LSD_BPS, fs, count, False)
        lsd = lsd / max(1e-9, np.max(np.abs(lsd)))
        freq = LSD_DEVIATION_HZ * lsd
        if variant["mod"].startswith("speech:"):
            audio = sm.source_at_rate(variant["mod"].split(":", 1)[1], "audio", fs)
            start = int(rng.integers(0, max(1, len(audio) - count)))
            freq = freq + (variant["amount"] - LSD_DEVIATION_HZ) * np.resize(audio[start:], count)
    else:
        data = nrz(rng, DATA_BPS, fs, count, variant["mod"] == "dotting")
        freq = variant["amount"] * data / max(1e-9, np.max(np.abs(data)))
    phase = 2.0 * math.pi * np.cumsum(variant["offset"] + freq) / fs
    return np.exp(1j * (phase + theta0))


# --------------------------------------------------------------------------------------------- classification


def classify(sums20: dict, fp: dict) -> np.ndarray:
    """The shipped 40 ms classification of consecutive 20 ms window pairs, with the unsteady-halves rule."""
    s40 = sm.merge_windows(sums20, 2)
    n = s40["n"]
    p = s40["sa"] / n
    cv2 = (s40["sa2"] / n) / (p * p) - 1.0
    x = (cv2 - 1.0) * fp["sqrt_neff"]
    y = np.abs(s40["su"] / n - fp["beta"]) * fp["sqrt_neff"]
    cls = np.full(len(n), UNDECIDED, dtype=np.int8)
    carrier = (x <= CARRIER_X) | (y >= CARRIER_Y)
    noise = (x >= NOISE_X) & (y <= NOISE_Y) & ~carrier
    count = len(n)
    halves = (sums20["sa"][: 2 * count] / sums20["n"][: 2 * count]).reshape(-1, 2)
    unsteady = (halves[:, 1] >= halves[:, 0] * UNSTEADY_RATIO) | (halves[:, 0] >= halves[:, 1] * UNSTEADY_RATIO)
    cls[carrier] = CARRIER
    cls[noise & ~unsteady] = NOISE
    return cls


def noise_job(job: dict) -> dict:
    taps, hb, fs, fp = job["taps"], job["hb"], job["fs"], job["floor_plan"]
    lag = fp["lag"]
    rng = np.random.default_rng(sm.job_seed("paths-noise", job["key"]))
    per_chunk = max(2, 400000 // max(1, fs // sm.WINDOW_PER_S)) // 2 * 2
    counts = np.zeros(3, dtype=np.int64)
    done = 0
    while done < job["n_windows"]:
        n_w = min(per_chunk, job["n_windows"] - done)
        bounds = sm.window_bounds(fs, n_w, lag)
        sums = sm.window_sums(sm.filtered_noise(rng, int(bounds[-1]), taps, hb), bounds, lag)
        counts += np.bincount(classify(sums, fp), minlength=3)
        done += n_w
    return {"kind": "noise", "key": job["key"], "counts": counts.tolist()}


def carrier_job(job: dict) -> dict:
    taps, hb, fs, fp = job["taps"], job["hb"], job["fs"], job["floor_plan"]
    lag = fp["lag"]
    rng = np.random.default_rng(sm.job_seed("paths-carrier", job["key"]))
    variants = (
        edacs_variants(job["quick"]) if job["path"] == "edacs" else encoder_variants(job["protected_hz"], job["quick"])
    )
    rows = []
    for variant in variants:
        n_w = max(2, round(variant["seconds"] * sm.WINDOW_PER_S)) // 2 * 2
        bounds = sm.window_bounds(fs, n_w, lag)
        count = int(bounds[-1])
        s = sm.channel_filter(synth(variant, fs, count + len(taps) - 1, rng), taps)
        noise = sm.filtered_noise(rng, count, taps, hb)
        sig_power = float(np.mean(np.abs(s[lag:].astype(np.complex128)) ** 2))
        for cnr in sm.CNRS_DB:
            gain = np.float32(math.sqrt(sig_power / (job["noise_power"] * 10.0 ** (cnr / 10.0))))
            cls = classify(sm.window_sums(s + gain * noise, bounds, lag), fp)
            rows.append({"variant": variant, "cnr": cnr, "counts": np.bincount(cls, minlength=3).tolist()})
    return {"kind": "carrier", "key": job["key"], "rows": rows}


# --------------------------------------------------------------------------------------------- timing (C tracker)


def track(exe: Path, f: dict, margin: int, iq: np.ndarray) -> np.ndarray:
    taps = f["taps"] if f["taps_len"] > 0 else np.zeros(0)
    header = f"{f['plan'].channel_rate} {margin} {f['hb']} {len(taps)} " + " ".join(f"{t:.9g}" for t in taps) + "\n"
    data = np.empty(2 * len(iq), dtype=np.float32)
    data[0::2] = iq.real
    data[1::2] = iq.imag
    proc = subprocess.run([str(exe), "track"], input=header.encode() + data.tobytes(), capture_output=True, check=True)
    return np.frombuffer(proc.stdout, dtype=np.uint8)


def longest_closed(flags: np.ndarray) -> int:
    closed = (flags & 1).astype(np.int8)
    if not closed.any():
        return 0
    edges = np.diff(np.concatenate(([0], closed, [0])))
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return int(np.max(ends - starts))


def timing_job(job: dict) -> dict:
    f, exe = job["filter"], Path(job["exe"])
    fs = f["plan"].channel_rate
    taps, hb = f["taps"], job["hb"]
    rng = np.random.default_rng(sm.job_seed("paths-timing", job["key"]))
    variants = [
        v
        for v in (
            edacs_variants(job["quick"]) if f["path"] == "edacs" else encoder_variants(f["protected_hz"], job["quick"])
        )
        if v["wanted"] and v["in_channel"]
    ]
    if job["quick"]:
        variants = variants[::7]
    noise_power = job["noise_power"]
    window = fs // 25
    lead = fs  # 1 s of noise ahead of a carrier on a known floor
    out = {
        "key": job["key"],
        "learn_worst_s": 0.0,
        "learn_worst": None,
        "known_worst_s": 0.0,
        "known_worst": None,
        "below_open_worst": 0.0,
        "delay_learn_s": 0.0,
        "delay_known_s": 0.0,
    }

    def carrier_plus_noise(variant, count, cnr=None, cnr_lin=None):
        s = sm.channel_filter(synth(variant, fs, count + len(taps) - 1, rng), taps)
        sig_power = float(np.mean(np.abs(s.astype(np.complex128)) ** 2))
        n = sm.filtered_noise(rng, count, taps, hb)
        ratio = cnr_lin if cnr_lin is not None else 10.0 ** (cnr / 10.0)
        gain = math.sqrt(sig_power / (noise_power * ratio))
        return (s + np.float32(gain) * n) / np.float32(gain)  # noise at its own power: the floor stays put

    for variant in variants:
        count = round(variant["seconds"] * fs)
        for cnr in TIMING_LEARN_CNRS_DB:
            flags = track(exe, f, 10, carrier_plus_noise(variant, count, cnr))
            run = longest_closed(flags) / fs
            if run > out["learn_worst_s"]:
                out["learn_worst_s"], out["learn_worst"] = run, f"{sm.variant_label(variant)} @ {cnr:g} dB"
        for margin in TIMING_MARGINS_DB:
            noise = sm.filtered_noise(rng, lead, taps, hb)
            sig = carrier_plus_noise(variant, count, margin + 3.0)
            flags = track(exe, f, margin, np.concatenate((noise, sig)))
            run = longest_closed(flags[lead:]) / fs
            if run > out["known_worst_s"]:
                out["known_worst_s"], out["known_worst"] = run, f"{sm.variant_label(variant)} @ +{margin} dB"
    dead = {"family": "cw", "mod": "none", "amount": 0.0, "offset": 0.0, "seconds": 1.0}
    for margin in BELOW_MARGINS_DB:
        noise = sm.filtered_noise(rng, lead, taps, hb)
        sig = carrier_plus_noise(dead, fs, cnr_lin=10.0 ** ((margin - 3.0) / 10.0) - 1.0)
        flags = track(exe, f, margin, np.concatenate((noise, sig)))
        opened = float(np.mean((flags[lead + 2 * window :] & 1) == 0))
        out["below_open_worst"] = max(out["below_open_worst"], opened)
    for phase in range(TIMING_DELAY_PHASES):
        drop = fs + (phase * window) // TIMING_DELAY_PHASES
        tail = sm.filtered_noise(rng, fs, taps, hb)
        on = carrier_plus_noise(dead, drop, 20.0)
        flags = track(exe, f, 10, np.concatenate((on, tail)))
        closed = np.flatnonzero(flags[drop:] & 1)
        delay = (closed[0] if len(closed) else fs) / fs
        out["delay_learn_s"] = max(out["delay_learn_s"], delay)
        noise = sm.filtered_noise(rng, lead, taps, hb)
        on = carrier_plus_noise(dead, drop, 20.0)
        tail = sm.filtered_noise(rng, fs, taps, hb)
        flags = track(exe, f, 10, np.concatenate((noise, on, tail)))
        closed = np.flatnonzero(flags[lead + drop :] & 1)
        delay = (closed[0] if len(closed) else fs) / fs
        out["delay_known_s"] = max(out["delay_known_s"], delay)
    return {"kind": "timing", **out}


def run_job(job: dict) -> dict:
    if job["type"] == "noise":
        return noise_job(job)
    if job["type"] == "carrier":
        return carrier_job(job)
    return timing_job(job)


# --------------------------------------------------------------------------------------------- analysis


def class_rate(crows: list[dict], pred, field: int) -> tuple[float | None, int]:
    """The share of the windows of the rows pred selects that read as class field, and their count."""
    num = den = 0
    for r in crows:
        if pred(r):
            c = r["counts"]
            den += sum(c)
            num += c[field]
    return (num / den if den else None), den


def wanted(r: dict) -> bool:
    return r["variant"]["wanted"] and r["variant"]["in_channel"]


def summarize(filters: list[dict], results: dict) -> list[dict]:
    rows = []
    for f in filters:
        nres = results[("noise", f["fkey"])]["counts"]
        ckey = (f["path"], f["fkey"], f["protected_hz"])
        crows = results[("carrier", ckey)]["rows"]
        timing = results[("timing", ckey)]
        total_noise = sum(nres)
        carrier6, n6 = class_rate(crows, lambda r: wanted(r) and r["cnr"] == 6.0, CARRIER)
        poll, _ = class_rate(crows, lambda r: r["variant"]["in_channel"] and r["cnr"] >= 6.0, NOISE)
        edge6, _ = class_rate(
            crows, lambda r: r["variant"]["wanted"] and not r["variant"]["in_channel"] and r["cnr"] == 6.0, CARRIER
        )
        data6, _ = class_rate(crows, lambda r: not r["variant"]["wanted"] and r["cnr"] == 6.0, CARRIER)
        miss3, _ = class_rate(crows, lambda r: wanted(r) and r["cnr"] == 3.0, CARRIER)
        row = {
            "path": f["path"],
            "chain": f["plan"].name,
            "filter": f["label"],
            "taps": f["taps_len"],
            "channel_rate": f["plan"].channel_rate,
            "neff": f["floor_plan"]["neff"],
            "lag": f["floor_plan"]["lag"],
            "plan_valid": f["floor_plan"]["valid"],
            "false_carrier": nres[CARRIER] / total_noise,
            "noise_undecided": 1.0 - nres[NOISE] / total_noise,
            "miss_cnr6": None if carrier6 is None else 1.0 - carrier6,
            "pollution_ge6": poll,
            "miss_edge6": None if edge6 is None else 1.0 - edge6,
            "miss_data6": None if data6 is None else 1.0 - data6,
            "miss_cnr3": None if miss3 is None else 1.0 - miss3,
            "noise_windows": total_noise,
            "carrier6_windows": n6,
            "hold_s": f["hold_s"],
            **{k: v for k, v in timing.items() if k not in ("kind", "key")},
        }
        reasons = []
        if not row["plan_valid"]:
            reasons.append("no valid plan")
        if row["false_carrier"] > sm.FALSE_CARRIER_TARGET:
            reasons.append(f"noise read as carrier {row['false_carrier']:.2e}")
        if row["miss_cnr6"] is not None and row["miss_cnr6"] > sm.MISS_TARGET:
            reasons.append(f"6 dB carrier missed {row['miss_cnr6']:.2%}")
        if row["pollution_ge6"] is not None and row["pollution_ge6"] > sm.POLLUTION_TARGET:
            reasons.append(f"carrier read as noise {row['pollution_ge6']:.2e}")
        if row["noise_undecided"] > sm.NOISE_UNDECIDED_TARGET:
            reasons.append(f"noise undecided {row['noise_undecided']:.1%}")
        if row["learn_worst_s"] >= row["hold_s"]:
            reasons.append(f"closed {row['learn_worst_s'] * 1000:.0f} ms while learning ({row['learn_worst']})")
        if row["known_worst_s"] >= row["hold_s"]:
            reasons.append(f"closed {row['known_worst_s'] * 1000:.0f} ms over the threshold ({row['known_worst']})")
        if row["below_open_worst"] > BELOW_OPEN_LIMIT:
            reasons.append(f"opened {row['below_open_worst']:.1%} under the threshold")
        if f["path"] == "edacs" and max(row["delay_learn_s"], row["delay_known_s"]) * 1000.0 > L_MAX_MS:
            reasons.append(f"closing delay {max(row['delay_learn_s'], row['delay_known_s']) * 1000:.0f} ms")
        accepted = ACCEPTED.get((f["path"], f["plan"].name, f["label"]))
        row["reasons"] = reasons
        row["accepted"] = accepted if reasons and accepted else None
        row["pass"] = not reasons or row["accepted"] is not None
        rows.append(row)
    return rows


def verdict_text(r: dict) -> str:
    if r["accepted"]:
        return f"accepted ({'; '.join(r['reasons'])}): {r['accepted']}"
    return "pass" if r["pass"] else "FAIL: " + "; ".join(r["reasons"])


def fmt(v, spec):
    return "-" if v is None else format(v, spec)


def write_report(out_dir: Path, meta: dict, rows: list[dict]) -> tuple[Path, Path]:
    lines = [
        "# Auto squelch on EDACS analog voice and the M17 encoder (issue #625)",
        "",
        (
            f"Command: `{meta['command']}` at {meta['git']} ({meta['mode']}, {meta['noise_windows']} noise windows of "
            f"20 ms per filter, {meta['elapsed_s']:.0f} s)."
        ),
        "",
        (
            "Shipped thresholds: CARRIER at X <= -7.0 or Y >= 3.3, NOISE at X >= -3.8 and Y <= 2.1 (40 ms windows, the "
            "3 dB halves rule). Targets per window: noise read as carrier <= 1e-4, 6 dB carrier missed <= 1e-2, "
            f"carrier (>= 6 dB, data included) read as noise <= 1e-4, noise undecided <= 0.25. EDACS budgets "
            f"{L_MAX_MS:g} ms of closing delay; holds are EDACS's 4 triplets less that, and the VOX's 440 ms."
        ),
        "",
    ]
    for path in ("edacs", "encoder"):
        sub = [r for r in rows if r["path"] == path]
        passing = sum(1 for r in sub if r["pass"])
        lines += [
            (
                f"## {'EDACS analog voice (9600/2 FSK path)' if path == 'edacs' else 'M17 encoder monitor'}: "
                f"{passing}/{len(sub)} pass"
            ),
            "",
            (
                "| chain | filter | taps | N_eff | noise->carrier | 6 dB miss | ->noise >=6 dB | noise undecided | "
                "3 dB miss | edge 6 dB miss | data 6 dB miss | longest closed learning | longest closed known | "
                "open below | delay learn/known | hold | verdict |"
            ),
            "|" + " --- |" * 17,
        ]
        for r in sub:
            lines.append(
                f"| {r['chain']} | {r['filter']} | {r['taps']} | {r['neff']:.0f} | {r['false_carrier']:.1e} | "
                f"{fmt(r['miss_cnr6'], '.2%')} | {fmt(r['pollution_ge6'], '.1e')} | {r['noise_undecided']:.1%} | "
                f"{fmt(r['miss_cnr3'], '.1%')} | {fmt(r['miss_edge6'], '.1%')} | {fmt(r['miss_data6'], '.1%')} | "
                f"{r['learn_worst_s'] * 1000:.0f} ms | {r['known_worst_s'] * 1000:.0f} ms | "
                f"{r['below_open_worst']:.2%} | {r['delay_learn_s'] * 1000:.0f}/{r['delay_known_s'] * 1000:.0f} ms | "
                f"{r['hold_s'] * 1000:.0f} ms | {verdict_text(r)} |"
            )
        lines.append("")
    md = out_dir / "report.md"
    md.write_text("\n".join(lines))
    js = out_dir / "report.json"
    js.write_text(json.dumps({"meta": meta, "rows": rows}, indent=1, default=str))
    return md, js


# --------------------------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--quick", action="store_true", help="short run: fewer noise windows, shorter carriers")
    parser.add_argument("--out", type=Path, default=sm.ROOT / "build" / "squelch_paths_model", help="report directory")
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=sm.ROOT / "build" / "dev-debug",
        help="configured and built tree whose static libraries the tap harness links",
    )
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1), help="worker processes")
    parser.add_argument(
        "--noise-windows",
        type=int,
        default=0,
        help=f"20 ms noise windows per filter (default {sm.FULL_NOISE_WINDOWS}, {sm.QUICK_NOISE_WINDOWS} with --quick)",
    )
    args = parser.parse_args(argv)
    noise_windows = args.noise_windows or (sm.QUICK_NOISE_WINDOWS if args.quick else sm.FULL_NOISE_WINDOWS)
    noise_windows = max(2, noise_windows // 2 * 2)
    started = time.time()
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    exe = sm.build_harness(args.build_dir.resolve(), out_dir)
    header, _ = sm.run_harness(exe, [])
    hb_taps = {name: np.asarray(header[name], dtype=np.float64) for name in ("hb15", "hb31")}
    filters = path_filters(sm.rate_plans())
    run_profile_harness(exe, filters)
    sm.real_sources()  # fail early on a missing fixture

    jobs, seen = [], set()
    for f in filters:
        plan = f["plan"]
        hb = hb_taps.get(plan.hb) if plan.hb else None
        info = sm.filter_info(f["taps"], hb, plan.channel_rate)
        base = {
            "taps": f["taps"],
            "hb": hb,
            "fs": plan.channel_rate,
            "floor_plan": f["floor_plan"],
            "quick": args.quick,
            "noise_power": info["noise_power"],
        }
        if ("noise", f["fkey"]) not in seen:
            seen.add(("noise", f["fkey"]))
            jobs.append({**base, "type": "noise", "key": f["fkey"], "n_windows": noise_windows})
        ckey = (f["path"], f["fkey"], f["protected_hz"])
        if ("carrier", ckey) not in seen:
            seen.add(("carrier", ckey))
            jobs.append({**base, "type": "carrier", "key": ckey, "path": f["path"], "protected_hz": f["protected_hz"]})
            jobs.append({**base, "type": "timing", "key": ckey, "filter": f, "exe": str(exe)})
    jobs.sort(key=lambda j: -j["fs"] * (2 * j.get("n_windows", 0) + 15000))
    print(f"{len(filters)} path filters, {len(jobs)} jobs, {args.jobs} workers", flush=True)
    results = {}
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for done, res in enumerate(pool.map(run_job, jobs, chunksize=1), start=1):
            results[(res["kind"], res["key"] if res["kind"] == "noise" else tuple(res["key"]))] = res
            if done % 20 == 0 or done == len(jobs):
                print(f"  {done}/{len(jobs)} jobs ({time.time() - started:.0f} s)", flush=True)
    rows = summarize(filters, results)
    command = ["python3", "tools/squelch_paths_model.py", *(sys.argv[1:] if argv is None else argv)]
    meta = {
        "command": " ".join(command),
        "git": sm.git_describe(),
        "mode": "quick" if args.quick else "full",
        "noise_windows": noise_windows,
        "scipy": scipy.__version__,
        "python": platform.python_version(),
        "elapsed_s": time.time() - started,
        "l_max_ms": L_MAX_MS,
    }
    md, js = write_report(out_dir, meta, rows)
    failing = [r for r in rows if not r["pass"]]
    for path in ("edacs", "encoder"):
        sub = [r for r in rows if r["path"] == path]
        print(f"{path}: {sum(1 for r in sub if r['pass'])}/{len(sub)} plans pass")
    for r in failing:
        print(f"  FAIL {r['path']} {r['chain']} {r['filter']}: {'; '.join(r['reasons'])}")
    for r in rows:
        if r["accepted"]:
            print(f"  accepted {r['path']} {r['chain']} {r['filter']}: {'; '.join(r['reasons'])}")
    print(f"verdict: the shipped classifier {'holds' if not failing else 'does NOT hold'} on every plan")
    print(f"report: {md}\n        {js}")
    return 0 if not failing else 1


if __name__ == "__main__":
    sys.exit(main())
