#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Design gate for the auto squelch on two more paths (issue #625): EDACS analog voice and the M17 encoder's monitor.

The auto squelch's classifier was chosen by tools/squelch_model.py on the channel plans the analog monitor runs. Issue
#625 runs the same tracker, with its shipped thresholds unchanged, on two more paths:

  - EDACS analog voice. EDACS keeps the stream on the FSK discriminator at 9600 bit/s, two levels, so the plan is the
    PROVOICE profile's (designed within the 288-tap cap, or its 63-tap fallback), no channel filter at all (the channel
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
  - The M17 encoder's monitor: the legacy WIDE profile plan (288-tap cap, 63-tap fallback) off the analog family, or no
    filter, at every rate chain, with the analog monitor's own NFM carrier set.

It evaluates the shipped classifier exactly (src/dsp/squelch_floor.c), with tools/squelch_model.py's frozen-threshold
port: 40 ms windows of two 20 ms sub-windows whose boundaries fall at floor(k fs / 50) samples from the stream's start,
X = (CV^2 - 1) sqrt(N_eff) and Y = |mean(u_k conj(u_{k-L})) - beta| sqrt(N_eff), CARRIER at X <= -7.0 or Y >= 3.3,
NOISE at X >= -3.8 and Y <= 2.1, and a NOISE window whose 20 ms halves differ by 3 dB or more read as UNDECIDED, each
window decided before anything is counted. The targets are tools/squelch_model.py's four, per window: noise read as
carrier <= 1e-4, a 6 dB wanted carrier not read as carrier <= 1e-2, a carrier (wanted or data) of 6 dB or more read as
noise <= 1e-4, noise not read as noise <= 0.25.

The chains are tools/squelch_model.py's production chain identities (issue #899): the RTL-SDR bandwidths at their
capture rates through the 4 to 8 half-band passes they run, the I/Q replay chains at their recorded passes, and the
replay reader's chains (its resampler, then the passes). Noise and carriers are made at the chain's source rate and run
through the chain's own stages (numpy ports, every stage's history and the windows' boundaries kept across chunks),
CNR set at the channel filter's output. The plan each filter is classified with is the one full_demod() installs on
its path (tools/squelch_model_taps.cpp runs the FSK path or the encoder's monitor on that chain, a reader row through
the reader), checked against the chain model's. A reader row's wanted variants must fit the band the source carries
(the reader's pass edge). Stepped chains (Airspy, Airspy Mini,
RX-888/SDDC: the passes, then the live step) are scoped out: there is no port of the live step here, and
DSP_SQUELCH_STEP_MODEL holds the real step.

