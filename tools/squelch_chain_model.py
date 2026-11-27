# SPDX-License-Identifier: GPL-3.0-or-later
"""The squelch model tools' chain model (issue #899): what reaches the channel filter and the tracker, as the decoder
runs it. tools/squelch_model.py, tools/squelch_paths_model.py and tools/noise_squelch_model.py import it.

  - The production chain identity (RatePlan): the capture rate, the half-band passes, the live step, the replay
    reader's resampler (Reader) and the complex rate, with ports of the planners that choose them
    (rtl_downsample_passes_for_rate_in(), rate_plan_passes(), the rate converter's ratio, the general replay plan) and
    of iq_resampler.c's design.
  - Streaming ports of the chain's stages (FirStream, ChainStream: the reader's polyphase bank, the half-band
    decimators centred and looking ahead as simd_hb_decim2_complex() runs them, the channel FIR), every stage's
    history and the reader's phase kept across pushes.
  - The tracker's 20 ms sub-windows and the shipped 40 ms decisions (SubWindowStream, FrozenStream,
    classify_windows(): src/dsp/squelch_floor.c).
  - The noise model: the half-band recursion, the channel taps, every output phase of a reader chain (impulses
    streamed through the ported chain in bounded pushes), and the floor plan designed from them (plan_from_cov(): dsd_squelch_floor_plan_design_periodic_mean(), the averaged or the full
    periodic design chosen on the C reader model's bound).

Not a script; offline, CI does not run it.
"""

from __future__ import annotations

import dataclasses
import math
import zlib

import numpy as np
from scipy import signal as sps
from scipy import special

WINDOW_PER_S = 50  # 20 ms windows
LAG_RHO_LIMIT = 0.1

MAX_DOWNSAMPLE_PASSES = 10  # kMaxDownsamplePasses, DSD_IQ_REPLAY_MAX_PLAN_PASSES
# rtl_choose_passes_near_good_rate() (src/io/radio/rtl_sdr_fm.cpp) and the capture limits it keeps to.
RTL_GOOD_RATES_HZ = (960000, 1024000, 1200000, 1536000, 1920000, 2048000, 2400000)
RTL_MIN_CAPTURE_HZ = 225000
RTL_MAX_CAPTURE_HZ = 3200000
RATIO_MAX_TERM = 4096  # DSD_RATE_PLAN_MAX_RATIO_TERM, DSD_RATE_CONVERTER_MAX_TERM
RATE_CONVERTER_TOLERANCE_PPM = 250  # DSD_RATE_CONVERTER_TOLERANCE_PPM
RATE_CONVERTER_MIN_IN_HZ = 1000
RATE_CONVERTER_MAX_DECIMATION = 16
# src/dsp/iq_resampler.c: the Kaiser design's attenuation, the pass edge's share of the input rate, the bank's limits.
READER_DESIGN_DB = 65.0
READER_MAX_PASS_FRACTION = 0.45
READER_MAX_TAPS_PER_PHASE = 256
READER_MAX_BANK_TAPS = 4096 * 256
READER_MAX_PPM = 250.0
REPLAY_MAX_UPSAMPLE_RATIO = 9  # DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO


@dataclasses.dataclass(frozen=True)
class Reader:
    """The replay or network reader's resampler (src/dsp/iq_resampler.c) as rtl_iq_resampler_open() configures it."""

    source_hz: int
    out_hz: int  # the DSP input rate, D x 2^k
    L: int
    M: int
    taps_per_phase: int
    pass_hz: float
    stop_hz: float

    @property
    def key(self) -> str:
        return (
            f"{self.source_hz}>{self.out_hz}:{self.L}/{self.M}:T{self.taps_per_phase}:{self.pass_hz:g}/{self.stop_hz:g}"
        )


@dataclasses.dataclass(frozen=True)
class RatePlan:
    """A rate chain's production identity: what reaches the channel filter, and at which rate."""

    name: str
    source: str  # rtl | device | replay | reader
    rate_in: int  # the stream's rate_in: decides the unset NFM default's channel-LPF enable
    capture_hz: int  # the rate into the half-band cascade (a reader's output rate on a reader row)
    passes: int  # half-band passes: hb31 first, hb15 after
    complex_rate: int  # where the channel filter, the plan and the tracker run
    post_downsample: int = 1
    step: tuple | None = None  # (L, M) of the live step: stepped rows are scoped out
    reader: Reader | None = None

    @property
    def channel_rate(self) -> int:
        return self.complex_rate

    @property
    def rate_out(self) -> int:
        return self.complex_rate // self.post_downsample

    @property
    def source_hz(self) -> int:
        return self.reader.source_hz if self.reader else self.capture_hz

    @property
    def stepped(self) -> bool:
        return self.step is not None

    @property
    def chain_key(self) -> str:
        reader = self.reader.key if self.reader else "-"
        step = f"step {self.step[0]}/{self.step[1]}" if self.step else "-"
        return f"{reader}|{self.capture_hz}/p{self.passes}|{step}|{self.complex_rate}"

    @property
    def chain_label(self) -> str:
        parts = []
        if self.reader:
            r = self.reader
            parts.append(f"{r.source_hz} Hz reader L/M {r.L}/{r.M} to {r.out_hz} Hz")
        else:
            parts.append(f"{self.capture_hz} Hz")
        parts.append(f"{self.passes} passes" if self.passes != 1 else "1 pass")
        if self.step:
            parts.append(f"step {self.step[0]}/{self.step[1]}")
        return ", ".join(parts) + f" -> {self.complex_rate} Hz"


