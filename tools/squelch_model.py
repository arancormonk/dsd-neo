#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Design gate for a floor-relative ("auto") analog squelch: can a per-window classifier tell
"noise only" from "carrier present" on the channel-filtered complex baseband, at every channel
plan the analog monitor runs?

The classifier sees z, the complex baseband after the half-band cascade and the channel low-pass
(full_demod(): halfband -> channel_lpf_apply()), before any demodulation. Per sample-exact 20 ms
window it measures

  P    mean |z|^2
  CV2  envelope variance over the squared mean power, (mean |z|^4 - P^2) / P^2: about 1 for
       complex Gaussian noise whatever the filtering, about 0 for a constant-envelope FM carrier
  C    phase coherence |mean(u_k conj(u_{k-L}))|, u = z/|z|, with L the smallest lag at which
       the channel taps' autocorrelation |R_h(L)| / R_h(0) drops below 0.1: about 0 for filtered
       noise, about 1 for an AM carrier at any depth

and decides CARRIER when CV2 <= t1 or C >= t2, NOISE when CV2 >= t3 and C <= t4, else UNDECIDED.

Everything comes from the repository, not from invented numbers:

  - The channel taps are the plans full_demod() designs, printed by tools/squelch_model_taps.cpp,
    which this script compiles against the static DSP/runtime/IQ/platform libraries of a configured
    build tree (default build/dev-debug, the link line of dsd-neo_test_dsp_channel_filters). The
    half-band taps come from the same harness. A numpy port of dsd_firdes_low_pass() cross-checks
    every designed plan.
  - Which (kind, width, DSP rate, post_downsample) runs, and with which plan, follows the stream
    rules in src/io/radio/rtl_demod_config.cpp and src/runtime/analog_channel.c: an explicit NFM
    width and every AM width need dsd_analog_width_realizable() at the demod rate and
    post_downsample 1; the unset NFM default runs everywhere, with the 16 kHz design where the rate
    realizes it, the legacy WIDE profile plan (144-tap cap, 63-tap fallback prototype) where it does
    not, and no channel filter at all where the channel LPF is off (rate_in below 20 kHz, or
    DSD_NEO_CHANNEL_LPF=0). With post_downsample above 1 the plan designed at the demod rate runs
    at rate_out x post_downsample; every window and lag here is at that pre-post-decimation rate.
  - DSP rates: the RTL DSP bandwidths (4, 6, 8, 12, 16, 24, 48 kHz), the rates a fixed-grid device
    forces (rtl_choose_passes_for_actual_rate() on 2.5, 6 and 2 MS/s captures: Airspy R2, Airspy
    Mini, RX-888/SDDC), and I/Q replay chains (the fixtures' 48 kHz, the squelch captures' native
    39,062 Hz, a 125 kHz capture past the analog tap capacity, and post_downsample 2 and 4).

Signals (fixed seeds) are built at the channel rate: channel-filtered complex Gaussian noise (white
at twice the rate through the last half-band stage for live and device chains, white at the rate
for replay with base_decimation 1); FM carriers (dead carrier, tones 300/1000/3000 Hz, real speech
taken from the discriminator of tests/fixtures/iq/nfm_*_real.iq) at half and full rated deviation;
AM carriers (dead carrier, the same tones, real airband speech from am_airband_real.iq and the
nfm_squelch_real_a speech, at 30, 60 and 100 % depth); each at 0, +/-1 and +/-2.5 kHz offset; and
the real captures themselves (nfm_ctcss_real without its 12.5 kHz neighbour). CNR is
the carrier power over the noise power at the channel filter's output (so the noise bandwidth is
the filter's own), at 0, 3, 6, 10 and 20 dB. A CARRIER window is any window of an in-channel
carrier at CNR >= 6 dB (3 and 0 dB are reported separately); a NOISE window is noise alone.

Rated FM deviation for a width W is the Carson-rule fit for 3 kHz audio, min(5 kHz, W/2 - 3 kHz),
floored at W/8: 1 kHz at 8 kHz, 2 kHz at 10 kHz, 3.25 kHz at 12.5 kHz, 5 kHz from 16 kHz (the
25 kHz-plan rating; 12.5 kHz plans rate 2.5 kHz, inside this). A variant whose occupied band
(|offset| + deviation + highest audio frequency for FM, |offset| + highest audio frequency for AM)
passes the protected half-width is reported as "edge" and kept out of the gating rates.

Decision targets per window: false CARRIER on noise <= 1e-4; an in-channel carrier at 6 dB not called CARRIER
<= 1e-2; a carrier at >= 6 dB called NOISE (it would raise the floor) <= 1e-4; noise not called NOISE <= 0.25.
Thresholds are chosen on a grid, globally over every plan that can meet the targets on its own, deepest inside the
region all of them accept. Besides the planned classifier (fixed thresholds, 20 ms, C) the script evaluates the
smallest changes, alone and together: a 40 ms window (two sample-exact 20 ms windows pooled), C less its expected
value on noise (beta = (pi/4) rho_n(L) 2F1(1/2, 1/2; 2; rho_n(L)^2), rho_n the noise autocorrelation with the
half-band stage), thresholds normalised by sqrt(N_eff), and an amplitude-weighted coherence Cz. It recommends the
first scheme, in order of how far it departs from the plan, that passes the most plans.

Usage:
    python3 tools/squelch_model.py [--quick] [--out DIR] [--build-dir build/dev-debug] [--jobs N]
                                   [--noise-windows N]

Writes report.md and report.json under DIR (default build/squelch_model/) and prints the verdict. The full run takes
about 25 minutes on 8 workers; --quick (5000 noise windows per filter, short carriers) about one minute, too few
windows to resolve 1e-4. Offline; CI does not run it.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import platform
import shlex
import subprocess
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import scipy
from scipy import ndimage, optimize, special
from scipy import signal as sps

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tests" / "fixtures" / "iq"
HARNESS_SRC = ROOT / "tools" / "squelch_model_taps.cpp"
LINK_TEMPLATE_TARGET = "dsd-neo_test_dsp_channel_filters"

BASE_SEED = 20261004
FIXTURE_RATE_HZ = 48000
WINDOW_PER_S = 50  # 20 ms windows
LAG_RHO_LIMIT = 0.1
CNRS_DB = (0.0, 3.0, 6.0, 10.0, 20.0)
CARRIER_MIN_CNR_DB = 6.0
FM_KIND = 0
AM_KIND = 1
KIND_LABEL = {FM_KIND: "NFM", AM_KIND: "AM"}

# Constants mirrored from include/dsd-neo/runtime/analog_channel.h (checked against the harness header).
NFM_WIDTH_DEFAULT_HZ = 16000
AM_WIDTH_DEFAULT_HZ = 6000
TRANSITION_HZ = 1200
GUARD_HZ = 600
ANALOG_MAX_TAPS = 288
LEGACY_WIDE_CUTOFF_HZ = 8000.0 + GUARD_HZ  # kChannelEdgeWideHz + kChannelLpfGuardHz
LEGACY_WIDE_MAX_TAPS = 144  # kChannelLpfTaps
LPF_DEFAULT_ENABLE_RATE_IN_HZ = 20000  # demod_channel_lpf_default_enable()

NFM_WIDTHS_HZ = (8000, 10000, 12500, 16000, 20000, 25000)
AM_WIDTHS_HZ = (5000, 6000, 8000, 12500, 20000)
RTL_DSP_BW_KHZ = (4, 6, 8, 12, 16, 24, 48)
# Fixed-grid device capture rates (Airspy R2 2.5/10 MS/s give the same chain; Airspy Mini 3/6 MS/s; RX-888/SDDC 2/4/8).
DEVICE_CAPTURES_HZ = {"airspy": 2500000, "airspy-mini": 6000000, "sddc": 2000000}
MAX_DOWNSAMPLE_PASSES = 10  # kMaxDownsamplePasses
# I/Q replay chains: name, rate_in (sample_rate / base_decimation), post_downsample, half-band stage before the channel.
REPLAY_CHAINS = (
    ("replay-48k", 48000, 1, None),
    ("replay-39062", 39062, 1, None),
    ("replay-125k", 125000, 1, None),
    ("replay-48k/pd2", 48000, 2, None),
    ("replay-48k/pd4", 48000, 4, None),
    ("replay-96k/pd2", 96000, 2, None),
    ("replay-24k/pd2", 24000, 2, None),
    ("replay-16k/pd2", 16000, 2, None),
)

TONES_HZ = (300.0, 1000.0, 3000.0)
VOICE_TOP_HZ = 3000.0
OFFSETS_HZ = (0.0, 1000.0, -1000.0, 2500.0, -2500.0)
FM_DEVIATION_FRACTIONS = (0.5, 1.0)
AM_DEPTHS = (0.3, 0.6, 1.0)

FALSE_CARRIER_TARGET = 1e-4  # NOISE windows called CARRIER
POLLUTION_TARGET = 1e-4  # CARRIER windows (>= 6 dB) called NOISE: they would raise the floor
MISS_TARGET = 1e-2  # CARRIER windows at 6 dB not called CARRIER
NOISE_UNDECIDED_TARGET = 0.25  # NOISE windows not called NOISE: they leave the floor estimate without an update
PERCENTILES = (0.01, 1.0, 50.0, 99.0, 99.99)
FULL_NOISE_WINDOWS = 200000  # 1e-4 per window is then 20 events at 20 ms and 10 at 40 ms
QUICK_NOISE_WINDOWS = 5000


@dataclasses.dataclass(frozen=True)
class RatePlan:
    name: str
    source: str  # rtl | device | replay
    rate_in: int  # decides the unset NFM default's channel-LPF enable
    rate_out: int  # demod rate the channel plan is designed at
    post_downsample: int
    hb: str | None  # last half-band stage ahead of the channel filter

    @property
    def channel_rate(self) -> int:
        return self.rate_out * self.post_downsample


@dataclasses.dataclass(frozen=True)
class Config:
    kind: int
    width_hz: int  # 0 = unset NFM default
    lpf_env: str  # "" (DSD_NEO_CHANNEL_LPF unset), "1" or "0"; only the unset NFM default follows it

    @property
    def label(self) -> str:
        if self.width_hz == 0:
            env = f", DSD_NEO_CHANNEL_LPF={self.lpf_env}" if self.lpf_env else ""
            return f"NFM unset{env}"
        default = " (default)" if self.kind == AM_KIND and self.width_hz == AM_WIDTH_DEFAULT_HZ else ""
        return f"{KIND_LABEL[self.kind]} {self.width_hz}{default}"


# --------------------------------------------------------------------------------------------- rate plans


def choose_passes_for_actual_rate(actual_rate_hz: int, rate_in_hz: int) -> int:
    """rtl_choose_passes_for_actual_rate(): the decimation landing closest at or above the requested DSP bandwidth."""
    best_p, best_err = 0, None
    for p in range(MAX_DOWNSAMPLE_PASSES + 1):
        out = actual_rate_hz >> p
        if out < rate_in_hz:
            break
        err = out - rate_in_hz
        if best_err is None or err < best_err:
            best_err, best_p = err, p
    return best_p