Timing checks run the C tracker itself (squelch_model_taps track, given the plan) on each distinct plan, on carriers
made through the chain as above (each variant's once, mixed at every CNR and margin) and noise drawn through the chain
afresh for every case:
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
The holds: EDACS releases after a closed run of 4 triplets (4 x 2880 samples at 48 kHz, the rate it runs analog
voice at whatever rate it reads, issue #633) less L_MAX_MS, never under EDACS_MIN_HOLD_MS (two windows). The
encoder's VOX releases after 11 closed 40 ms reads (440 ms).

Usage:
    python3 tools/squelch_paths_model.py [--quick] [--out DIR] [--build-dir build/dev-debug] [--jobs N]
                                         [--noise-windows N] [--segments N] [--chains TEXT]

Writes report.md and report.json under DIR (default build/squelch_paths_model/) and prints the verdict; exits 1 when a
plan either path runs misses a target that its ACCEPTED_MARGINAL entry (one plan, one bound, an allowance just above
the value measured) does not accept; the accepted plans are printed with why. --quick runs in about 10 minutes, too
few windows to resolve 1e-4. Offline; CI does not run it.
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
import squelch_chain_model as cm
import squelch_model as sm  # noqa: E402

# The shipped classifier, frozen (src/dsp/squelch_floor.c): tools/squelch_model.py's port decides every window.
UNDECIDED, NOISE, CARRIER = cm.UNDECIDED, cm.NOISE, cm.CARRIER

PROFILE_WIDE = 0  # DSD_CH_LPF_PROFILE_WIDE
PROFILE_PROVOICE = 3  # DSD_CH_LPF_PROFILE_PROVOICE
PROFILE_NONE = -2  # the harness's "no channel filter"
HARNESS_PATH_ENCODER = 1  # tools/squelch_model_taps.cpp's paths: the M17 encoder's monitor
HARNESS_PATH_FSK = 2  # the 9600 bit/s two-level FSK path

EDACS_DEVIATIONS_HZ = (2500.0, 4000.0, 4500.0, 5000.0)
EDACS_DATA_DEVIATIONS_HZ = (2500.0, 3500.0, 4500.0)
EDACS_IN_CHANNEL_OFFSET_HZ = 1000.0
LSD_BPS = 150.0
LSD_DEVIATION_HZ = 750.0
DATA_BPS = 9600.0

# EDACS's hold (src/protocol/edacs/edacs-fme.c): four triplets of 2880 samples at 48 kHz (EDACS_ANALOG_RATE_HZ, which
# it converts every input rate to), less the closing delay it budgets (L_MAX_MS, DSD_SQUELCH_CLOSE_DELAY_MS in
# include/dsd-neo/core/power.h). The encoder's VOX: 11 closed 40 ms reads.
EDACS_MIN_CHANNEL_RATE_HZ = 12000
EDACS_RATE_HZ = 48000
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

# Known marginal plans the maintainer accepted after the frozen-threshold run of issue #899 (docs/testing.md): each
# entry is one plan (path, chain, filter) and the one bound it misses, accepted up to an allowance just above what that
# run measured (the 95 % interval's upper end for a rate; one 20 ms sub-window over the measured closed run), and why.
# Any other bound of the plan, a value past the allowance, or any other plan still fails the gate. Stepped chains
# (the SDDC's included, once accepted here at its passes-only rate) are scoped out of this run.
_TAIL = (
    "an unfiltered replay with no pass: its plan and noise are unchanged by issue #899, a noise->carrier tail near 1e-4"
)
_EDACS_12K = (
    "the channel LPF forced on at the 12 kHz RTL rate: the real 7-pass cascade and the filter truncate a 5 kHz-deviation "
    "carrier 1 kHz off centre, so a fresh tracker on it at 6 dB stays closed past the hold; EDACS presets run 24 kHz "
    "and the default 12 kHz chain (channel LPF off) passes"
)
ACCEPTED_MARGINAL = {
    ("edacs", "rtl-12k", "PROVOICE, DSD_NEO_CHANNEL_LPF=1"): {
        "learn_worst_s": (0.62, f"{_EDACS_12K}; measured 600 ms (fm tone300 dev 5000 Hz offset +1000 Hz @ 6 dB)")
    },
    ("edacs", "rtl-12k", "WIDE (live switch), DSD_NEO_CHANNEL_LPF=1"): {
        "learn_worst_s": (
            0.38,
            (
                f"{_EDACS_12K}; a plan only DSD_NEO_CHANNEL_LPF=1 makes; measured 360 ms (the same carrier) in the "
                "first full run with fresh timing noise per case and the decoder's half-band centring, 240 ms before; "
                "no other bound missed"
            ),
        )
    },
    ("encoder", "rtl-8k", "legacy WIDE, DSD_NEO_CHANNEL_LPF=1"): {
        "miss_cnr6": (
            0.026,
            (
                "the channel LPF forced on at the 8 kHz RTL rate, where the legacy WIDE design is all the channel "
                "there is; measured 1.26 % (7 / 555; 95 % 0.51 %..2.58 %), as the analog monitor's same plan "
                "(tools/squelch_model.py)"
            ),
        )
    },
    ("edacs", "replay-125k", "none (channel LPF off), DSD_NEO_CHANNEL_LPF=0"): {
        "false_carrier": (2.4e-4, f"{_TAIL}; measured 1.4e-4 (14 / 100,000; 95 % 7.7e-5..2.3e-4)")
    },
    ("encoder", "replay-125k", "none (channel LPF off), DSD_NEO_CHANNEL_LPF=0"): {
        "false_carrier": (2.4e-4, f"{_TAIL}; measured 1.4e-4 (14 / 100,000; 95 % 7.7e-5..2.3e-4)")
    },
    ("encoder", "replay-96k/pd2", "none (channel LPF off), DSD_NEO_CHANNEL_LPF=0"): {
        "false_carrier": (2.8e-4, f"{_TAIL}; measured 1.7e-4 (17 / 100,000; 95 % 9.9e-5..2.7e-4)")
    },
}


# --------------------------------------------------------------------------------------------- plans


def edacs_hold_s() -> float:
    """EDACS's hold, the same at every channel rate: it converts what it reads to 48 kHz."""
    hold = EDACS_HOLD_TRIPLETS * EDACS_TRIPLET_SAMPLES / EDACS_RATE_HZ - L_MAX_MS / 1000.0
    return max(hold, EDACS_MIN_HOLD_MS / 1000.0)


def path_filters(plans: list[cm.RatePlan]) -> list[dict]:
    """Every (path, chain, filter) the two paths run: the filter as a harness request. Stepped chains are scoped out
    (tools/squelch_model.py has no port of the live step; DSP_SQUELCH_STEP_MODEL holds it)."""
    out = []
    for plan in plans:
        if plan.stepped:
            continue
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
                            "env": env,
                            "label": f"PROVOICE{label_env}",
                        }
                    )
                    out.append(
                        {
                            "path": "edacs",
                            "plan": plan,
                            "profile": PROFILE_WIDE,
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
                            "env": env,
                            "label": f"none (channel LPF off){label_env}",
                        }
                    )
            out.append(
                {
                    "path": "encoder",
                    "plan": plan,
                    "profile": PROFILE_WIDE if enable else PROFILE_NONE,
                    "env": env,
                    "label": ("legacy WIDE" if enable else "none (channel LPF off)") + label_env,
                }
            )
    return out