def rtl_passes(rate_in_hz: int) -> int:
    """rtl_downsample_passes_for_rate_in() with rtl_choose_passes_near_good_rate() (src/io/radio/rtl_sdr_fm.cpp)."""
    factor = 1000000 // rate_in_hz + 1
    if factor <= 1:
        return 0
    log2 = factor.bit_length() - 1
    suggested = max(0, min(MAX_DOWNSAMPLE_PASSES, log2 if factor & (factor - 1) == 0 else log2 + 1))
    best, best_err = suggested, None
    for delta in (-1, 0, 1):
        p = max(0, min(MAX_DOWNSAMPLE_PASSES, suggested + delta))
        capture = rate_in_hz << p
        if capture < RTL_MIN_CAPTURE_HZ or capture > RTL_MAX_CAPTURE_HZ:
            continue
        for good in RTL_GOOD_RATES_HZ:
            err = abs(capture - good)
            if best_err is None or err < best_err:
                best_err, best = err, p
    return best


def rate_plan_passes(nominal_hz: int, target_hz: int, max_passes: int = MAX_DOWNSAMPLE_PASSES) -> int:
    """rate_plan_passes() (src/io/iq/rate_plan.c): the most passes that keep nominal / 2^p at or above the target."""
    p = 0
    while p < max_passes and (target_hz << (p + 1)) <= nominal_hz:
        p += 1
    return p