def rate_plans() -> list[RatePlan]:
    plans = [RatePlan(f"rtl-{bw}k", "rtl", bw * 1000, bw * 1000, 1, "hb15") for bw in RTL_DSP_BW_KHZ]
    seen = set()
    for device, capture in DEVICE_CAPTURES_HZ.items():
        for bw in RTL_DSP_BW_KHZ:
            rate_in = bw * 1000
            passes = choose_passes_for_actual_rate(capture, rate_in)
            rate_out = capture >> passes
            key = (rate_in >= LPF_DEFAULT_ENABLE_RATE_IN_HZ, rate_out)
            if rate_out == rate_in or key in seen:
                continue  # same chain as an RTL plan, or as another device's
            seen.add(key)
            plans.append(RatePlan(f"{device}-bw{bw}k->{rate_out}", "device", rate_in, rate_out, 1, "hb15"))
    for name, rate_in, pd, hb in REPLAY_CHAINS:
        plans.append(RatePlan(name, "replay", rate_in, rate_in // pd, pd, hb))
    return plans


def configs() -> list[Config]:
    out = [Config(FM_KIND, w, "") for w in NFM_WIDTHS_HZ]
    out += [Config(FM_KIND, 0, env) for env in ("", "1", "0")]
    out += [Config(AM_KIND, w, "") for w in AM_WIDTHS_HZ]
    return out


# --------------------------------------------------------------------------------------------- tap harness


def harness_build_flags(build_dir: Path) -> tuple[list[str], list[str]]:
    """Compile flags and link inputs of the DSP channel-filter test, so the harness sees the same demod_state."""
    target_dir = build_dir / "tests" / "CMakeFiles" / f"{LINK_TEMPLATE_TARGET}.dir"
    flags_file = target_dir / "flags.make"
    link_file = target_dir / "link.txt"
    if not flags_file.is_file() or not link_file.is_file():
        raise SystemExit(
            f"{target_dir} has no flags.make/link.txt: configure and build {build_dir} first "
            f"(cmake --preset dev-debug && cmake --build --preset dev-debug -j)"
        )
    flags = {}
    for line in flags_file.read_text().splitlines():
        if " = " in line and not line.startswith("#"):
            key, value = line.split(" = ", 1)
            flags[key.strip()] = value.strip()
    compile_flags = []
    for key in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
        compile_flags += shlex.split(flags.get(key, ""))
    compile_flags = [f for f in compile_flags if f not in ("-Werror",)]
    link_inputs = []
    tests_dir = build_dir / "tests"
    for token in shlex.split(link_file.read_text()):
        if token.endswith((".a", ".so")) or ".so." in token:
            path = Path(token)
            link_inputs.append(str(path if path.is_absolute() else (tests_dir / path).resolve()))
        elif token.startswith("-l"):
            link_inputs.append(token)
    missing = [p for p in link_inputs if not p.startswith("-l") and not Path(p).exists()]
    if missing:
        raise SystemExit(f"missing link inputs {missing}: build {build_dir} first")
    return compile_flags, link_inputs


def build_harness(build_dir: Path, out_dir: Path) -> Path:
    compile_flags, link_inputs = harness_build_flags(build_dir)
    exe = out_dir / "squelch_model_taps"
    inputs = [HARNESS_SRC] + [Path(p) for p in link_inputs if not p.startswith("-l")]
    if exe.exists() and all(exe.stat().st_mtime >= p.stat().st_mtime for p in inputs):
        return exe
    cxx = os.environ.get("CXX", "c++")
    cmd = [cxx, *compile_flags, "-O1", str(HARNESS_SRC), "-o", str(exe), *link_inputs, "-lm", "-lpthread"]
    subprocess.run(cmd, check=True)
    return exe


def run_harness(exe: Path, requests: list[tuple[int, int, int]]) -> tuple[dict, dict]:
    stdin = "".join(f"{rate} {width} {kind}\n" for rate, width, kind in requests)
    proc = subprocess.run([str(exe)], input=stdin, capture_output=True, text=True, check=True)
    lines = proc.stdout.splitlines()
    header = json.loads(lines[0])
    plans = {}
    for line in lines[1:]:
        rec = json.loads(line)
        rec["taps"] = np.asarray(rec["taps"], dtype=np.float64)
        plans[(rec["rate_out"], rec["width_hz"], rec["kind"])] = rec
    return header, plans


def firdes_low_pass_port(rate_hz: float, cutoff_hz: float, max_taps: int) -> np.ndarray | None:
    """numpy port of dsd_firdes_low_pass() with the Blackman window (float window, double sinc, float taps)."""
    if cutoff_hz <= 0.0 or cutoff_hz > rate_hz / 2.0:
        return None
    ntaps = int(74 * rate_hz / (22.0 * TRANSITION_HZ))
    if ntaps % 2 == 0:
        ntaps += 1
    if ntaps > max_taps:
        return None
    pi_f = np.float32(math.pi)
    m_f = np.float32(ntaps - 1)
    n_f = np.arange(ntaps, dtype=np.float32)
    window = (
        np.float32(0.42)
        - np.float32(0.5) * np.cos((np.float32(2.0) * pi_f * n_f) / m_f)
        + np.float32(0.08) * np.cos((np.float32(4.0) * pi_f * n_f) / m_f)
    ).astype(np.float32)
    half = (ntaps - 1) // 2
    fwt0 = 2.0 * math.pi * cutoff_hz / rate_hz
    n = np.arange(-half, half + 1, dtype=np.float64)
    with np.errstate(invalid="ignore", divide="ignore"):
        ideal = np.where(n == 0, fwt0 / math.pi, np.sin(n * fwt0) / (n * math.pi))
    taps = (ideal * window.astype(np.float64)).astype(np.float32)
    fmax = float(taps[half]) + 2.0 * float(np.sum(taps[half + 1 :].astype(np.float64)))
    return (taps * np.float32(1.0 / fmax)).astype(np.float64)


def port_plan(rate_out: int, width_hz: int) -> np.ndarray | None:
    if width_hz > 0:
        return firdes_low_pass_port(rate_out, width_hz / 2 + GUARD_HZ, ANALOG_MAX_TAPS)
    cutoff = min(max(LEGACY_WIDE_CUTOFF_HZ, 100.0), rate_out * 0.5 * 0.9)
    return firdes_low_pass_port(rate_out, cutoff, LEGACY_WIDE_MAX_TAPS)


# --------------------------------------------------------------------------------------------- plan resolution


def resolve(config: Config, plan: RatePlan, harness_plans: dict) -> dict:
    """The channel filter the stream runs for this configuration and chain, or why it refuses to run."""
    kind, width = config.kind, config.width_hz
    if width > 0:
        rec = harness_plans[(plan.rate_out, width, kind)]
        if plan.post_downsample > 1:
            return {"runs": False, "why": f"post_downsample {plan.post_downsample} refuses an explicit/AM width"}
        if not rec["realizable"]:
            return {
                "runs": False,
                "why": f"{width} Hz not realizable at {plan.rate_out} Hz (max {rec['max_width_for_rate']})",
            }
        return {
            "runs": True,
            "filter": f"analog {width}",
            "taps": rec["taps"],
            "design_rate": plan.rate_out,
            "protected_hz": width * plan.post_downsample,
        }
    enable = plan.rate_in >= LPF_DEFAULT_ENABLE_RATE_IN_HZ
    if config.lpf_env == "1":
        enable = True
    elif config.lpf_env == "0":
        enable = False
    if not enable:
        return {
            "runs": True,
            "filter": "none (channel LPF off)",
            "taps": np.ones(1),
            "design_rate": plan.rate_out,
            "protected_hz": plan.channel_rate,
        }
    rec16 = harness_plans[(plan.rate_out, NFM_WIDTH_DEFAULT_HZ, FM_KIND)]
    if rec16["realizable"]:
        return {
            "runs": True,
            "filter": "analog 16000 (unset default)",
            "taps": rec16["taps"],
            "design_rate": plan.rate_out,
            "protected_hz": NFM_WIDTH_DEFAULT_HZ * plan.post_downsample,
        }
    rec0 = harness_plans[(plan.rate_out, 0, FM_KIND)]
    fallback = port_plan(plan.rate_out, 0) is None
    name = "legacy WIDE 63-tap fallback" if fallback else "legacy WIDE"
    return {
        "runs": True,
        "filter": name,
        "taps": rec0["taps"],
        "design_rate": plan.rate_out,
        "protected_hz": rec0["legacy_wide_width_hz"] * plan.post_downsample,
    }


# --------------------------------------------------------------------------------------------- DSP helpers


def autocorr(taps: np.ndarray) -> np.ndarray:
    """Full autocorrelation of real taps, lag 0 at the centre."""
    return np.correlate(taps, taps, mode="full")


def noise_autocorr(taps: np.ndarray, hb: np.ndarray | None) -> np.ndarray:
    """Autocorrelation of the channel-filter output for unit-variance white complex noise at the half-band input."""
    if hb is not None:
        r_hb = autocorr(hb)
        r_in = r_hb[(len(hb) - 1) % 2 :: 2]  # even lags only, still centred: the decimation by 2
    else:
        r_in = np.ones(1)
    return np.convolve(autocorr(taps), r_in)


def lag_rule(taps: np.ndarray) -> tuple[int, float]:
    """The planned lag: smallest L >= 1 with |R_h(L)| / R_h(0) < LAG_RHO_LIMIT, and R_h(L) / R_h(0)."""
    r = autocorr(taps)
    centre = len(taps) - 1
    rho = r[centre:] / r[centre]
    for lag in range(1, len(rho)):
        if abs(rho[lag]) < LAG_RHO_LIMIT:
            return lag, float(rho[lag])
    return len(rho), 0.0


def channel_filter(x: np.ndarray, taps: np.ndarray) -> np.ndarray:
    """The streaming channel FIR's steady-state output ('valid' part): len(x) - len(taps) + 1 samples."""
    taps32 = taps.astype(np.float32)
    if len(taps) == 1:
        return (x * taps32[0]).astype(np.complex64)
    return sps.oaconvolve(x.astype(np.complex64), taps32, mode="valid").astype(np.complex64)


def white_noise(rng: np.random.Generator, count: int) -> np.ndarray:
    scale = np.float32(math.sqrt(0.5))
    re = rng.standard_normal(count, dtype=np.float32)
    im = rng.standard_normal(count, dtype=np.float32)
    return (re * scale) + 1j * (im * scale)


def filtered_noise(rng: np.random.Generator, count: int, taps: np.ndarray, hb: np.ndarray | None) -> np.ndarray:
    """count samples of channel-filter output for white complex noise (unit variance at the half-band input)."""
    need = count + len(taps) - 1
    if hb is not None:
        wide = white_noise(rng, 2 * need + len(hb) - 1)
        y = sps.oaconvolve(wide, hb.astype(np.float32), mode="valid")[::2][:need]
    else:
        y = white_noise(rng, need)
    return channel_filter(y, taps)


def window_bounds(fs: int, n_windows: int, start: int) -> np.ndarray:
    """Sample-exact 20 ms windows: boundary k at floor(k * fs / 50), offset by start."""
    return start + (np.arange(n_windows + 1, dtype=np.int64) * fs) // WINDOW_PER_S


def window_sums(z: np.ndarray, bounds: np.ndarray, lag: int) -> dict:
    """Per-20 ms-window sums the features are built from; z holds lag samples before bounds[0] (streaming history).

    n count, sa sum |z|^2, sa2 sum |z|^4, su sum u_k conj(u_{k-L}) (u = z/|z|), sz sum z_k conj(z_{k-L}).
    """
    first, last = int(bounds[0]), int(bounds[-1])
    idx = (bounds[:-1] - first).astype(np.int64)
    seg = z[first:last]
    a = (seg.real.astype(np.float64) ** 2) + (seg.imag.astype(np.float64) ** 2)
    hist = z[first - lag : last]
    prod_z = hist[lag:] * np.conj(hist[:-lag])
    mag = np.abs(hist)
    mag_prod = mag[lag:] * mag[:-lag]
    prod_u = np.divide(prod_z, mag_prod, out=np.zeros_like(prod_z), where=mag_prod > 0)
    return {
        "n": np.diff(bounds).astype(np.float64),
        "sa": np.add.reduceat(a, idx),
        "sa2": np.add.reduceat(a * a, idx),
        "su": np.add.reduceat(prod_u.astype(np.complex128), idx),
        "sz": np.add.reduceat(prod_z.astype(np.complex128), idx),
    }


def merge_windows(sums: dict, multiple: int) -> dict:
    """Sums over consecutive groups of `multiple` 20 ms windows (a sample-exact 20 x multiple ms window)."""
    if multiple == 1:
        return sums
    count = len(sums["n"]) // multiple * multiple
    return {k: v[:count].reshape(-1, multiple).sum(axis=1) for k, v in sums.items()}


def window_features(sums: dict, beta_c: float, rho_n: float) -> dict:
    """The features of each window.

    cv2   (mean |z|^4 - P^2) / P^2, P = mean |z|^2
    c     |mean(u_k conj(u_{k-L}))|                     the planned phase coherence
    cdb   |mean(u_k conj(u_{k-L})) - beta|              C less its expected value on noise (beta, from rho_n(L))
    cz    |sum z_k conj(z_{k-L})| / sum |z_k|^2         amplitude-weighted coherence
    czdb  |sum z_k conj(z_{k-L}) / sum |z_k|^2 - rho_n(L)|  the same less its expected value on noise
    """
    p = sums["sa"] / sums["n"]
    mean_u = sums["su"] / sums["n"]
    ratio_z = sums["sz"] / sums["sa"]
    return {
        "cv2": (sums["sa2"] / sums["n"]) / (p * p) - 1.0,
        "c": np.abs(mean_u),
        "cdb": np.abs(mean_u - beta_c),
        "cz": np.abs(ratio_z),
        "czdb": np.abs(ratio_z - rho_n),
    }


def resample(x: np.ndarray, fs_in: float, fs_out: float, trim_s: float = 0.05) -> np.ndarray:
    """FFT resampling (ideal low-pass at the smaller Nyquist); trim_s is cut from both ends against wrap-around."""
    if int(fs_in) == int(fs_out):
        y = x
    else:
        y = sps.resample(x, round(len(x) * fs_out / fs_in))
    trim = round(trim_s * fs_out)
    return y[trim : len(y) - trim] if trim > 0 else y


def brickwall(x: np.ndarray, fs: float, lo_hz: float, hi_hz: float) -> np.ndarray:
    """Keep only lo_hz <= f < hi_hz (signed frequencies for complex x, |f| for real x)."""
    spec = np.fft.fft(x)
    f = np.fft.fftfreq(len(x), 1.0 / fs)
    if np.iscomplexobj(x):
        keep = (f >= lo_hz) & (f < hi_hz)
        return np.fft.ifft(spec * keep)
    keep = (np.abs(f) >= lo_hz) & (np.abs(f) < hi_hz)
    return np.real(np.fft.ifft(spec * keep))


def load_cu8(name: str) -> tuple[np.ndarray, int]:
    meta = json.loads((FIXTURES / f"{name}.iq.json").read_text())
    if meta.get("sample_format") != "cu8" or meta.get("base_decimation") != 1:
        raise SystemExit(f"{name}: expected a cu8 capture with base_decimation 1")
    raw = np.fromfile(FIXTURES / meta["data_file"], dtype=np.uint8).astype(np.float64)
    centred = (raw - 127.5) / 127.5  # the RTL-SDR u8 convention tools/build_iq_fixtures.py writes
    return centred[0::2] + 1j * centred[1::2], int(meta["sample_rate_hz"])


# Real NFM excerpts (tools/build_iq_fixtures.py ANALOG_EXCERPTS): the channel sits at 0 Hz in each; nfm_ctcss_real keeps
# its 12.5 kHz neighbour at -12.5 kHz, which a +/-6.25 kHz brick wall removes before the capture is used as a carrier.
REAL_FM = ("nfm_squelch_real_a", "nfm_squelch_real_b", "nfm_ctcss_real")
REAL_AM = ("am_airband_real",)
REAL_CHANNEL_HALF_HZ = 6250.0

_SOURCE_CACHE: dict = {}


def real_sources() -> dict:
    """Real captures (channel only) and their audio, at the fixture rate. Cached per process."""
    if "real" in _SOURCE_CACHE:
        return _SOURCE_CACHE["real"]
    real = {}
    for name in REAL_FM + REAL_AM:
        z, fs = load_cu8(name)
        z = brickwall(z, fs, -REAL_CHANNEL_HALF_HZ, REAL_CHANNEL_HALF_HZ)
        if name in REAL_FM:
            inst = np.angle(z[1:] * np.conj(z[:-1])) * fs / (2.0 * math.pi)
            inst = brickwall(inst - np.mean(inst), fs, 0.0, VOICE_TOP_HZ + 400.0)
            audio = inst / np.percentile(np.abs(inst), 99.9)  # speech (and its sub-audible) at unit 99.9 % deviation
        else:
            env = np.abs(z)
            audio = brickwall(env / np.mean(env) - 1.0, fs, 200.0, VOICE_TOP_HZ + 400.0)
            audio = audio / np.max(np.abs(audio))  # unit peak, so 100 % depth never over-modulates
        spec = np.abs(np.fft.fftshift(np.fft.fft(z))) ** 2
        freqs = np.fft.fftshift(np.fft.fftfreq(len(z), 1.0 / fs))
        order = np.argsort(np.abs(freqs))
        cum = np.cumsum(spec[order]) / np.sum(spec)
        half99 = float(np.abs(freqs[order][np.searchsorted(cum, 0.99)]))
        real[name] = {"iq": z, "audio": audio, "fs": fs, "half99_hz": half99}
    _SOURCE_CACHE["real"] = real
    return real


def rated_deviation_hz(protected_hz: float) -> float:
    return min(5000.0, max(protected_hz / 2.0 - VOICE_TOP_HZ, protected_hz / 8.0))


def carrier_variants(kind: int, protected_hz: float, quick: bool) -> list[dict]:
    """The carrier set for one channel: kind, modulation, deviation or depth, offset, length and in-channel flag."""
    tone_s = 0.2 if quick else 0.6
    speech_s = 0.6 if quick else 4.0
    half = protected_hz / 2.0
    variants = []
    sources = real_sources()
    for offset in OFFSETS_HZ:
        variants.append(
            {
                "family": "cw",
                "mod": "none",
                "amount": 0.0,
                "offset": offset,
                "seconds": tone_s,
                "in_channel": abs(offset) <= half,
            }
        )
        if kind == FM_KIND:
            dev_rated = rated_deviation_hz(protected_hz)
            for frac in FM_DEVIATION_FRACTIONS:
                dev = frac * dev_rated
                for tone in TONES_HZ:
                    variants.append(
                        {
                            "family": "fm",
                            "mod": f"tone{int(tone)}",
                            "amount": dev,
                            "offset": offset,
                            "seconds": tone_s,
                            "in_channel": abs(offset) + dev + tone <= half,
                        }
                    )
                for name in REAL_FM:
                    variants.append(
                        {
                            "family": "fm",
                            "mod": f"speech:{name}",
                            "amount": dev,
                            "offset": offset,
                            "seconds": speech_s,
                            "in_channel": abs(offset) + dev + VOICE_TOP_HZ <= half,
                        }
                    )
            for name in REAL_FM:
                variants.append(
                    {
                        "family": "real",
                        "mod": name,
                        "amount": 0.0,
                        "offset": offset,
                        "seconds": speech_s,
                        "in_channel": abs(offset) + sources[name]["half99_hz"] <= half,
                    }
                )
        else:
            for depth in AM_DEPTHS:
                for tone in TONES_HZ:
                    variants.append(
                        {
                            "family": "am",
                            "mod": f"tone{int(tone)}",
                            "amount": depth,
                            "offset": offset,
                            "seconds": tone_s,
                            "in_channel": abs(offset) + tone <= half,
                        }
                    )
                for name in REAL_AM + REAL_FM[:1]:
                    variants.append(
                        {
                            "family": "am",
                            "mod": f"speech:{name}",
                            "amount": depth,
                            "offset": offset,
                            "seconds": speech_s,
                            "in_channel": abs(offset) + VOICE_TOP_HZ <= half,
                        }
                    )
            for name in REAL_AM:
                variants.append(
                    {
                        "family": "real",
                        "mod": name,
                        "amount": 0.0,
                        "offset": offset,
                        "seconds": speech_s,
                        "in_channel": abs(offset) + sources[name]["half99_hz"] <= half,
                    }
                )
    return variants


def source_at_rate(name: str, field: str, fs: int) -> np.ndarray:
    """A real capture's channel ("iq") or audio ("audio") resampled to fs. Cached per process."""
    key = (name, field, fs)
    if key not in _SOURCE_CACHE:
        src = real_sources()[name]
        _SOURCE_CACHE[key] = resample(src[field], src["fs"], fs)
    return _SOURCE_CACHE[key]


def synth_carrier(variant: dict, fs: int, count: int, rng: np.random.Generator) -> np.ndarray:
    """Unit-ish complex baseband carrier of count samples at fs for one variant."""
    t = np.arange(count) / fs
    theta0 = rng.uniform(0.0, 2.0 * math.pi)
    offset = variant["offset"]
    family, mod, amount = variant["family"], variant["mod"], variant["amount"]
    if family == "real":
        iq = source_at_rate(mod, "iq", fs)
        start = int(rng.integers(0, max(1, len(iq) - count)))
        iq = np.resize(iq[start:], count)  # tiles only when the excerpt is shorter than asked
        return iq * np.exp(1j * (2.0 * math.pi * offset * t + theta0))
    if mod == "none":
        audio = np.zeros(count)
    elif mod.startswith("tone"):
        tone = float(mod[4:])
        audio = np.sin(2.0 * math.pi * tone * t + rng.uniform(0.0, 2.0 * math.pi))
    else:
        audio = source_at_rate(mod.split(":", 1)[1], "audio", fs)
        start = int(rng.integers(0, max(1, len(audio) - count)))
        audio = np.resize(audio[start:], count)
    if family in ("cw", "fm"):
        phase = 2.0 * math.pi * np.cumsum(offset + amount * audio) / fs
        return np.exp(1j * (phase + theta0))
    return (1.0 + amount * audio) * np.exp(1j * (2.0 * math.pi * offset * t + theta0))


# --------------------------------------------------------------------------------------------- jobs

WINDOW_MULTIPLES = (1, 2)  # 20 ms (the plan) and 40 ms windows
COHERENCES = ("c", "cdb", "cz", "czdb")
COH_LABEL = {"c": "C", "cdb": "C-beta", "cz": "Cz", "czdb": "Cz-rho"}
SPACE_NAMES = ("raw", "norm")
# Threshold grids. A threshold sits on a bin edge, so every rate is exact for a grid threshold.
CV2_EDGES = np.round(np.arange(0.0, 3.0 + 1e-9, 0.01), 6)
COH_EDGES = np.round(np.arange(0.0, 1.0 + 1e-9, 0.01), 6)
# Normalised features: X = (CV2 - 1) * sqrt(N_eff), Y = (coherence - bias) * sqrt(N_eff), with N_eff (scaled by the
# window multiple) and the bias from the noise model. Small X and large Y read as carrier, as small CV2 and large
# coherence do.
X_EDGES = np.round(np.arange(-14.0, 6.0 + 1e-9, 0.1), 6)
Y_EDGES = np.round(np.arange(-3.0, 20.0 + 1e-9, 0.1), 6)
SPACE_EDGES = {"raw": (CV2_EDGES, COH_EDGES), "norm": (X_EDGES, Y_EDGES)}


def job_seed(*parts) -> int:
    return (BASE_SEED + zlib.crc32(repr(parts).encode())) & 0xFFFFFFFF


def to_space(cv2: np.ndarray, coh_values: np.ndarray, space: str, norm: dict, multiple: int, coh: str):
    if space == "raw":
        return cv2, coh_values
    root = math.sqrt(norm["neff"] * multiple)
    return (cv2 - 1.0) * root, (coh_values - norm["bias"][coh]) * root


def edge_hist(x: np.ndarray, y: np.ndarray, x_edges: np.ndarray, y_edges: np.ndarray) -> np.ndarray:
    """Counts on a threshold grid: bin i holds edges[i] <= value < edges[i + 1]; the end bins are open."""
    i = np.clip(np.searchsorted(x_edges, x, side="right") - 1, 0, len(x_edges) - 1)
    j = np.clip(np.searchsorted(y_edges, y, side="right") - 1, 0, len(y_edges) - 1)
    return np.bincount(i * len(y_edges) + j, minlength=len(x_edges) * len(y_edges)).reshape(len(x_edges), -1)


def noise_job(job: dict) -> dict:
    """Feature percentiles and sparse grid histograms of n_windows 20 ms windows of channel-filtered noise."""
    taps, hb, fs, lag, norm = job["taps"], job["hb"], job["fs"], job["lag"], job["norm"]
    rng = np.random.default_rng(job_seed("noise", job["key"]))
    per_chunk = max(2, 400000 // max(1, fs // WINDOW_PER_S)) // 2 * 2
    chunks = []
    done = 0
    while done < job["n_windows"]:
        n_w = min(per_chunk, job["n_windows"] - done)
        bounds = window_bounds(fs, n_w, lag)
        chunks.append(window_sums(filtered_noise(rng, int(bounds[-1]), taps, hb), bounds, lag))
        done += n_w
    sums = {k: np.concatenate([c[k] for c in chunks]) for k in chunks[0]}
    out = {"key": job["key"], "n": {}, "pct": {}, "hist": {}}
    for multiple in WINDOW_MULTIPLES:
        feats = window_features(merge_windows(sums, multiple), norm["beta_c"], norm["rho_n"])
        out["n"][multiple] = len(feats["cv2"])
        out["pct"][multiple] = {name: np.percentile(v, PERCENTILES).tolist() for name, v in feats.items()}
        for coh in COHERENCES:
            for space in SPACE_NAMES:
                x, y = to_space(feats["cv2"], feats[coh], space, norm, multiple, coh)
                h = edge_hist(x, y, *SPACE_EDGES[space])
                nz = np.nonzero(h)
                out["hist"][(space, multiple, coh)] = (
                    nz[0].astype(np.int32),
                    nz[1].astype(np.int32),
                    h[nz].astype(np.int64),
                )
    return out


def carrier_job(job: dict) -> dict:
    """Features of every carrier variant at every CNR for one (kind, filter, protected width)."""
    taps, hb, fs, lag, norm = job["taps"], job["hb"], job["fs"], job["lag"], job["norm"]
    rng = np.random.default_rng(job_seed("carrier", job["key"]))
    variants = carrier_variants(job["kind"], job["protected_hz"], job["quick"])
    parts = {m: {name: [] for name in ("cv2", *COHERENCES, "variant", "cnr")} for m in WINDOW_MULTIPLES}
    for vi, variant in enumerate(variants):
        n_w = max(2, round(variant["seconds"] * WINDOW_PER_S))
        bounds = window_bounds(fs, n_w, lag)
        count = int(bounds[-1])
        s = channel_filter(synth_carrier(variant, fs, count + len(taps) - 1, rng), taps)
        n = filtered_noise(rng, count, taps, hb)
        sig_power = float(np.mean(np.abs(s[lag:].astype(np.complex128)) ** 2))
        for ci, cnr in enumerate(CNRS_DB):
            gain = np.float32(math.sqrt(sig_power / (job["noise_power"] * 10.0 ** (cnr / 10.0))))
            sums = window_sums(s + gain * n, bounds, lag)
            for multiple in WINDOW_MULTIPLES:
                feats = window_features(merge_windows(sums, multiple), norm["beta_c"], norm["rho_n"])
                for name in ("cv2", *COHERENCES):
                    parts[multiple][name].append(feats[name].astype(np.float32))
                parts[multiple]["variant"].append(np.full(len(feats["cv2"]), vi, dtype=np.int32))
                parts[multiple]["cnr"].append(np.full(len(feats["cv2"]), ci, dtype=np.int8))
    out = {"key": job["key"], "variants": variants}
    for multiple in WINDOW_MULTIPLES:
        out[multiple] = {name: np.concatenate(v) for name, v in parts[multiple].items()}
    return out


def run_job(job: dict) -> tuple[str, dict]:
    if "n_windows" in job:
        return "noise", noise_job(job)
    return "carrier", carrier_job(job)


# --------------------------------------------------------------------------------------------- combos


def filter_info(taps: np.ndarray, hb: np.ndarray | None, fs: int) -> dict:
    """Lag rule, noise correlation, bias and effective window size for one filter at its channel rate."""
    lag, rho_h = lag_rule(taps)
    r_h = autocorr(taps)
    rho_taps = r_h / r_h[len(taps) - 1]
    r_n = noise_autocorr(taps, hb)
    centre = len(r_n) // 2
    rho_all = r_n / r_n[centre]
    rho_n = float(rho_all[centre + lag]) if centre + lag < len(rho_all) else 0.0
    n_mean = fs / WINDOW_PER_S
    neff = n_mean / float(np.sum(rho_all**2))
    # E[u_k conj(u_{k-L})] on complex Gaussian noise with correlation rho: (pi/4) rho 2F1(1/2, 1/2; 2; rho^2).
    beta_c = (math.pi / 4.0) * rho_n * float(special.hyp2f1(0.5, 0.5, 2.0, rho_n * rho_n))
    return {
        "lag": lag,
        "rho_taps_lag": rho_h,
        "rho_noise_lag": rho_n,
        "window_samples": n_mean,
        "neff": neff,
        "neff_taps": n_mean / float(np.sum(rho_taps**2)),
        "noise_power": float(r_n[centre]),
        "enbw_hz": fs * float(np.sum(taps**2)) / float(np.sum(taps)) ** 2,
        "norm": {
            "neff": neff,
            "beta_c": beta_c,
            "rho_n": rho_n,
            "bias": {"c": abs(beta_c), "cdb": 0.0, "cz": abs(rho_n), "czdb": 0.0},
        },
    }


def taps_key(taps: np.ndarray) -> str:
    return f"{len(taps)}:{zlib.crc32(taps.astype(np.float64).tobytes()):08x}"


def build_combos(plans: list[RatePlan], harness_plans: dict, hb_taps: dict) -> list[dict]:
    combos = []
    for config in configs():
        for plan in plans:
            res = resolve(config, plan, harness_plans)
            combo = {"config": config, "plan": plan, **res}
            if res["runs"]:
                fkey = f"{plan.channel_rate}:{plan.hb}:{taps_key(res['taps'])}"
                combo["fkey"] = fkey
                combo["ckey"] = f"{KIND_LABEL[config.kind]}:{fkey}:{res['protected_hz']}"
                combo.update(filter_info(res["taps"], hb_taps.get(plan.hb), plan.channel_rate))
            combos.append(combo)
    return combos


def make_jobs(combos: list[dict], hb_taps: dict, quick: bool, noise_windows: int) -> list[dict]:
    noise_jobs, carrier_jobs = {}, {}
    for combo in combos:
        if not combo["runs"]:
            continue
        plan = combo["plan"]
        base = {
            "taps": combo["taps"],
            "hb": hb_taps.get(plan.hb),
            "fs": plan.channel_rate,
            "lag": combo["lag"],
            "norm": combo["norm"],
        }
        noise_jobs.setdefault(combo["fkey"], {**base, "key": combo["fkey"], "n_windows": noise_windows})
        carrier_jobs.setdefault(
            combo["ckey"],
            {
                **base,
                "key": combo["ckey"],
                "kind": combo["config"].kind,
                "protected_hz": combo["protected_hz"],
                "quick": quick,
                "noise_power": combo["noise_power"],
            },
        )
    jobs = list(noise_jobs.values()) + list(carrier_jobs.values())
    return sorted(jobs, key=lambda j: -j["fs"] * (2 * j.get("n_windows", 0) + 15000))


# --------------------------------------------------------------------------------------------- decision rates


def rule_counts(h: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Window counts for every grid threshold pair from a (K x M) histogram over (x, y) bins.

    carrier[i, j]: windows with x < edge_x[i] or y >= edge_y[j]   (CARRIER rule, t1 = edge_x[i], t2 = edge_y[j])
    noise[i, j]:   windows with x >= edge_x[i] and y < edge_y[j]  (NOISE rule, t3 = edge_x[i], t4 = edge_y[j])
    Both are (K + 1) x (M + 1); index K or M is the threshold past the last edge (x: all below, y: none above).
    """
    k, m = h.shape
    below = np.vstack([np.zeros((1, m), dtype=np.int64), np.cumsum(h, axis=0, dtype=np.int64)])
    joint = np.hstack([np.flip(np.cumsum(np.flip(below, 1), axis=1), 1), np.zeros((k + 1, 1), dtype=np.int64)])
    carrier = joint[:, :1] + joint[k : k + 1, :] - joint
    at_or_above = np.vstack(
        [np.flip(np.cumsum(np.flip(h, 0), axis=0, dtype=np.int64), 0), np.zeros((1, m), dtype=np.int64)]
    )
    noise = np.hstack([np.zeros((k + 1, 1), dtype=np.int64), np.cumsum(at_or_above, axis=1)])
    return carrier, noise


def distance_inside(mask: np.ndarray, x_step: float, y_step: float) -> np.ndarray:
    """Euclidean distance, in feature units, from each feasible cell to the nearest infeasible one (0 outside)."""
    if not mask.any():
        return np.zeros(mask.shape)
    padded = np.pad(mask, 1, constant_values=False)
    return ndimage.distance_transform_edt(padded, sampling=(x_step, y_step))[1:-1, 1:-1]


def edge_value(edges: np.ndarray, index: int) -> float:
    return float(edges[index]) if index < len(edges) else math.inf


def scheme_parts(name: str) -> tuple[str, int, str]:
    space, window, coh = name.split("/")
    return space, int(window[:-2]) // 20, coh


# How far a scheme departs from the plan: normalised thresholds, a 40 ms window, subtracting the coherence's value on
# noise and the amplitude-weighted coherence each add to it. The recommendation is the first scheme, in this order,
# that passes the most plans.
SPACE_COST = {"raw": 0, "norm": 2}
WINDOW_COST = {1: 0, 2: 1}
COHERENCE_COST = {"c": 0, "cdb": 1, "cz": 2, "czdb": 3}


def scheme_order_key(name: str) -> tuple:
    space, multiple, coh = scheme_parts(name)
    cost = SPACE_COST[space] + WINDOW_COST[multiple] + COHERENCE_COST[coh]
    return cost, multiple, COHERENCES.index(coh), SPACE_NAMES.index(space)


SCHEMES = sorted(
    (f"{space}/{20 * m}ms/{coh}" for m in WINDOW_MULTIPLES for coh in COHERENCES for space in SPACE_NAMES),
    key=scheme_order_key,
)
PLANNED_SCHEME = "raw/20ms/c"


def carrier_subsets(cres: dict, multiple: int) -> dict:
    """Window masks: in-channel carriers per CNR, in-channel at >= 6 dB, and edge variants at 6 dB."""
    arr = cres[multiple]
    in_channel = np.array([v["in_channel"] for v in cres["variants"]], dtype=bool)[arr["variant"]]
    out = {f"cnr{int(c)}": in_channel & (arr["cnr"] == i) for i, c in enumerate(CNRS_DB)}
    ge = np.isin(arr["cnr"], [i for i, c in enumerate(CNRS_DB) if c >= CARRIER_MIN_CNR_DB])
    out["ge6"] = in_channel & ge
    out["edge6"] = (~in_channel) & (arr["cnr"] == CNRS_DB.index(CARRIER_MIN_CNR_DB))
    return out


def carrier_xy(cres: dict, combo: dict, scheme: str) -> tuple[np.ndarray, np.ndarray]:
    space, multiple, coh = scheme_parts(scheme)
    arr = cres[multiple]
    return to_space(arr["cv2"].astype(np.float64), arr[coh].astype(np.float64), space, combo["norm"], multiple, coh)


def noise_hist(nres: dict, scheme: str) -> np.ndarray:
    space, multiple, coh = scheme_parts(scheme)
    x_edges, y_edges = SPACE_EDGES[space]
    i, j, counts = nres["hist"][(space, multiple, coh)]
    h = np.zeros((len(x_edges), len(y_edges)), dtype=np.int64)
    h[i, j] = counts
    return h


def rate_surfaces(combo: dict, nres: dict, cres: dict, scheme: str) -> dict:
    """The rate surfaces thresholds are chosen from, over the whole grid of one scheme.

    fa       NOISE windows the CARRIER rule (t1, t2) calls CARRIER
    nn       NOISE windows the NOISE rule (t3, t4) calls NOISE
    called6  in-channel carrier windows at 6 dB the CARRIER rule calls CARRIER
    pol      in-channel carrier windows at >= 6 dB the NOISE rule calls NOISE (they would raise the floor)
    """
    space, multiple, _ = scheme_parts(scheme)
    x_edges, y_edges = SPACE_EDGES[space]
    h = noise_hist(nres, scheme)
    total = float(h.sum())
    fa, nn = rule_counts(h)
    x, y = carrier_xy(cres, combo, scheme)
    sub = carrier_subsets(cres, multiple)
    called6, _ = rule_counts(edge_hist(x[sub["cnr6"]], y[sub["cnr6"]], x_edges, y_edges))
    _, pol = rule_counts(edge_hist(x[sub["ge6"]], y[sub["ge6"]], x_edges, y_edges))
    return {
        "fa": (fa / total).astype(np.float32),
        "nn": (nn / total).astype(np.float32),
        "called6": (called6 / max(1, int(sub["cnr6"].sum()))).astype(np.float32),
        "pol": (pol / max(1, int(sub["ge6"].sum()))).astype(np.float32),
    }


def own_masks(surf: dict) -> tuple[np.ndarray, np.ndarray]:
    ok12 = (surf["fa"] <= FALSE_CARRIER_TARGET) & ((1.0 - surf["called6"]) <= MISS_TARGET)
    ok34 = (surf["pol"] <= POLLUTION_TARGET) & ((1.0 - surf["nn"]) <= NOISE_UNDECIDED_TARGET)
    return ok12, ok34


def choose_thresholds(surfaces: list[dict], scheme: str) -> dict:
    """Global (t1, t2), then (t3, t4) with t3 >= t1 and t4 <= t2, each deepest inside the region all plans accept.

    Where no threshold pair meets the targets on every plan, the pair with the smallest worst-case excess is taken.
    """
    space, _, _ = scheme_parts(scheme)
    x_edges, y_edges = SPACE_EDGES[space]
    steps = (float(x_edges[1] - x_edges[0]), float(y_edges[1] - y_edges[0]))
    fa = np.max([sf["fa"] for sf in surfaces], axis=0)
    miss6 = np.max([1.0 - sf["called6"] for sf in surfaces], axis=0)
    score12 = np.maximum(fa / FALSE_CARRIER_TARGET, miss6 / MISS_TARGET)
    depth = distance_inside(score12 <= 1.0, *steps)
    flat = int(np.argmax(depth)) if depth.max() > 0 else int(np.argmin(score12))
    i1, j2 = np.unravel_index(flat, score12.shape)
    pol = np.max([sf["pol"] for sf in surfaces], axis=0)
    undecided = np.max([1.0 - sf["nn"] for sf in surfaces], axis=0)
    score34 = np.maximum(pol / POLLUTION_TARGET, undecided / NOISE_UNDECIDED_TARGET)
    allowed = np.zeros(score34.shape, dtype=bool)
    allowed[i1:, : j2 + 1] = True  # t3 >= t1 and t4 <= t2: no window is both
    depth34 = distance_inside((score34 <= 1.0) & allowed, *steps)
    if depth34.max() > 0:
        flat34 = int(np.argmax(depth34))
    else:
        flat34 = int(np.argmin(np.where(allowed, score34, np.inf)))
    i3, j4 = np.unravel_index(flat34, score34.shape)
    return {
        "scheme": scheme,
        "index": (int(i1), int(j2), int(i3), int(j4)),
        "t1": edge_value(x_edges, int(i1)),
        "t2": edge_value(y_edges, int(j2)),
        "t3": edge_value(x_edges, int(i3)),
        "t4": edge_value(y_edges, int(j4)),
        "all_plans_carrier_rule": bool(depth.max() > 0),
        "all_plans_noise_rule": bool(depth34.max() > 0),
    }


def combo_verdict(combo: dict, surf: dict, cres: dict, th: dict, with_slack: bool) -> dict:
    """Error rates of one plan at the chosen thresholds and, with_slack, its own margin around them."""
    scheme = th["scheme"]
    space, multiple, _ = scheme_parts(scheme)
    i1, j2, i3, j4 = th["index"]
    x, y = carrier_xy(cres, combo, scheme)
    called = (x < th["t1"]) | (y >= th["t2"])
    noise_rule = (x >= th["t3"]) & (y < th["t4"]) & ~called
    sub = carrier_subsets(cres, multiple)

    def rate(flags, name):
        mask = sub[name]
        return float(np.mean(flags[mask])) if mask.any() else None

    def miss(name):
        r = rate(called, name)
        return None if r is None else 1.0 - r

    out = {
        "false_carrier": float(surf["fa"][i1, j2]),
        "noise_undecided": 1.0 - float(surf["nn"][i3, j4]),
        "miss_cnr6": miss("cnr6"),
        "miss_ge6": miss("ge6"),
        "miss_cnr3": miss("cnr3"),
        "miss_cnr0": miss("cnr0"),
        "pollution_ge6": rate(noise_rule, "ge6"),
        "pollution_cnr3": rate(noise_rule, "cnr3"),
        "pollution_cnr0": rate(noise_rule, "cnr0"),
        "miss_edge6": miss("edge6"),
    }
    ok12, ok34 = own_masks(surf)
    out["own_feasible"] = bool(ok12.any() and ok34.any())
    out["pass"] = bool(
        out["false_carrier"] <= FALSE_CARRIER_TARGET
        and out["miss_cnr6"] <= MISS_TARGET
        and out["pollution_ge6"] <= POLLUTION_TARGET
        and out["noise_undecided"] <= NOISE_UNDECIDED_TARGET
    )
    if with_slack:
        x_edges, y_edges = SPACE_EDGES[space]
        steps = (float(x_edges[1] - x_edges[0]), float(y_edges[1] - y_edges[0]))
        out["slack_carrier_rule"] = float(distance_inside(ok12, *steps)[i1, j2])
        out["slack_noise_rule"] = float(distance_inside(ok34, *steps)[i3, j4])
    return out


def variant_label(v: dict) -> str:
    amount = ""
    if v["family"] == "fm":
        amount = f" dev {v['amount']:.0f} Hz"
    elif v["family"] == "am":
        amount = f" depth {v['amount'] * 100:.0f}%"
    return f"{v['family']} {v['mod']}{amount} offset {v['offset']:+.0f} Hz"


def worst_variants(cres: dict, combo: dict, th: dict, cnr_db: float, count: int = 3) -> list:
    """The in-channel variants the chosen CARRIER rule misses most often at cnr_db."""
    _, multiple, _ = scheme_parts(th["scheme"])
    x, y = carrier_xy(cres, combo, th["scheme"])
    called = (x < th["t1"]) | (y >= th["t2"])
    arr = cres[multiple]
    ci = CNRS_DB.index(cnr_db)
    rows = []
    for vi, v in enumerate(cres["variants"]):
        sel = (arr["variant"] == vi) & (arr["cnr"] == ci)
        if v["in_channel"] and sel.any():
            rows.append((1.0 - float(np.mean(called[sel])), variant_label(v)))
    rows.sort(reverse=True)
    return [{"miss": m, "variant": label} for m, label in rows[:count] if m > 0.0]


def neff_cut(unique: dict, verdicts: dict, multiple: int) -> tuple[float | None, list[str]]:
    """Smallest N_eff at and above which every plan passes, and the plans that fail above any such cut."""
    rows = sorted(((unique[ck]["neff"] * multiple, ck) for ck in unique), reverse=True)
    cut = None
    for neff, ck in rows:
        if not verdicts[ck]["pass"]:
            break
        cut = neff
    return cut, [ck for _, ck in rows if not verdicts[ck]["pass"]]


def analyse(combos: list[dict], noise_results: dict, carrier_results: dict) -> dict:
    """Thresholds and per-plan verdicts for every scheme; slack and worst variants for the planned and best."""
    unique = {}
    for combo in combos:
        if combo["runs"]:
            unique.setdefault(combo["ckey"], combo)

    def evaluate(scheme: str, with_slack: bool) -> dict:
        surfaces = {
            ck: rate_surfaces(cb, noise_results[cb["fkey"]], carrier_results[ck], scheme) for ck, cb in unique.items()
        }
        feasible = [sf for sf in surfaces.values() if all(m.any() for m in own_masks(sf))]
        th = choose_thresholds(feasible or list(surfaces.values()), scheme)
        verdicts = {ck: combo_verdict(unique[ck], surfaces[ck], carrier_results[ck], th, with_slack) for ck in unique}
        _, multiple, _ = scheme_parts(scheme)
        cut, failing = neff_cut(unique, verdicts, multiple)
        return {
            "thresholds": th,
            "verdicts": verdicts,
            "passing": sum(1 for v in verdicts.values() if v["pass"]),
            "own_feasible": len(feasible),
            "total": len(unique),
            "worst": {
                key: max(v[key] for v in verdicts.values())
                for key in ("false_carrier", "miss_cnr6", "pollution_ge6", "noise_undecided")
            },
            "neff_cut": cut,
            "failing": failing,
        }

    schemes = {}
    for scheme in SCHEMES:
        schemes[scheme] = evaluate(scheme, with_slack=False)
        s = schemes[scheme]
        print(f"  {scheme:16s} {s['passing']:3d}/{s['total']} plans pass", flush=True)
    best = max(SCHEMES, key=lambda n: (schemes[n]["passing"], -SCHEMES.index(n)))
    for name in {PLANNED_SCHEME, best}:
        schemes[name] = evaluate(name, with_slack=True)
        th = schemes[name]["thresholds"]
        for ck, cb in unique.items():
            schemes[name]["verdicts"][ck]["worst_variants"] = worst_variants(
                carrier_results[ck], cb, th, CARRIER_MIN_CNR_DB
            )
    return {"schemes": schemes, "recommended": best, "unique": unique}


def carrier_percentiles(cres: dict, multiple: int) -> dict:
    sub = carrier_subsets(cres, multiple)
    arr = cres[multiple]
    out = {}
    for name in [f"cnr{int(c)}" for c in CNRS_DB] + ["ge6", "edge6"]:
        mask = sub[name]
        out[name] = {
            feat: (np.percentile(arr[feat][mask].astype(np.float64), PERCENTILES).tolist() if mask.any() else None)
            for feat in ("cv2", *COHERENCES)
        }
        out[name]["n"] = int(mask.sum())
    return out


# --------------------------------------------------------------------------------------------- report


def signal_sanity(harness_plans: dict) -> dict:
    """The signal generator against the demodulators the monitor runs, and the synthetic noise against a fixture.

    numpy ports of dsd_fm_demod() (angle of z_k conj(z_{k-1})) and dsd_am_demod() (|z| / carrier - 1) recover the
    deviation and depth the generator was asked for; the real captures' deviation and depth are measured the same way.
    tests/fixtures/iq/noise_floor.iq (cu8 noise, 16 LSB) through the 48 kHz replay chain's unset NFM plan is compared
    with the synthetic noise for that plan.
    """
    fs = FIXTURE_RATE_HZ
    rng = np.random.default_rng(job_seed("sanity"))
    nfm = harness_plans[(fs, NFM_WIDTH_DEFAULT_HZ, FM_KIND)]["taps"]
    am = harness_plans[(fs, AM_WIDTH_DEFAULT_HZ, AM_KIND)]["taps"]
    out = {}
    tone = {"family": "fm", "mod": "tone1000", "amount": 5000.0, "offset": 0.0}
    z = channel_filter(synth_carrier(tone, fs, fs + len(nfm) - 1, rng), nfm).astype(np.complex128)
    step = np.angle(z[1:] * np.conj(z[:-1]))
    out["fm_1k_at_5000_hz_dev_measured_hz"] = float(np.max(np.abs(step[fs // 10 :])) * fs / (2.0 * math.pi))
    tone = {"family": "am", "mod": "tone1000", "amount": 0.5, "offset": 0.0}
    z = channel_filter(synth_carrier(tone, fs, fs + len(am) - 1, rng), am)
    env = np.abs(z[fs // 10 :].astype(np.complex128))
    audio = env / np.mean(env) - 1.0
    out["am_1k_at_50pct_depth_measured_pct"] = float(50.0 * (np.max(audio) - np.min(audio)))
    sources = real_sources()
    for name in REAL_FM:
        z = channel_filter(sources[name]["iq"], nfm).astype(np.complex128)
        inst = np.angle(z[1:] * np.conj(z[:-1])) * fs / (2.0 * math.pi)
        inst -= np.mean(inst)
        out[f"{name}_dev_rms_hz"] = float(np.sqrt(np.mean(inst**2)))
        out[f"{name}_dev_p99_9_hz"] = float(np.percentile(np.abs(inst), 99.9))
    for name in REAL_AM:
        env = np.abs(channel_filter(sources[name]["iq"], am).astype(np.complex128))
        audio = env / np.mean(env) - 1.0
        out[f"{name}_depth_p99_pct"] = float(100.0 * np.percentile(np.abs(audio), 99.0))
    z, _ = load_cu8("noise_floor")
    zf = channel_filter(z, nfm)
    info = filter_info(nfm, None, fs)
    n_w = (len(zf) - info["lag"]) * WINDOW_PER_S // fs
    feats = window_features(
        window_sums(zf, window_bounds(fs, n_w, info["lag"]), info["lag"]), info["norm"]["beta_c"], info["norm"]["rho_n"]
    )
    out["noise_floor_fixture"] = {
        "windows": int(n_w),
        "cv2": np.percentile(feats["cv2"], (1, 50, 99)).tolist(),
        "c": np.percentile(feats["c"], (1, 50, 99)).tolist(),
    }
    return out


def cross_check(harness_plans: dict) -> dict:
    """Every designed plan against the numpy port; every analog plan against dsd_channel_lpf_design_analog()."""
    compared, max_abs, direct_mismatch, fallback = 0, 0.0, [], 0
    for (rate, width, _kind), rec in sorted(harness_plans.items()):
        if len(rec["taps"]) == 0:
            continue
        if width > 0 and rec["realizable"] and not rec["direct_same"]:
            direct_mismatch.append((rate, width))
        port = port_plan(rate, width)
        if port is None:
            fallback += 1
            continue
        if len(port) != len(rec["taps"]):
            max_abs = math.inf
            continue
        compared += 1
        max_abs = max(max_abs, float(np.max(np.abs(port - rec["taps"]))))
    return {
        "compared": compared,
        "max_abs_diff": max_abs,
        "fallback_plans": fallback,
        "direct_mismatch": direct_mismatch,
    }


def fmt(value, digits: int = 3) -> str:
    if value is None:
        return "-"
    if isinstance(value, float) and math.isinf(value):
        return "inf"
    if isinstance(value, float):
        if value != 0.0 and abs(value) < 10.0**-digits:
            return f"{value:.1e}"
        return f"{value:.{digits}f}"
    return str(value)


def fmt_rate(value) -> str:
    if value is None:
        return "-"
    if value == 0.0:
        return "0"
    return f"{value:.1e}" if value < 0.01 else f"{value:.3f}"


def fmt_list(values, digits: int = 3) -> str:
    return "-" if values is None else " / ".join(fmt(float(v), digits) for v in values)


def combo_id(combo: dict) -> str:
    return f"{combo['config'].label} @ {combo['plan'].name}"


def git_describe() -> str:
    try:
        proc = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True
        )
        dirty = subprocess.run(["git", "-C", str(ROOT), "diff", "--quiet", "HEAD"], check=False).returncode != 0
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return proc.stdout.strip() + ("-dirty" if dirty else "")


def failure_reasons(v: dict) -> list[str]:
    reasons = []
    if v["false_carrier"] > FALSE_CARRIER_TARGET:
        reasons.append(f"false CARRIER {fmt_rate(v['false_carrier'])}")
    if v["miss_cnr6"] > MISS_TARGET:
        reasons.append(f"6 dB miss {fmt_rate(v['miss_cnr6'])}")
    if v["pollution_ge6"] > POLLUTION_TARGET:
        reasons.append(f"pollution {fmt_rate(v['pollution_ge6'])}")
    if v["noise_undecided"] > NOISE_UNDECIDED_TARGET:
        reasons.append(f"noise undecided {fmt_rate(v['noise_undecided'])}")
    if not v["own_feasible"]:
        reasons.append("no thresholds meet the targets on this plan alone")
    return reasons


def members(runnable: list[dict], ck: str) -> list[str]:
    return [combo_id(c) for c in runnable if c["ckey"] == ck]


def scheme_section(lines: list[str], name: str, sch: dict, unique: dict, runnable: list[dict]) -> None:
    th = sch["thresholds"]
    _, multiple, _ = scheme_parts(name)
    lines.append(
        f"**{name}**: t1 {fmt(th['t1'], 2)}, t2 {fmt(th['t2'], 2)}, t3 {fmt(th['t3'], 2)}, t4 {fmt(th['t4'], 2)}; "
        f"{sch['passing']} of {sch['total']} distinct channel plans meet every target "
        f"({sch['own_feasible']} could on their own thresholds)."
    )
    lines.append("")
    if not sch["failing"]:
        return
    cut = sch["neff_cut"]
    if cut is not None:
        lines.append(f"Every plan with N_eff >= {cut:.0f} (at this window) passes. Failing plans:")
    else:
        lines.append("Failing plans:")
    lines.append("")
    for ck in sorted(sch["failing"], key=lambda k: unique[k]["neff"]):
        v = sch["verdicts"][ck]
        lines.append(
            f"- N_eff {unique[ck]['neff'] * multiple:.0f}, {', '.join(failure_reasons(v))}: "
            f"{'; '.join(members(runnable, ck))}"
        )
    lines.append("")


def lag_section(lines: list[str], unique: dict, noise_results: dict) -> None:
    """How the planned lag rule behaves across every plan: the residual noise correlation and the noise C it leaves."""
    rows = list(unique.values())
    lag_us = [1e6 * c["lag"] / c["plan"].channel_rate for c in rows]
    rho_h = [c["rho_taps_lag"] for c in rows]
    rho_n = [c["rho_noise_lag"] for c in rows]
    med = [noise_results[c["fkey"]]["pct"][1]["c"][2] for c in rows]
    top = [noise_results[c["fkey"]]["pct"][1]["c"][4] for c in rows]
    over = sorted(
        {
            f"{c['filter']} at {c['plan'].channel_rate} Hz ({c['rho_noise_lag']:+.3f})"
            for c in rows
            if abs(c["rho_noise_lag"]) >= LAG_RHO_LIMIT
        }
    )
    lines += [
        "## Lag rule",
        "",
        (
            f"L = smallest lag with |R_h(L)| / R_h(0) < {LAG_RHO_LIMIT} on the channel taps: L {min(c['lag'] for c in rows)} "
            f"to {max(c['lag'] for c in rows)} samples ({min(lag_us):.0f} to {max(lag_us):.0f} us); rho_h(L) "
            f"{min(rho_h):+.3f} to {max(rho_h):+.3f}. The noise the classifier sees also carries the last half-band "
            f"stage, so its correlation at L, rho_n(L), runs {min(rho_n):+.3f} to {max(rho_n):+.3f}; noise C (20 ms) has "
            f"its median at {min(med):.3f} to {max(med):.3f} and its 99.99 % point at {min(top):.3f} to {max(top):.3f}, "
            f"the bias (pi/4) rho_n(L) plus the window's own scatter, about 1/sqrt(N_eff)."
        ),
        "",
    ]
    if over:
        lines += [f"Plans whose noise correlation at L is still {LAG_RHO_LIMIT} or more: {'; '.join(over)}.", ""]


def sanity_section(lines: list[str], sanity: dict, combos: list[dict], noise_results: dict) -> None:
    ref = next(
        c for c in combos if c["runs"] and c["config"] == Config(FM_KIND, 0, "") and c["plan"].name == "replay-48k"
    )
    synth = noise_results[ref["fkey"]]["pct"][1]
    fixture = sanity["noise_floor_fixture"]
    real = "; ".join(
        f"{name} {sanity[f'{name}_dev_rms_hz']:.0f} Hz RMS / {sanity[f'{name}_dev_p99_9_hz']:.0f} Hz at 99.9 %"
        for name in REAL_FM
    )
    lines += [
        "## Sanity checks",
        "",
        (
            "numpy ports of dsd_fm_demod() (angle of z_k conj(z_{k-1})) and dsd_am_demod() (|z| / carrier - 1) on the "
            f"generator's output at 48 kHz: a 1 kHz tone at 5000 Hz deviation through the 16 kHz plan reads "
            f"{sanity['fm_1k_at_5000_hz_dev_measured_hz']:.0f} Hz peak; a 1 kHz tone at 50 % depth through the 6 kHz AM "
            f"plan reads {sanity['am_1k_at_50pct_depth_measured_pct']:.1f} %. Real captures through the same plans: "
            f"{real}; am_airband_real depth {sanity['am_airband_real_depth_p99_pct']:.0f} % at 99 %."
        ),
        "",
        (
            f"tests/fixtures/iq/noise_floor.iq (cu8 receiver noise, {fixture['windows']} windows) through the 48 kHz "
            f"replay chain's unset NFM plan: CV2 1/50/99 % {fmt_list(fixture['cv2'])}, C {fmt_list(fixture['c'])}; the "
            f"synthetic noise for that plan: CV2 {fmt_list(synth['cv2'][1:4])}, C {fmt_list(synth['c'][1:4])}."
        ),
        "",
    ]


def cw_snr_for_coherence(target: float) -> float:
    """CW-to-noise ratio (dB) at which a CW alone gives the coherence target: |E e^(j phi)|^2 = target, where
    E e^(j phi) = (sqrt(pi g) / 2) e^(-g/2) (I0(g/2) + I1(g/2)) for a CW at SNR g in complex Gaussian noise."""

    def excess(g: float) -> float:
        half = g / 2.0
        return (math.pi * g / 4.0) * (special.ive(0, half) + special.ive(1, half)) ** 2 - target

    if target <= 0.0:
        return -math.inf
    return 10.0 * math.log10(optimize.brentq(excess, 1e-9, 1e4))


def limits_section(lines: list[str], name: str, sch: dict, unique: dict, noise_windows: int) -> None:
    """What the model leaves out, with the numbers that bound it."""
    _, multiple, _ = scheme_parts(name)
    t2 = [effective_thresholds(cb, name, sch["thresholds"])["t2"] for cb in unique.values()]
    lo, hi = cw_snr_for_coherence(min(t2)), cw_snr_for_coherence(max(t2))
    events = FALSE_CARRIER_TARGET * noise_windows / multiple
    span = f"{lo:+.1f} dB" if abs(hi - lo) < 0.05 else f"{lo:+.1f} dB (widest plans) to {hi:+.1f} dB (narrowest)"
    lines += [
        "## Limits of the model",
        "",
        (
            f"- Rates near 1e-4 rest on {noise_windows // multiple} noise windows per plan at {20 * multiple} ms "
            f"(about {events:.0f} events at 1e-4), so a false CARRIER rate quoted at the target carries roughly "
            f"+/-{100 / math.sqrt(max(events, 1)):.0f} % sampling error."
        ),
        (
            "- The noise is complex Gaussian (checked against the cu8 noise_floor.iq fixture under Sanity checks). "
            "Spurs, birdies, the RTL DC spike and adjacent-channel energy are not modelled. A CW inside the channel is "
            f"a carrier to the coherence test: with {name} a lone CW opens CARRIER from about {span} relative to the "
            "in-channel noise, so an in-channel birdie that strong would hold the floor estimate still."
        ),
        (
            "- Decisions are per window; no hang time, hysteresis or floor tracker is modelled. Fading comes only from "
            "the real captures (nfm_squelch_real_b carries tens-of-Hz flutter: its 20 ms windows vary by about "
            "+/-3 dB), not from a channel model."
        ),
        "",
    ]


def effective_thresholds(combo: dict, scheme: str, th: dict) -> dict:
    """The scheme's thresholds in CV2 and coherence units for one plan (norm thresholds scale with its N_eff)."""
    space, multiple, coh = scheme_parts(scheme)
    if space == "raw":
        return {k: th[k] for k in ("t1", "t2", "t3", "t4")}
    root = math.sqrt(combo["norm"]["neff"] * multiple)
    bias = combo["norm"]["bias"][coh]
    return {
        "t1": 1.0 + th["t1"] / root,
        "t2": bias + th["t2"] / root,
        "t3": 1.0 + th["t3"] / root,
        "t4": bias + th["t4"] / root,
    }


def rates_table(lines: list[str], name: str, sch: dict, unique: dict, noise_windows: int) -> None:
    lines += [
        f"## Decision rates, {name}",
        "",
        (
            "| plan (first chain) | N_eff | t1 / t2 / t3 / t4 here | false CARRIER (windows) | noise undecided | "
            "miss 6 dB | miss >=6 dB | miss 3 dB | miss 0 dB | pollution >=6 dB | pollution 3 dB | pollution 0 dB | "
            "edge miss 6 dB | slack CARRIER | slack NOISE | pass | most-missed 6 dB variants |"
        ),
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    _, multiple, _ = scheme_parts(name)
    windows = noise_windows // multiple
    for ck, cb in sorted(unique.items(), key=lambda kv: (kv[1]["config"].kind, kv[1]["neff"])):
        v = sch["verdicts"][ck]
        worst = "; ".join(f"{w['variant']} {fmt_rate(w['miss'])}" for w in v.get("worst_variants", [])) or "-"
        eff = effective_thresholds(cb, name, sch["thresholds"])
        eff_text = " / ".join(fmt(eff[k], 3) for k in ("t1", "t2", "t3", "t4"))
        events = round(v["false_carrier"] * windows)
        lines.append(
            f"| {combo_id(cb)} | {cb['neff'] * multiple:.0f} | {eff_text} | {fmt_rate(v['false_carrier'])} ({events}) | "
            f"{fmt_rate(v['noise_undecided'])} | {fmt_rate(v['miss_cnr6'])} | {fmt_rate(v['miss_ge6'])} | "
            f"{fmt_rate(v['miss_cnr3'])} | {fmt_rate(v['miss_cnr0'])} | {fmt_rate(v['pollution_ge6'])} | "
            f"{fmt_rate(v['pollution_cnr3'])} | {fmt_rate(v['pollution_cnr0'])} | {fmt_rate(v['miss_edge6'])} | "
            f"{fmt(v.get('slack_carrier_rule'), 2)} | {fmt(v.get('slack_noise_rule'), 2)} | "
            f"{'yes' if v['pass'] else '**no**'} | {worst} |"
        )
    lines += [
        "",
        (
            "Slack is the distance, in the scheme's feature units, from the chosen thresholds to the nearest threshold "
            "pair at which this plan would miss a target: its measured margin. Edge variants (occupied band past the "
            "protected half-width) are reported, not gated."
        ),
        "",
    ]


def write_report(
    out_dir: Path, meta: dict, combos: list[dict], noise_results: dict, carrier_results: dict, analysis: dict
) -> tuple[Path, Path]:
    schemes, best, unique = analysis["schemes"], analysis["recommended"], analysis["unique"]
    runnable = [c for c in combos if c["runs"]]
    refused = [c for c in combos if not c["runs"]]
    cpct = {ck: {m: carrier_percentiles(carrier_results[ck], m) for m in WINDOW_MULTIPLES} for ck in unique}
    planned = schemes[PLANNED_SCHEME]
    lines = ["# Auto-squelch classifier design gate", ""]
    lines += [
        (
            f"Generated by `{meta['command']}` at {meta['git']} ({meta['mode']} mode, {meta['noise_windows']} noise "
            f"windows of 20 ms per filter, base seed {BASE_SEED}, numpy {np.__version__}, scipy {meta['scipy']}, "
            f"{meta['elapsed_s']:.0f} s)."
        ),
        "",
        (
            f"Targets per window: false CARRIER on noise <= {FALSE_CARRIER_TARGET:g}; an in-channel carrier at 6 dB CNR "
            f"not called CARRIER <= {MISS_TARGET:g}; a carrier at >= 6 dB called NOISE (floor pollution) <= "
            f"{POLLUTION_TARGET:g}; noise not called NOISE <= {NOISE_UNDECIDED_TARGET:g}."
        ),
        "",
        "## Verdict",
        "",
        "The planned classifier (20 ms windows, coherence C, fixed thresholds):",
        "",
    ]
    scheme_section(lines, PLANNED_SCHEME, planned, unique, runnable)
    if best != PLANNED_SCHEME:
        lines += [
            "The recommended scheme (the first, in order of how small a change it is, that passes the most plans):",
            "",
        ]
        scheme_section(lines, best, schemes[best], unique, runnable)
    limits_section(lines, best, schemes[best], unique, meta["noise_windows"])
    lines += [
        "## Schemes",
        "",
        (
            "space: raw = CARRIER if CV2 <= t1 or coherence >= t2, NOISE if CV2 >= t3 and coherence <= t4; norm = the "
            "same rule on X = (CV2 - 1) sqrt(N_eff) and Y = (coherence - bias) sqrt(N_eff), N_eff = N / sum rho_n(m)^2 "
            "and the bias from the noise model (both computable at plan time from the channel and half-band taps). "
            "Window: 20 ms, or 40 ms (two sample-exact 20 ms windows pooled). Coherence: C = |mean(u_k conj(u_{k-L}))|, "
            "u = z/|z| (planned); C-beta = |mean(...) - beta|, beta = (pi/4) rho_n(L) 2F1(1/2,1/2;2;rho_n(L)^2), its "
            "value on noise; Cz = |sum z_k conj(z_{k-L})| / sum |z_k|^2; Cz-rho = |sum z_k conj(z_{k-L}) / sum |z_k|^2 - "
            "rho_n(L)|. rho_n is the noise autocorrelation at the channel output (channel taps with the last half-band "
            "stage)."
        ),
        "",
        (
            "| scheme | t1 | t2 | t3 | t4 | plans passing | worst false CARRIER | worst 6 dB miss | worst pollution | "
            "worst noise undecided | all pass from N_eff (this window) |"
        ),
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name, sch in schemes.items():
        t = sch["thresholds"]
        cut = sch["neff_cut"]
        lines.append(
            f"| {name} | {fmt(t['t1'], 2)} | {fmt(t['t2'], 2)} | {fmt(t['t3'], 2)} | {fmt(t['t4'], 2)} | "
            f"{sch['passing']}/{sch['total']} | {fmt_rate(sch['worst']['false_carrier'])} | "
            f"{fmt_rate(sch['worst']['miss_cnr6'])} | {fmt_rate(sch['worst']['pollution_ge6'])} | "
            f"{fmt_rate(sch['worst']['noise_undecided'])} | {'-' if cut is None else f'{cut:.0f}'} |"
        )
    xc = meta["xcheck"]
    lines += [
        "",
        "## Channel taps",
        "",
        (
            f"From `{meta['harness']}`, built against `{meta['build_dir']}` with the {LINK_TEMPLATE_TARGET} link line: "
            f"{meta['harness_plans']} full_demod() plans and the half-band taps (hb15 {meta['hb15_len']}, hb31 "
            f"{meta['hb31_len']}). The numpy port of dsd_firdes_low_pass() matches all {xc['compared']} designed plans "
            f"to {xc['max_abs_diff']:.1e}; {xc['fallback_plans']} plans are the 63-tap fallback prototype (harness only); "
            f"analog plans that differ from a direct dsd_channel_lpf_design_analog() call: {len(xc['direct_mismatch'])}."
        ),
        "",
    ]
    sanity_section(lines, meta["sanity"], combos, noise_results)
    lines += [
        "## Supported combinations",
        "",
        (
            "Chains: rtl = RTL-SDR/rtl_tcp at its DSP bandwidth; device = the rate a fixed-grid device forces (rate_in "
            "stays the requested bandwidth, which decides the unset NFM default's channel LPF); replay = I/Q replay "
            "(rate_in = sample_rate / base_decimation, demod rate = rate_in / post_downsample). Every window and lag is "
            "at the channel rate (rate_out x post_downsample). L is the planned lag; rho_h(L) the channel taps' "
            "normalised autocorrelation there, rho_n(L) the noise's (with the half-band stage); N the mean 20 ms window "
            "length, N_eff = N / sum rho_n(m)^2."
        ),
        "",
        (
            "| config | chain | rate_in | rate_out | pd | channel rate | filter | taps | protected Hz | L | L us | "
            "rho_h(L) | rho_n(L) | N | N_eff |"
        ),
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for c in runnable:
        p = c["plan"]
        lines.append(
            f"| {c['config'].label} | {p.name} | {p.rate_in} | {p.rate_out} | {p.post_downsample} | {p.channel_rate} | "
            f"{c['filter']} | {len(c['taps'])} | {c['protected_hz']} | {c['lag']} | "
            f"{1e6 * c['lag'] / p.channel_rate:.0f} | {c['rho_taps_lag']:+.3f} | {c['rho_noise_lag']:+.3f} | "
            f"{c['window_samples']:.1f} | {c['neff']:.0f} |"
        )
    lines += ["", f"Refused combinations (the stream never runs them): {len(refused)}", ""]
    by_config: dict = {}
    for c in refused:
        by_config.setdefault(c["config"].label, []).append(f"{c['plan'].name} ({c['why']})")
    for label, items in by_config.items():
        lines.append(f"- {label}: {'; '.join(items)}")
    lines.append("")
    lag_section(lines, unique, noise_results)
    lines += [
        "## Feature distributions (20 ms)",
        "",
        (
            "Percentiles 0.01 / 1 / 50 / 99 / 99.99 %. Noise: channel-filtered complex Gaussian noise. Carrier: every "
            "in-channel variant at 6 dB CNR, pooled (all CNRs and the 40 ms window are in report.json). CV2 margin: noise "
            "CV2 at 0.01 % minus carrier CV2 at 99 %; C margin: carrier C at 1 % minus noise C at 99.99 %. A negative "
            "margin means that feature alone does not separate those tails; the decision combines both features, so the "
            "decision rates are the gate."
        ),
        "",
        "| plan (first chain) | noise CV2 | noise C | noise C-beta | carrier CV2 | carrier C | CV2 margin | C margin |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for ck, cb in sorted(unique.items(), key=lambda kv: (kv[1]["config"].kind, kv[1]["neff"])):
        npct = noise_results[cb["fkey"]]["pct"][1]
        c6 = cpct[ck][1]["cnr6"]
        lines.append(
            f"| {combo_id(cb)} | {fmt_list(npct['cv2'])} | {fmt_list(npct['c'])} | {fmt_list(npct['cdb'])} | "
            f"{fmt_list(c6['cv2'])} | {fmt_list(c6['c'])} | {npct['cv2'][0] - c6['cv2'][3]:+.3f} | "
            f"{c6['c'][1] - npct['c'][4]:+.3f} |"
        )
    lines.append("")
    rates_table(lines, PLANNED_SCHEME, planned, unique, meta["noise_windows"])
    if best != PLANNED_SCHEME:
        rates_table(lines, best, schemes[best], unique, meta["noise_windows"])
    md_path = out_dir / "report.md"
    md_path.write_text("\n".join(lines))

    def combo_json(c: dict) -> dict:
        p = c["plan"]
        out = {
            "config": c["config"].label,
            "kind": KIND_LABEL[c["config"].kind],
            "width_hz": c["config"].width_hz,
            "lpf_env": c["config"].lpf_env,
            "chain": p.name,
            "source": p.source,
            "rate_in": p.rate_in,
            "rate_out": p.rate_out,
            "post_downsample": p.post_downsample,
            "channel_rate": p.channel_rate,
            "half_band": p.hb,
            "runs": c["runs"],
        }
        if not c["runs"]:
            out["why"] = c["why"]
            return out
        out.update(
            {
                key: c[key]
                for key in (
                    "filter",
                    "protected_hz",
                    "lag",
                    "rho_taps_lag",
                    "rho_noise_lag",
                    "window_samples",
                    "neff",
                    "neff_taps",
                    "enbw_hz",
                    "norm",
                )
            }
        )
        out.update({"taps": len(c["taps"]), "plan_key": c["ckey"]})
        return out

    def clean(value):
        if isinstance(value, float) and math.isinf(value):
            return None
        if isinstance(value, dict):
            return {str(k): clean(v) for k, v in value.items()}
        if isinstance(value, (list, tuple)):
            return [clean(v) for v in value]
        if isinstance(value, np.generic):
            return value.item()
        return value

    report = {
        "meta": meta,
        "targets": {
            "false_carrier": FALSE_CARRIER_TARGET,
            "miss_cnr6": MISS_TARGET,
            "pollution_ge6": POLLUTION_TARGET,
            "noise_undecided": NOISE_UNDECIDED_TARGET,
        },
        "percentiles": PERCENTILES,
        "cnrs_db": CNRS_DB,
        "planned_scheme": PLANNED_SCHEME,
        "recommended_scheme": best,
        "schemes": schemes,
        "combos": [combo_json(c) for c in combos],
        "plans": {
            ck: {
                "members": members(runnable, ck),
                "neff": cb["neff"],
                "noise": {"windows": noise_results[cb["fkey"]]["n"], "percentiles": noise_results[cb["fkey"]]["pct"]},
                "carrier_in_channel": cpct[ck],
                "variants": [{**v, "label": variant_label(v)} for v in carrier_results[ck]["variants"]],
            }
            for ck, cb in unique.items()
        },
    }
    json_path = out_dir / "report.json"
    json_path.write_text(json.dumps(clean(report), indent=1))
    return md_path, json_path


# --------------------------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--quick", action="store_true", help="short run: fewer noise windows, shorter carriers")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "squelch_model", help="report directory")
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / "build" / "dev-debug",
        help="configured and built tree whose static libraries the tap harness links",
    )
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1), help="worker processes")
    parser.add_argument(
        "--noise-windows",
        type=int,
        default=0,
        help=f"20 ms noise windows per filter (default {FULL_NOISE_WINDOWS}, {QUICK_NOISE_WINDOWS} with --quick)",
    )
    args = parser.parse_args(argv)
    noise_windows = args.noise_windows or (QUICK_NOISE_WINDOWS if args.quick else FULL_NOISE_WINDOWS)
    started = time.time()
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    exe = build_harness(args.build_dir.resolve(), out_dir)
    plans = rate_plans()
    requests = set()
    for plan in plans:
        for width in (*NFM_WIDTHS_HZ, NFM_WIDTH_DEFAULT_HZ, 0):
            requests.add((plan.rate_out, width, FM_KIND))
        for width in AM_WIDTHS_HZ:
            requests.add((plan.rate_out, width, AM_KIND))
    header, harness_plans = run_harness(exe, sorted(requests))
    if (header["transition_hz"], header["guard_hz"], header["max_taps"]) != (TRANSITION_HZ, GUARD_HZ, ANALOG_MAX_TAPS):
        raise SystemExit(f"analog channel constants changed ({header}); update tools/squelch_model.py")
    xcheck = cross_check(harness_plans)
    hb_taps = {name: np.asarray(header[name], dtype=np.float64) for name in ("hb15", "hb31")}
    combos = build_combos(plans, harness_plans, hb_taps)
    jobs = make_jobs(combos, hb_taps, args.quick, noise_windows)
    n_noise = sum(1 for j in jobs if "n_windows" in j)
    print(
        f"{len(combos)} combinations, {sum(1 for c in combos if c['runs'])} run: {n_noise} filters x "
        f"{noise_windows} noise windows, {len(jobs) - n_noise} carrier plans, {args.jobs} workers",
        flush=True,
    )
    real_sources()  # fail early on a missing fixture
    noise_results, carrier_results = {}, {}
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for done, (kind, result) in enumerate(pool.map(run_job, jobs, chunksize=1), start=1):
            (noise_results if kind == "noise" else carrier_results)[result["key"]] = result
            if done % 50 == 0 or done == len(jobs):
                print(f"  {done}/{len(jobs)} jobs ({time.time() - started:.0f} s)", flush=True)
    sanity = signal_sanity(harness_plans)
    print("schemes:", flush=True)
    analysis = analyse(combos, noise_results, carrier_results)
    command = ["python3", "tools/squelch_model.py", *(sys.argv[1:] if argv is None else argv)]
    meta = {
        "command": " ".join(command),
        "git": git_describe(),
        "mode": "quick" if args.quick else "full",
        "noise_windows": noise_windows,
        "scipy": scipy.__version__,
        "python": platform.python_version(),
        "elapsed_s": time.time() - started,
        "harness": str(HARNESS_SRC.relative_to(ROOT)),
        "build_dir": str(args.build_dir),
        "harness_plans": len(harness_plans),
        "hb15_len": len(header["hb15"]),
        "hb31_len": len(header["hb31"]),
        "xcheck": xcheck,
        "sanity": sanity,
    }
    md_path, json_path = write_report(out_dir, meta, combos, noise_results, carrier_results, analysis)
    runnable = [c for c in combos if c["runs"]]
    best = analysis["recommended"]
    print(f"taps: {xcheck['compared']} designed plans match the numpy port to {xcheck['max_abs_diff']:.1e}")
    for name in dict.fromkeys((PLANNED_SCHEME, best)):
        sch = analysis["schemes"][name]
        t = sch["thresholds"]
        cut = sch["neff_cut"]
        print(
            f"{'planned' if name == PLANNED_SCHEME else 'recommended'} {name}: t1 {fmt(t['t1'], 2)} "
            f"t2 {fmt(t['t2'], 2)} t3 {fmt(t['t3'], 2)} t4 {fmt(t['t4'], 2)}; {sch['passing']}/{sch['total']} "
            f"plans pass; all pass from N_eff {'-' if cut is None else f'{cut:.0f}'}"
        )
        for ck in sch["failing"]:
            reasons = ", ".join(failure_reasons(sch["verdicts"][ck]))
            neff = analysis["unique"][ck]["neff"] * scheme_parts(name)[1]
            print(f"    fails: {'; '.join(members(runnable, ck))} (N_eff {neff:.0f}: {reasons})")
    verdict = analysis["schemes"][PLANNED_SCHEME]["passing"] == analysis["schemes"][PLANNED_SCHEME]["total"]
    print(
        f"verdict: the planned classifier {'separates' if verdict else 'does NOT separate'} the classes at every "
        f"supported combination"
    )
    print(f"report: {md_path}\n        {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