def run_profile_harness(exe: Path, filters: list[dict], hb_taps: dict) -> None:
    """Fill each filter's taps, protected width and plans: the one full_demod() installs on its path and chain (the
    decoder's, which each filter is classified with; a reader row's through the reader) and the chain model's
    (tools/squelch_model.py), held to it."""
    harness_path = {"edacs": HARNESS_PATH_FSK, "encoder": HARNESS_PATH_ENCODER}

    def request(f: dict) -> tuple:
        plan = f["plan"]
        reader = plan.reader.source_hz if plan.reader else 0
        return (plan.complex_rate, harness_path[f["path"]], f["profile"], plan.passes, reader)

    requests = sorted({request(f) for f in filters})
    stdin = "".join(
        f"{rate} 0 0 {path} {profile} {passes} {reader}\n" for rate, path, profile, passes, reader in requests
    )
    proc = subprocess.run([str(exe)], input=stdin, capture_output=True, text=True, check=True)
    recs = {}
    for line in proc.stdout.splitlines()[1:]:
        rec = json.loads(line)
        if not rec["installed"]:
            raise SystemExit(f"the tracker did not run on {rec['rate_out']} Hz path {rec['path']}")
        recs[(rec["rate_out"], rec["path"], rec["profile"], rec["passes"], rec["reader_source_hz"])] = rec
    models: dict = {}
    for f in filters:
        plan = f["plan"]
        rec = recs[request(f)]
        taps = np.asarray(rec["taps"], dtype=np.float64)
        f["taps"] = taps if len(taps) else np.ones(1)
        f["taps_len"] = len(rec["taps"])
        f["decoder_plan"] = rec["floor_plan"]
        if f["profile"] == PROFILE_PROVOICE and len(rec["taps"]) == 63:
            f["label"] = f["label"].replace("PROVOICE", "PROVOICE 63-tap fallback")
        if f["path"] == "encoder" and f["profile"] == PROFILE_WIDE and len(rec["taps"]) == 63:
            f["label"] = f["label"].replace("legacy WIDE", "legacy WIDE 63-tap fallback")
        if f["profile"] == PROFILE_NONE:
            f["protected_hz"] = float(plan.channel_rate)
        elif f["path"] == "encoder":
            f["protected_hz"] = float(rec["legacy_wide_width_hz"])
        else:
            f["protected_hz"] = 2.0 * float(rec["protected_edge_hz"])
        f["fkey"] = f"{plan.chain_key}:{cm.taps_key(f['taps'])}"
        cov, model = cm.filter_model(plan, f["taps"], hb_taps, models)
        f["model_plan"] = model
        f["noise_power"] = float(np.mean(cov[:, 0]))
        f["floor_plan"] = f["decoder_plan"]
        f["model_off"] = cm.plan_distance(model, f["decoder_plan"])
        f["band_half_hz"] = plan.reader.pass_hz if plan.reader else None
        f["hold_s"] = edacs_hold_s() if f["path"] == "edacs" else VOX_HOLD_S


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