def best_ratio(p: int, q: int, limit: int = RATIO_MAX_TERM) -> tuple[int, int] | None:
    """rate_converter_best_ratio(): the last convergent of p/q with both terms in bound, or the semiconvergent after it
    when that is closer."""
    h_prev2, h_prev, k_prev2, k_prev = 0, 1, 1, 0
    num, den = p, q
    while den != 0:
        a = num // den
        h, k = a * h_prev + h_prev2, a * k_prev + k_prev2
        if h > limit or k > limit:
            t = a - 1
            if h_prev > 0:
                t = min(t, (limit - h_prev2) // h_prev)
            if k_prev > 0:
                t = min(t, (limit - k_prev2) // k_prev)
            best_h, best_k = h_prev, k_prev
            if t >= 1:
                sh, sk = t * h_prev + h_prev2, t * k_prev + k_prev2
                if best_k < 1 or abs(sh * q - p * sk) * best_k <= abs(best_h * q - p * best_k) * sk:
                    best_h, best_k = sh, sk
            return (best_h, best_k) if best_h >= 1 and best_k >= 1 else None
        h_prev2, h_prev, k_prev2, k_prev = h_prev, h, k_prev, k
        num, den = den, num - a * den
    return (h_prev, k_prev) if h_prev >= 1 and k_prev >= 1 else None


def converter_ratio(from_hz: int, to_hz: int) -> tuple[int, int] | None:
    """dsd_rate_converter_ratio(): the reduced L/M, or the best within the term bound and 250 ppm; None when none."""
    if from_hz == to_hz:
        return 1, 1
    if from_hz < RATE_CONVERTER_MIN_IN_HZ or to_hz <= 0 or from_hz > to_hz * RATE_CONVERTER_MAX_DECIMATION:
        return None
    g = math.gcd(from_hz, to_hz)
    p, q = to_hz // g, from_hz // g
    ratio = (p, q) if p <= RATIO_MAX_TERM and q <= RATIO_MAX_TERM else best_ratio(p, q)
    if ratio is None or abs(ratio[0] * q - p * ratio[1]) * 1000000 > RATE_CONVERTER_TOLERANCE_PPM * p * ratio[1]:
        return None
    return ratio


def reader_for(source_hz: int, out_hz: int, final_hz: int) -> Reader | None:
    """rtl_iq_resampler_open() -> dsd_iq_resampler_configure(): the reader for source_hz to out_hz, or None when the
    rates are equal (no reader); raises when the resampler would refuse them."""
    if source_hz == out_hz:
        return None
    ratio = converter_ratio(source_hz, out_hz)
    if ratio is None:
        raise ValueError(f"no reader ratio for {source_hz} Hz to {out_hz} Hz")
    L, M = ratio
    if abs((L * source_hz / (M * out_hz) - 1.0) * 1e6) > READER_MAX_PPM:
        raise ValueError(f"the reader ratio {L}/{M} is too far from {source_hz} Hz to {out_hz} Hz")
    protect = float(int(0.45 * float(final_hz)))
    pass_hz = min(protect, READER_MAX_PASS_FRACTION * source_hz)
    stop_hz = min(source_hz - pass_hz, out_hz - protect)
    if not stop_hz > pass_hz:
        raise ValueError(f"the reader's band edges do not fit {source_hz} Hz to {out_hz} Hz")
    fh = float(source_hz) * L
    order = (READER_DESIGN_DB - 7.95) / (2.285 * 2.0 * math.pi * (stop_hz - pass_hz) / fh)
    per_phase = math.ceil((math.ceil(order) + 1.0) / L)
    if per_phase > READER_MAX_TAPS_PER_PHASE:
        raise ValueError(f"the reader for {source_hz} Hz to {out_hz} Hz needs too many taps")
    taps = max(4, (per_phase + 3) & ~3)
    if taps > READER_MAX_TAPS_PER_PHASE or L * taps > READER_MAX_BANK_TAPS:
        raise ValueError(f"the reader for {source_hz} Hz to {out_hz} Hz needs too many taps")
    return Reader(source_hz, out_hz, L, M, taps, pass_hz, stop_hz)


def reader_prototype(r: Reader) -> np.ndarray:
    """iq_resampler_design_bank()'s Kaiser-windowed sinc as one prototype of L x T taps (float taps, each phase scaled
    to unity gain on average), for scipy's upfirdn: output n = sum_j h[j] x_up[n M - j], the reader's own phase walk."""
    n = r.L * r.taps_per_phase
    fh = r.source_hz * r.L
    fc = (r.pass_hz + r.stop_hz) / (2.0 * fh)
    centre = (n - 1) / 2.0
    beta = 0.1102 * (READER_DESIGN_DB - 8.7)
    t = np.arange(n, dtype=np.float64) - centre
    x = 2.0 * fc * t
    with np.errstate(invalid="ignore", divide="ignore"):
        sinc = np.where(np.abs(x) > 1e-12, np.sin(math.pi * x) / (math.pi * x), 1.0)
    u = t / centre
    w = np.i0(beta * np.sqrt(np.maximum(1.0 - u * u, 0.0))) / np.i0(beta)
    h = 2.0 * fc * sinc * w
    total = float(np.sum(h))
    scale = r.L / total if total > 0.0 else 1.0
    return (h.astype(np.float32).astype(np.float64) * scale).astype(np.float32).astype(np.float64)


def general_rate(source_hz: int, dsp_hz: int) -> tuple[int, int]:
    """dsd_iq_plan_general_rate(): the rate R = D x 2^k a source resamples to (or is), and k."""
    for k in range(MAX_DOWNSAMPLE_PASSES + 1):
        if dsp_hz << k == source_hz:
            return source_hz, k
    if source_hz < dsp_hz:
        if dsp_hz > source_hz * REPLAY_MAX_UPSAMPLE_RATIO:
            raise ValueError(f"{source_hz} Hz needs more than a {REPLAY_MAX_UPSAMPLE_RATIO}x upsample")
        return dsp_hz, 0
    k = 0
    while k < MAX_DOWNSAMPLE_PASSES and (dsp_hz << (k + 1)) <= source_hz:
        k += 1
    if source_hz >= 2 * (dsp_hz << k):
        raise ValueError(f"{source_hz} Hz is too fast for {dsp_hz} Hz")
    return dsp_hz << k, k


def device_step(capture_hz: int, target_hz: int, passes: int) -> tuple[int, int]:
    """rate_plan_pick_step()'s L/M for capture to target x 2^passes (exact where both terms fit)."""
    want = target_hz << passes
    g = math.gcd(want, capture_hz)
    p, q = want // g, capture_hz // g
    if p <= RATIO_MAX_TERM and q <= RATIO_MAX_TERM:
        return p, q
    ratio = best_ratio(p, q)
    return ratio if ratio else (p, q)


# --------------------------------------------------------------------------------------------- DSP helpers


def autocorr(taps: np.ndarray) -> np.ndarray:
    """Full autocorrelation of real taps, lag 0 at the centre."""
    return np.correlate(taps, taps, mode="full")


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


class FirStream:
    """A streaming FIR with rational rate change, as the chain's stages run: output n = sum_j h[j] x_up[n down +
    advance - j], x_up the input with up - 1 zeros after each sample (upfirdn's convention, the reader's phase walk).
    The stream starts on silence and keeps its history across pushes. With up == 1 (no interpolation: the half-bands
    and the channel FIR), output n is made once input n down + advance is in, so a push of any length returns the
    outputs it completes; with up > 1 (the reader), advance is 0 and a push holds a whole number of `down` samples."""

    def __init__(self, h: np.ndarray, up: int = 1, down: int = 1, advance: int = 0):
        self.h = np.asarray(h, dtype=np.float64)
        self.up, self.down, self.advance = up, down, advance
        if up == 1:
            # The inputs from absolute index `base` on (silence before the stream), and the next output to make.
            self.base = -(len(self.h) - 1)
            self.buf = np.zeros(len(self.h) - 1, dtype=np.complex128)
            self.n_in = 0
            self.n_out = 0
            return
        if advance:
            raise ValueError("a look-ahead needs up 1")
        span = -(-len(self.h) // up)
        self.hist_len = down * -(-span // down)
        self.hist = np.zeros(self.hist_len, dtype=np.complex128)

    def push(self, x: np.ndarray) -> np.ndarray:
        if self.up == 1:
            return self._push_decimating(np.asarray(x, dtype=np.complex128))
        if len(x) % self.down:
            raise ValueError("a push must hold whole output periods")
        buf = np.concatenate((self.hist, np.asarray(x, dtype=np.complex128)))
        self.hist = buf[len(buf) - self.hist_len :].copy()  # a copy: a view would keep the whole push alive
        y = sps.upfirdn(self.h, buf, self.up, self.down)
        start = self.hist_len * self.up // self.down
        return y[start : start + len(x) * self.up // self.down]

    def _push_decimating(self, x: np.ndarray) -> np.ndarray:
        self.buf = np.concatenate((self.buf, x))
        self.n_in += len(x)
        span = len(self.h) - 1
        last = self.n_in - 1 - self.advance  # the last input an output can end on
        end = last // self.down + 1 if last >= 0 else 0
        if end <= self.n_out:
            return np.zeros(0, dtype=np.complex128)
        lo = self.n_out * self.down + self.advance - span
        seg = self.buf[lo - self.base : (end - 1) * self.down + self.advance - self.base + 1]
        if span == 0:
            y = seg * self.h[0]
        else:
            y = sps.oaconvolve(seg, self.h, mode="valid")
        self.n_out = end
        keep = min(end * self.down + self.advance - span, self.n_in)
        self.buf = self.buf[keep - self.base :].copy()  # the history alone, not a view of the whole push
        self.base = keep
        return y[:: self.down]


def stage_taps(passes: int, hb_taps: dict) -> list[np.ndarray]:
    """The half-band cascade as full_demod_apply_halfband_decimation() runs it: hb31 first, hb15 after."""
    return [hb_taps["hb31"] if i == 0 else hb_taps["hb15"] for i in range(passes)]


def halfband_advance(h: np.ndarray) -> int:
    """simd_hb_decim2_complex()'s look-ahead: its output q is centred on input 2q (counted from a reset), the causal sum
    at 2q + (len - 1) / 2, made once that input is in."""
    return (len(h) - 1) // 2


def chain_look_ahead(plan: RatePlan, hb_taps: dict) -> int:
    """Channel-rate outputs the half-bands' look-ahead holds back at most: a stage of advance c makes at least
    (N - c - 1) / 2 outputs for N inputs, so the shortfall halves at each later stage."""
    short = 0.0
    for h in stage_taps(plan.passes, hb_taps):
        short = short / 2.0 + (halfband_advance(h) + 1) / 2.0
    return math.ceil(short) + 1


class ChainStream:
    """A chain's stages ahead of the channel filter, streaming: the reader (when the row has one), then the half-band
    passes, each centred as the decoder centres it (halfband_advance()), so a reader's phases reach the channel filter
    as the decoder's decimators pick them. A push takes a whole number of `unit` source samples and returns the outputs
    the cascade completes at the complex rate."""

    def __init__(self, plan: RatePlan, hb_taps: dict):
        self.stages = []
        L = M = 1
        if plan.reader:
            L, M = plan.reader.L, plan.reader.M
            self.stages.append(FirStream(reader_prototype(plan.reader), L, M))
        for h in stage_taps(plan.passes, hb_taps):
            self.stages.append(FirStream(h, 1, 2, halfband_advance(h)))
        self.unit = M << plan.passes
        self.ratio = (L, M << plan.passes)  # outputs per source sample, as a fraction

    def push(self, x: np.ndarray) -> np.ndarray:
        y = np.asarray(x, dtype=np.complex128)
        for stage in self.stages:
            y = stage.push(y)
        return y


def chain_lead(plan: RatePlan, taps: np.ndarray) -> int:
    """Channel outputs a stream drops at its start while the chain's stages fill."""
    reader_span = 0
    if plan.reader:
        reader_span = -(-plan.reader.taps_per_phase * plan.reader.L // (plan.reader.M << plan.passes))
    return reader_span + 64 + len(taps)


def source_count(plan: RatePlan, outputs: int) -> int:
    """Source samples (a whole number of the chain's units) that make at least @p outputs channel outputs."""
    L, den = (plan.reader.L if plan.reader else 1), ((plan.reader.M if plan.reader else 1) << plan.passes)
    need = -(-outputs * den // L)
    unit = den
    return -(-need // unit) * unit


def through_chain(plan: RatePlan, hb_taps: dict, taps: np.ndarray, x: np.ndarray) -> np.ndarray:
    """A source-rate signal through the chain and the channel filter, from silence."""
    chain = ChainStream(plan, hb_taps)
    pad = (-len(x)) % chain.unit
    if pad:
        x = np.concatenate((x, np.zeros(pad, dtype=x.dtype)))
    return FirStream(taps).push(chain.push(x))


def chain_signal(plan: RatePlan, hb_taps: dict, taps: np.ndarray, make, outputs: int) -> np.ndarray:
    """@p outputs channel outputs of the source-rate signal make(count) through the chain, past its lead."""
    lead = chain_lead(plan, taps)
    count = source_count(plan, outputs + lead + chain_look_ahead(plan, hb_taps))
    y = through_chain(plan, hb_taps, taps, make(count))
    if len(y) < lead + outputs:
        raise ValueError("the chain made fewer outputs than its source count promised")
    return y[lead : lead + outputs]


def chain_noise(plan: RatePlan, hb_taps: dict, taps: np.ndarray, rng: np.random.Generator, outputs: int) -> np.ndarray:
    """@p outputs channel outputs of unit-variance white complex noise at the source rate through the chain."""
    return chain_signal(plan, hb_taps, taps, lambda n: white_noise(rng, n), outputs)


class SubWindowStream:
    """The tracker's 20 ms sub-windows over a stream (src/dsp/squelch_floor.c): sub-window k ends at sample
    floor((k + 1) fs / 50) from the stream's start, whatever the pushes; the first L samples of the stream carry no
    coherence term (no sample L before them), and every sample counts in n. A push returns the sub-windows it
    completed: k, n, sa = sum |z|^2, sa2 = sum |z|^4, su = sum u_k conj(u_{k-L}) (u = z / |z|) and sz = sum z_k
    conj(z_{k-L})."""

    FIELDS = ("n", "sa", "sa2", "su", "sz")

    def __init__(self, rate: int, lag: int):
        self.rate, self.lag = rate, lag
        self.count = 0
        self.k = 0
        self.hist = np.zeros(lag, dtype=np.complex128)
        self.carry = {"n": 0.0, "sa": 0.0, "sa2": 0.0, "su": 0j, "sz": 0j}

    def push(self, z: np.ndarray) -> dict:
        z = np.asarray(z, dtype=np.complex128)
        n = len(z)
        start, end = self.count, self.count + n
        a = z.real * z.real + z.imag * z.imag
        ext = np.concatenate((self.hist, z))
        prev = ext[:n]
        prod = z * np.conj(prev)
        mag = np.sqrt(a * (prev.real * prev.real + prev.imag * prev.imag))
        live = np.ones(n, dtype=bool)
        if start < self.lag:
            live[: min(n, self.lag - start)] = False
        ok = live & (mag > 0.0)
        pu = np.zeros(n, dtype=np.complex128)
        pu[ok] = prod[ok] / mag[ok]
        pz = np.where(live, prod, 0.0)
        ks = np.arange(self.k, (end * WINDOW_PER_S) // self.rate + 2)
        ends = ((ks + 1) * self.rate) // WINDOW_PER_S
        done = int(np.count_nonzero(ends <= end))
        cuts = ends[:done] - start
        values = {"n": np.ones(n), "sa": a, "sa2": a * a, "su": pu, "sz": pz}
        out = {"k": ks[:done]}
        for name in self.FIELDS:
            cs = np.concatenate(([0.0], np.cumsum(values[name])))
            at = cs[cuts]
            sums = at - np.concatenate(([0.0], at[:-1]))
            if done:
                sums[0] += self.carry[name]
                self.carry[name] = cs[-1] - at[-1]
            else:
                self.carry[name] += cs[-1]
            out[name] = sums
        self.k += done
        self.count = end
        self.hist = ext[len(ext) - self.lag :].copy()  # the last L samples, not a view of the whole push
        return out


UNDECIDED, NOISE, CARRIER = 0, 1, 2
# The shipped classifier (src/dsp/squelch_floor.c), frozen.
FROZEN_CARRIER_X = -7.0
FROZEN_CARRIER_Y = 3.3
FROZEN_NOISE_X = -3.8
FROZEN_NOISE_Y = 2.1
UNSTEADY_RATIO = 2.0  # a NOISE window whose 20 ms halves differ by 3 dB is UNDECIDED
MIN_POWER = 1e-30


def classify_windows(first: dict, second: dict, plan: dict) -> np.ndarray:
    """dsd_squelch_floor_classify() and sqf_window_end()'s unsteady-halves rule on 40 ms windows (sub-window pairs)."""
    n = first["n"] + second["n"]
    sa = first["sa"] + second["sa"]
    with np.errstate(divide="ignore", invalid="ignore"):
        p = sa / n
        live = (n >= 2.0) & (p > MIN_POWER)
        cv2 = ((first["sa2"] + second["sa2"]) / n) / (p * p) - 1.0
        x = (cv2 - 1.0) * plan["sqrt_neff"]
        mean_u = (first["su"] + second["su"]) / n
        y = np.hypot(mean_u.real - plan["beta"], mean_u.imag) * plan["sqrt_neff"]
        p0 = first["sa"] / first["n"]
        p1 = second["sa"] / second["n"]
    carrier = live & ((x <= FROZEN_CARRIER_X) | (y >= FROZEN_CARRIER_Y))
    noise = live & ~carrier & (x >= FROZEN_NOISE_X) & (y <= FROZEN_NOISE_Y)
    unsteady = (p1 >= p0 * UNSTEADY_RATIO) | (p0 >= p1 * UNSTEADY_RATIO)
    cls = np.full(len(n), UNDECIDED, dtype=np.int8)
    cls[carrier] = CARRIER
    cls[noise & ~unsteady] = NOISE
    return cls


class FrozenStream:
    """The shipped 40 ms decisions over a stream: its sub-windows paired (0, 1), (2, 3), ... from the stream's start,
    the pair split across pushes kept for the next."""

    def __init__(self, rate: int, plan: dict):
        self.subs = SubWindowStream(rate, plan["lag"])
        self.plan = plan
        self.pending = None

    def push(self, z: np.ndarray) -> np.ndarray:
        s = self.subs.push(z)
        if self.pending is not None:
            s = {k: np.concatenate((self.pending[k], v)) for k, v in s.items()}
            self.pending = None
        count = len(s["n"]) // 2 * 2
        if len(s["n"]) > count:
            self.pending = {k: v[count:].copy() for k, v in s.items()}  # copies, as every kept history
        first = {k: v[0:count:2] for k, v in s.items()}
        second = {k: v[1:count:2] for k, v in s.items()}
        return classify_windows(first, second, self.plan)


def frozen_classes(z: np.ndarray, rate: int, plan: dict) -> np.ndarray:
    """The shipped decisions on one whole stream."""
    return FrozenStream(rate, plan).push(z)


def stream_sums(z: np.ndarray, rate: int, lag: int) -> dict:
    """One whole stream's 20 ms sub-window sums."""
    return SubWindowStream(rate, lag).push(z)


def merge_windows(sums: dict, multiple: int) -> dict:
    """Sums over consecutive groups of `multiple` 20 ms windows (a sample-exact 20 x multiple ms window)."""
    if multiple == 1:
        return sums
    count = len(sums["n"]) // multiple * multiple
    return {k: v[:count].reshape(-1, multiple).sum(axis=1) for k, v in sums.items() if k != "k"}


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


# --------------------------------------------------------------------------------------------- the noise model


def two_sided(r: np.ndarray) -> np.ndarray:
    """Lags -(n-1)..(n-1) of a covariance given at lags 0..n-1."""
    return np.concatenate((r[:0:-1], r))


def cascade_cov(passes: int, hb_taps: dict) -> np.ndarray:
    """The covariance (lags 0, 1, ...) of unit white noise at the capture rate behind the half-band cascade: each stage
    takes R'[k] = sum_j g[j] R[2k - j], g its taps' autocorrelation (ssm_halfband_recursion()'s recursion)."""
    r = np.ones(1)
    for h in stage_taps(passes, hb_taps):
        c = np.convolve(two_sided(r), autocorr(h))
        r = c[(len(r) - 1) + (len(h) - 1) :: 2]
    return r


def through_taps_cov(r: np.ndarray, taps: np.ndarray) -> np.ndarray:
    """A stationary covariance behind the channel taps: R_out(m) = sum_j R(j) R_taps(m - j), lags 0, 1, ..."""
    c = np.convolve(two_sided(r), autocorr(taps))
    return c[(len(r) - 1) + (len(taps) - 1) :]


IMPULSE_CHUNK = 1 << 20  # source samples an impulse_cov() push takes: the workspace's bound


def impulse_spacing(n_in: int, per_in: float, spacing_out: int) -> int:
    """The source-sample spacing of impulse_cov()'s impulses: the first that keeps consecutive responses
    @p spacing_out outputs apart and is coprime to the @p n_in source phases, so impulses j x spacing,
    j = 0 .. n_in - 1, land on every phase once."""
    gap = max(1, math.ceil(spacing_out / per_in))
    while math.gcd(gap, n_in) != 1:
        gap += 1
    return gap


def add_phase_products(cov: np.ndarray, y: np.ndarray, first: int, count: int) -> None:
    """R(q, m) += y_n y_{n+m} for the @p count outputs n from the start of @p y, which holds outputs from absolute
    index @p first on (q = n mod the period) and at least len(cov[0]) - 1 more past them."""
    period, lags = cov.shape
    offset = first % period
    rows = -(-(offset + count) // period)
    padded = np.zeros(rows * period)
    for m in range(lags):
        padded[offset : offset + count] = y[:count] * y[m : m + count]
        cov[:, m] += padded.reshape(rows, period).sum(axis=0)


def impulse_cov(plan: RatePlan, hb_taps: dict, taps: np.ndarray) -> np.ndarray:
    """Every output phase's covariance at the channel output of a reader chain, for unit white noise at the source
    rate: unit impulses at each of the N source phases the chain repeats over, far enough apart that no two responses
    meet (impulse_spacing()), through the ported chain; R(q, m) is the sum of y_n y_{n+m} over outputs n = q mod P,
    n counted from the stream's start (a shift of N inputs shifts the responses by P outputs). The impulses stream in
    pushes of at most IMPULSE_CHUNK source samples (the last one only as long as the stream, a whole number of the
    chain's units), the products accumulating as they come with the last lags - 1 outputs carried, so the workspace
    stays bounded whatever N and a short chain streams only what it needs. Rows are phases, columns lags."""
    L = plan.reader.L if plan.reader else 1
    M = plan.reader.M if plan.reader else 1
    period_in = 1 << plan.passes
    g = math.gcd(L, period_in)
    n_in = M * period_in // g
    n_out = L // g
    lags = chain_lead(plan, taps)
    per_in = L / (M * period_in)
    gap = impulse_spacing(n_in, per_in, 2 * lags + 16)
    # The first impulse a whole number of input periods in, past the stages' start (a response there would lose the
    # outputs before the stream's first); past the last, its response and the look-ahead the stages hold back.
    lead = -(-source_count(plan, lags) // n_in) * n_in
    total = lead + (n_in - 1) * gap + 1 + source_count(plan, 2 * lags + 16 + chain_look_ahead(plan, hb_taps))
    chain = ChainStream(plan, hb_taps)
    fir = FirStream(taps)
    chunk = max(chain.unit, IMPULSE_CHUNK // chain.unit * chain.unit)
    total = -(-total // chain.unit) * chain.unit
    cov = np.zeros((n_out, lags))
    carry = np.zeros(0)
    first = 0  # the absolute output index of carry[0]
    for pos in range(0, total, chunk):
        push = min(chunk, total - pos)
        x = np.zeros(push, dtype=np.complex128)
        j = np.arange(max(0, -(-(pos - lead) // gap)), min(n_in, -(-(pos + push - lead) // gap)))
        x[lead + j * gap - pos] = 1.0
        buf = np.concatenate((carry, fir.push(chain.push(x)).real))
        count = len(buf) - (lags - 1)
        if count > 0:
            add_phase_products(cov, buf, first, count)
            carry = buf[count:].copy()
            first += count
        else:
            carry = buf
    if len(carry):
        add_phase_products(cov, np.concatenate((carry, np.zeros(lags - 1))), first, len(carry))
    return cov


def chain_cov(plan: RatePlan, hb_taps: dict, taps: np.ndarray) -> np.ndarray:
    """The channel output's noise covariance for unit white noise at the source rate (rows: output phases)."""
    if plan.reader:
        return impulse_cov(plan, hb_taps, taps)
    return through_taps_cov(cascade_cov(plan.passes, hb_taps), taps)[None, :]


def plan_lag(taps: np.ndarray) -> int:
    """sqf_plan_lag(): the smallest lag >= 1 with |R_h(m)| / R_h(0) under 0.1 (the taps' length when none; 1 for a
    single tap)."""
    if len(taps) == 1:
        return 1
    return lag_rule(taps)[0]


def periodic_phase_means(cov: np.ndarray, lag: int, window: int) -> tuple[float, float, float]:
    """sqf_periodic_phase_means(): N_eff, beta and rho over every window start phase for windows of @p window samples
    (the samples' statistics repeat with the covariance's period)."""
    period, lags = cov.shape

    def at(n: int, m: int) -> float:
        if m < 0:
            n, m = n + m, -m
        return float(cov[n % period, m]) if m < lags else 0.0

    power = np.zeros(period)
    sq = np.zeros(period)
    beta = np.zeros(period)
    rho = np.zeros(period)
    for q in range(period):
        r0 = at(q, 0)
        s = r0 * r0
        for m in range(1, lags):
            s += at(q, m) ** 2 + at(q, -m) ** 2
        r_prev = at(q - lag, 0)
        rho_q = at(q, -lag) / math.sqrt(r0 * r_prev) if r0 > 0.0 and r_prev > 0.0 else 0.0
        power[q], sq[q], rho[q] = r0, s, rho_q
        beta[q] = (math.pi / 4.0) * rho_q * float(special.hyp2f1(0.5, 0.5, 2.0, rho_q * rho_q))
    neff = b = rh = 0.0
    for start in range(period):
        counts = np.bincount((start + np.arange(window)) % period, minlength=period).astype(np.float64)
        w_power, w_sq = float(counts @ power), float(counts @ sq)
        neff += w_power * w_power / w_sq
        b += float(counts @ beta) / window
        rh += float(counts @ rho) / window
    return neff / period, b / period, rh / period


MEAN_BOUND = 1e-2  # dsd_squelch_cascade_mean_suffices()'s bound (src/dsp/squelch_cascade_noise.c)


def mean_bound(cyclo_dev: float, taps: np.ndarray, power: float) -> float:
    """dsd_squelch_cascade_mean_suffices()'s bound on how far an output phase's covariance can stray from the phase
    mean after the channel taps: the phases' largest distance ahead of the taps (@p cyclo_dev) times (sum |c|)^2, over
    the output power. The C reader and step models design at a period of one while it is at most MEAN_BOUND."""
    return cyclo_dev * float(np.sum(np.abs(taps))) ** 2 / power


def plan_from_cov(cov: np.ndarray, taps: np.ndarray, rate: int, gain_scale: float, cyclo_dev: float = 0.0) -> dict:
    """The floor plan for a channel covariance (dsd_squelch_floor_plan_design_periodic_mean()), choosing the design as
    the C reader model does: the phase mean at a period of one while mean_bound() on @p cyclo_dev (the phases' largest
    distance ahead of the channel taps; 0 for a stationary chain) stays at most MEAN_BOUND, every output phase in full
    past it. N_eff, beta and rho are means over the tracker's windows, floor(fs / 25) samples or one more as often as
    the fractional mean window rate / 25 needs. The noise gain is the mean power times @p gain_scale."""
    mean = cov.mean(axis=0)
    dev = float(np.max(np.abs(cov - mean[None, :]))) / float(mean[0])
    bound = mean_bound(cyclo_dev, taps, float(mean[0]))
    use = mean[None, :] if bound <= MEAN_BOUND else cov
    lag = plan_lag(taps)
    window = rate // 25
    longer = (rate % 25) / 25.0
    neff, beta, rho = periodic_phase_means(use, lag, window)
    if longer > 0.0:
        neff2, beta2, rho2 = periodic_phase_means(use, lag, window + 1)
        neff = (1.0 - longer) * neff + longer * neff2
        beta = (1.0 - longer) * beta + longer * beta2
        rho = (1.0 - longer) * rho + longer * rho2
    return {
        "valid": 1,
        "rate_hz": rate,
        "lag": lag,
        "rho_lag": rho,
        "beta": beta,
        "neff": neff,
        "sqrt_neff": math.sqrt(neff),
        "noise_gain": float(np.mean(use[:, 0])) * gain_scale,
        "period": int(use.shape[0]),
        "phase_dev": dev,
        "mean_bound": bound,
    }


def gain_scale(plan: RatePlan) -> float:
    """The noise_gain convention: the power for white noise of variance 2^(passes - 1) at the capture rate."""
    return float(1 << (plan.passes - 1)) if plan.passes > 0 else 1.0


def plan_distance(a: dict, b: dict) -> float:
    """How far two floor plans are apart: N_eff and the noise gain relative, beta and rho absolute; infinite for
    another lag."""
    if a["lag"] != b["lag"] or not a["valid"] or not b["valid"]:
        return math.inf
    rel = max(abs(a["neff"] / b["neff"] - 1.0), abs(a["noise_gain"] / b["noise_gain"] - 1.0))
    return max(rel, abs(a["beta"] - b["beta"]), abs(a["rho_lag"] - b["rho_lag"]))


def filter_model(plan: RatePlan, taps: np.ndarray, hb_taps: dict, cache: dict) -> tuple[np.ndarray, dict]:
    """The channel noise covariance and the plan the tool designs from it for one chain and its taps, kept in @p cache
    (by chain and taps). A reader row's design choice takes the phases' distance ahead of the taps, as the C model."""
    key = f"{plan.chain_key}:{taps_key(taps)}"
    if key not in cache:
        cyclo_dev = 0.0
        if plan.reader:
            ahead_key = ("ahead", plan.chain_key)
            if ahead_key not in cache:
                ahead = impulse_cov(plan, hb_taps, np.ones(1))
                cache[ahead_key] = float(np.max(np.abs(ahead - ahead.mean(axis=0)[None, :])))
            cyclo_dev = cache[ahead_key]
        cov = chain_cov(plan, hb_taps, taps)
        cache[key] = (cov, plan_from_cov(cov, taps, plan.complex_rate, gain_scale(plan), cyclo_dev))
    return cache[key]


def taps_key(taps: np.ndarray) -> str:
    return f"{len(taps)}:{zlib.crc32(taps.astype(np.float64).tobytes()):08x}"