def encoder_variants(protected_hz: float, quick: bool, band_half_hz: float | None = None) -> list[dict]:
    """The analog monitor's NFM set (tools/squelch_model.py), every variant wanted; on a reader row a variant past the
    reader's pass edge is an edge variant."""
    half = protected_hz / 2.0 if band_half_hz is None else min(protected_hz / 2.0, band_half_hz)
    out = []
    for v in sm.carrier_variants(sm.FM_KIND, protected_hz, quick, half):
        out.append({**v, "wanted": True})
    return out


def occupied_half_hz(v: dict) -> float:
    """How far from the centre a variant's band reaches."""
    off = abs(v["offset"])
    if v["family"] == "cw":
        return off
    if v["family"] == "real":
        return off + sm.real_sources()[v["mod"]]["half99_hz"]
    if v["family"] == "data":
        return off + v["amount"] + 0.75 * DATA_BPS
    if v["family"] == "lsd" and v["mod"] == "lsd":
        return off + LSD_DEVIATION_HZ + LSD_BPS
    top = float(v["mod"][4:]) if v["mod"].startswith("tone") else sm.VOICE_TOP_HZ
    return off + v["amount"] + top


def path_variants(f: dict, quick: bool) -> list[dict]:
    """A filter's carrier set; on a reader row, qualified against the band the source carries too."""
    if f["path"] == "encoder":
        return encoder_variants(f["protected_hz"], quick, f["band_half_hz"])
    variants = edacs_variants(quick)
    if f["band_half_hz"] is not None:
        for v in variants:
            v["in_channel"] = v["in_channel"] and occupied_half_hz(v) <= f["band_half_hz"]
    return variants


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


def carrier_job(job: dict) -> dict:
    """Every variant at every CNR through the chain (tools/squelch_model.py's generator: made at the source rate, run
    through the chain and the channel filter, mixed at the channel output), each mix a stream of its own decided by
    the shipped rule."""
    f, hb = job["filter"], job["hb"]
    plan, taps, fp = f["plan"], f["taps"], f["floor_plan"]
    rate = plan.complex_rate
    rng = np.random.default_rng(sm.job_seed("paths-carrier", job["key"]))
    rows = []
    for variant in path_variants(f, job["quick"]):
        n_sub = 2 * max(1, round(variant["seconds"] * cm.WINDOW_PER_S / 2))
        outputs = (n_sub * rate) // cm.WINDOW_PER_S
        s = cm.chain_signal(plan, hb, taps, lambda n, v=variant: synth(v, plan.source_hz, n, rng), outputs)
        noise = cm.chain_noise(plan, hb, taps, rng, outputs)
        sig_power = float(np.mean(np.abs(s[fp["lag"] :]) ** 2))
        for cnr in sm.CNRS_DB:
            gain = math.sqrt(sig_power / (f["noise_power"] * 10.0 ** (cnr / 10.0)))
            cls = cm.frozen_classes(s + gain * noise, rate, fp)
            rows.append({"variant": variant, "cnr": cnr, "counts": np.bincount(cls, minlength=3).tolist()})
    return {"kind": "carrier", "key": job["key"], "rows": rows}


# --------------------------------------------------------------------------------------------- timing (C tracker)


def track(exe: Path, f: dict, margin: int, iq: np.ndarray) -> np.ndarray:
    """The C tracker (squelch_model_taps track) on @p iq with the filter's plan."""
    p = f["floor_plan"]
    header = (
        f"{margin} {p['rate_hz']} {p['lag']} {p['rho_lag']:.17g} {p['beta']:.17g} {p['neff']:.17g} "
        f"{p['sqrt_neff']:.17g} {p['noise_gain']:.17g}\n"
    )
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
    plan = f["plan"]
    fs = plan.channel_rate
    taps, hb = f["taps"], job["hb"]
    rng = np.random.default_rng(sm.job_seed("paths-timing", job["key"]))
    variants = [v for v in path_variants(f, job["quick"]) if v["wanted"] and v["in_channel"]]
    if job["quick"]:
        variants = variants[::7]
    noise_power = f["noise_power"]
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

    # Every case draws noise of its own through the chain (a fresh stream each time); each variant's carrier goes
    # through the chain once and is mixed at each CNR and margin.
    def noise_only(count):
        return cm.chain_noise(plan, hb, taps, rng, count)

    def carrier_of(variant, count):
        s = cm.chain_signal(plan, hb, taps, lambda n: synth(variant, plan.source_hz, n, rng), count)
        return s, float(np.mean(np.abs(s) ** 2))

    def plus_noise(carrier, count, cnr=None, cnr_lin=None):
        s, sig_power = carrier[0][:count], carrier[1]
        ratio = cnr_lin if cnr_lin is not None else 10.0 ** (cnr / 10.0)
        gain = math.sqrt(sig_power / (noise_power * ratio))
        return s / gain + noise_only(count)  # noise at its own power: the floor stays put

    for variant in variants:
        count = round(variant["seconds"] * fs)
        carrier = carrier_of(variant, count)
        for cnr in TIMING_LEARN_CNRS_DB:
            flags = track(exe, f, 10, plus_noise(carrier, count, cnr))
            run = longest_closed(flags) / fs
            if run > out["learn_worst_s"]:
                out["learn_worst_s"], out["learn_worst"] = run, f"{sm.variant_label(variant)} @ {cnr:g} dB"
        for margin in TIMING_MARGINS_DB:
            sig = plus_noise(carrier, count, margin + 3.0)
            flags = track(exe, f, margin, np.concatenate((noise_only(lead), sig)))
            run = longest_closed(flags[lead:]) / fs
            if run > out["known_worst_s"]:
                out["known_worst_s"], out["known_worst"] = run, f"{sm.variant_label(variant)} @ +{margin} dB"
    dead = carrier_of({"family": "cw", "mod": "none", "amount": 0.0, "offset": 0.0, "seconds": 1.0}, fs + window)
    for margin in BELOW_MARGINS_DB:
        sig = plus_noise(dead, fs, cnr_lin=10.0 ** ((margin - 3.0) / 10.0) - 1.0)
        flags = track(exe, f, margin, np.concatenate((noise_only(lead), sig)))
        opened = float(np.mean((flags[lead + 2 * window :] & 1) == 0))
        out["below_open_worst"] = max(out["below_open_worst"], opened)
    for phase in range(TIMING_DELAY_PHASES):
        drop = fs + (phase * window) // TIMING_DELAY_PHASES
        flags = track(exe, f, 10, np.concatenate((plus_noise(dead, drop, 20.0), noise_only(fs))))
        closed = np.flatnonzero(flags[drop:] & 1)
        delay = (closed[0] if len(closed) else fs) / fs
        out["delay_learn_s"] = max(out["delay_learn_s"], delay)
        flags = track(exe, f, 10, np.concatenate((noise_only(lead), plus_noise(dead, drop, 20.0), noise_only(fs))))
        closed = np.flatnonzero(flags[lead + drop :] & 1)
        delay = (closed[0] if len(closed) else fs) / fs
        out["delay_known_s"] = max(out["delay_known_s"], delay)
    return {"kind": "timing", **out}


def run_job(job: dict) -> dict:
    if job["type"] == "noise":
        return sm.noise_job(job)
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
            "false_carrier_events": int(nres[CARRIER]),
            "false_carrier_hi": sm.clopper_pearson(int(nres[CARRIER]), total_noise)[1],
            "carrier6_windows": n6,
            "plan_source": "decoder",
            "model_off": f["model_off"],
            "hold_s": f["hold_s"],
            **{k: v for k, v in timing.items() if k not in ("kind", "key")},
        }
        row_verdict(row)
        rows.append(row)
    return rows


def row_failures(row: dict) -> list[tuple[str | None, str]]:
    """The bounds a summarized plan misses: (the ACCEPTED_MARGINAL key the miss can be accepted under, or None, text)."""
    out = []
    if not row["plan_valid"]:
        out.append((None, "no valid plan"))
    if row["model_off"] is not None and row["model_off"] > 1e-6:
        out.append((None, f"the model's plan is {row['model_off']:.2e} off the decoder's"))
    if row["false_carrier"] > sm.FALSE_CARRIER_TARGET:
        out.append(("false_carrier", f"noise read as carrier {row['false_carrier']:.2e}"))
    if row["miss_cnr6"] is not None and row["miss_cnr6"] > sm.MISS_TARGET:
        out.append(("miss_cnr6", f"6 dB carrier missed {row['miss_cnr6']:.2%}"))
    if row["pollution_ge6"] is not None and row["pollution_ge6"] > sm.POLLUTION_TARGET:
        out.append((None, f"carrier read as noise {row['pollution_ge6']:.2e}"))
    if row["noise_undecided"] > sm.NOISE_UNDECIDED_TARGET:
        out.append((None, f"noise undecided {row['noise_undecided']:.1%}"))
    if row["learn_worst_s"] >= row["hold_s"]:
        out.append(
            ("learn_worst_s", f"closed {row['learn_worst_s'] * 1000:.0f} ms while learning ({row['learn_worst']})")
        )
    if row["known_worst_s"] >= row["hold_s"]:
        out.append((None, f"closed {row['known_worst_s'] * 1000:.0f} ms over the threshold ({row['known_worst']})"))
    if row["below_open_worst"] > BELOW_OPEN_LIMIT:
        out.append((None, f"opened {row['below_open_worst']:.1%} under the threshold"))
    if row["path"] == "edacs" and max(row["delay_learn_s"], row["delay_known_s"]) * 1000.0 > L_MAX_MS:
        out.append((None, f"closing delay {max(row['delay_learn_s'], row['delay_known_s']) * 1000:.0f} ms"))
    return out


def row_verdict(row: dict) -> dict:
    """Sets a summarized plan's reasons, its acceptance (every miss one its ACCEPTED_MARGINAL entry accepts, within the
    allowance) and its pass flag; works on the rows of a saved report.json too."""
    failures = row_failures(row)
    row["reasons"] = [text for _, text in failures]
    entry = ACCEPTED_MARGINAL.get((row["path"], row["chain"], row["filter"]))
    row["listed"] = entry is not None
    row["accepted"] = None
    if failures and entry and all(k in entry and row[k] <= entry[k][0] for k, _ in failures):
        row["accepted"] = "; ".join(f"{k} accepted up to {entry[k][0]:g}: {entry[k][1]}" for k, _ in failures)
    row["pass"] = not failures or row["accepted"] is not None
    return row


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
                "| chain | filter | taps | plan from | N_eff | noise->carrier (events / windows; 95 % upper) | "
                "6 dB miss | ->noise >=6 dB | noise undecided | 3 dB miss | edge 6 dB miss | data 6 dB miss | "
                "longest closed learning | longest closed known | open below | delay learn/known | hold | verdict |"
            ),
            "|" + " --- |" * 18,
        ]
        for r in sub:
            lines.append(
                f"| {r['chain']} | {r['filter']} | {r['taps']} | {r['plan_source']} | {r['neff']:.0f} | "
                f"{r['false_carrier']:.1e} ({r['false_carrier_events']} / {r['noise_windows']}; "
                f"{r['false_carrier_hi']:.1e}) | "
                f"{fmt(r['miss_cnr6'], '.2%')} | {fmt(r['pollution_ge6'], '.1e')} | {r['noise_undecided']:.1%} | "
                f"{fmt(r['miss_cnr3'], '.1%')} | {fmt(r['miss_edge6'], '.1%')} | {fmt(r['miss_data6'], '.1%')} | "
                f"{r['learn_worst_s'] * 1000:.0f} ms | {r['known_worst_s'] * 1000:.0f} ms | "
                f"{r['below_open_worst']:.2%} | {r['delay_learn_s'] * 1000:.0f}/{r['delay_known_s'] * 1000:.0f} ms | "
                f"{r['hold_s'] * 1000:.0f} ms | {verdict_text(r)} |"
            )
        lines.append("")
    listed = [r for r in rows if r["listed"]]
    lines += ["## Accepted marginal plans", ""]
    for r in listed:
        state = f"accepted ({'; '.join(r['reasons'])})" if r["accepted"] else verdict_text(r)
        entry = ACCEPTED_MARGINAL[(r["path"], r["chain"], r["filter"])]
        why = "; ".join(f"{k} up to {a:g}: {reason}" for k, (a, reason) in entry.items())
        lines.append(f"- {r['path']} {r['chain']} {r['filter']}: {state}. Entry: {why}")
    lines.append("")
    lines += [
        (
            "Stepped chains (Airspy, Airspy Mini, RX-888/SDDC) are scoped out: tools/squelch_model.py has no port of "
            f"the live step, and DSP_SQUELCH_STEP_MODEL holds the real step instead ({', '.join(meta['scoped'])})."
        ),
        "",
    ]
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
    parser.add_argument("--segments", type=int, default=4, help="independent noise streams per chain (in parallel)")
    parser.add_argument(
        "--chains", default="", help="run only the chains whose name contains this text (a partial run, same seeds)"
    )
    args = parser.parse_args(argv)
    noise_windows = args.noise_windows or (sm.QUICK_NOISE_WINDOWS if args.quick else sm.FULL_NOISE_WINDOWS)
    noise_windows = max(2, noise_windows // 2 * 2)
    started = time.time()
    plans = sm.rate_plans()
    filters = [f for f in path_filters(plans) if args.chains in f["plan"].name]
    if not filters:
        # A mistyped name, or stepped chains only (path_filters() scopes them out): nothing would be tested.
        matched = sorted(p.name for p in plans if args.chains in p.name)
        raise SystemExit(
            f"--chains {args.chains!r} selects no path filter (chains matched: {', '.join(matched) or 'none'}; "
            "stepped chains are scoped out)"
        )
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    exe = sm.build_harness(args.build_dir.resolve(), out_dir)
    header, _ = sm.run_harness(exe, [])
    hb_taps = {name: np.asarray(header[name], dtype=np.float64) for name in ("hb15", "hb31")}
    run_profile_harness(exe, filters, hb_taps)
    sm.real_sources()  # fail early on a missing fixture

    jobs, seen, chains = [], set(), {}
    for f in filters:
        plan = f["plan"]
        entry = chains.setdefault(plan.chain_key, {"plan": plan, "filters": {}})
        entry["filters"].setdefault(
            f["fkey"],
            {
                "fkey": f["fkey"],
                "taps": f["taps"],
                "lag": f["floor_plan"]["lag"],
                "plan": f["floor_plan"],
                "norm": None,
            },
        )
        ckey = (f["path"], f["fkey"], f["protected_hz"])
        if ckey not in seen:
            seen.add(ckey)
            base = {"key": ckey, "filter": f, "hb": hb_taps, "quick": args.quick, "cost": plan.source_hz * 3000}
            jobs.append({**base, "type": "carrier"})
            jobs.append({**base, "type": "timing", "exe": str(exe)})
    split = sm.segment_windows(noise_windows, args.segments)
    for entry in chains.values():
        for segment, per in enumerate(split):
            jobs.append(
                {
                    "type": "noise",
                    "plan": entry["plan"],
                    "hb": hb_taps,
                    "filters": list(entry["filters"].values()),
                    "n_windows": per,
                    "segment": ("paths", segment),
                    "frozen": True,
                    "cost": entry["plan"].source_hz * (len(entry["filters"]) + 4) * per,
                }
            )
    jobs.sort(key=lambda j: -j["cost"])
    scoped = sorted({p.name for p in plans if p.stepped})
    print(
        f"{len(filters)} path filters, {len(jobs)} jobs, {args.jobs} workers; stepped chains scoped out: "
        f"{', '.join(scoped)}",
        flush=True,
    )
    results, noise_parts = {}, []
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for done, res in enumerate(pool.map(run_job, jobs, chunksize=1), start=1):
            if res["kind"] == "noise":
                noise_parts.append(res)
            else:
                results[(res["kind"], tuple(res["key"]))] = res
            if done % 20 == 0 or done == len(jobs):
                print(f"  {done}/{len(jobs)} jobs ({time.time() - started:.0f} s)", flush=True)
    for fkey, value in sm.merge_noise(noise_parts, True).items():
        results[("noise", fkey)] = {"counts": [int(v) for v in value["counts"]]}
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
        "scoped": scoped,
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
            print(f"  accepted {r['path']} {r['chain']} {r['filter']}: {'; '.join(r['reasons'])} ({r['accepted']})")
        elif r["listed"]:
            state = "meets every bound in this run" if r["pass"] else "FAILS"
            print(f"  accepted-marginal entry {r['path']} {r['chain']} {r['filter']}: {state}")
    print(f"verdict: the shipped classifier {'holds' if not failing else 'does NOT hold'} on every plan")
    print(f"report: {md}\n        {js}")
    return 0 if not failing else 1


if __name__ == "__main__":
    sys.exit(main())
