#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure dsd-neo's own reception threshold per mode, for calibrating the scan skip-silent check (issue #834).

The skip check may call a channel quiet only well below the weakest signal dsd-neo still decodes. This tool finds that
weakest signal: for each mode it renders a 10 s signal, adds complex white Gaussian noise at a set S/N, replays the
noisy I/Q through the real decoder (--iq-replay, the path the DECODE_IQ_* tests use) and finds the lowest S/N, on a
0.5 dB grid, at which the decoder still meets the mode's criterion. A row's threshold is the median over the noise
seeds (five by default).

S/N is the signal power over the noise power inside the mode's class width (12.5 kHz or 6.25 kHz for digital modes,
the configured channel width for analog), both measured at the replayed 48 kHz baseband. For a burst train (DMR MS)
the signal power is the power during the bursts. The noise is fixed (its power and the cu8 scale never change); the
signal is scaled.

Digital criterion: at least 90 % of the frames the clean run (the same signal, no added noise) passes still pass
CRC/FEC. Each row counts one kind of frame:

  p25p1   LDU1 link control and LDU2 encryption sync decoded to the transmitted values (RS), lines dsd-neo prints
  p25p2   SACCH MAC PDUs (RS + CRC), with the scrambling parameters hand-set (-X), so the threshold is the
          channel's and not that of recovering the scrambling code from one voice burst
  dmr_bs  bursts whose slot type or EMB decodes to the transmitted colour code (Golay / QR)
  dmr_ms, dstar   voice frames whose 24 Golay-protected AMBE parameter bits (C0 and C1, ambe_d[0..23]) equal the
          generated frame's, read from dsd-neo's --frame-log. dsd-neo's MS path prints one line per superframe and
          its D-STAR path no per-frame verdict at all, so the frame's FEC-protected content is the verdict
  nxdn96, nxdn48   frames whose SACCH passes CRC (dsd-neo prints the part number "PF n/4", "PF X/4" otherwise)
  dpmr    half-frames whose CCH verdict holds ("FN n/4" under -Z)
  ysf     frames whose FICH passes Golay and CRC
  m17     stream frames whose LICH passes Golay ("LC: n/6")

Rows come in two roles. "clean signal" rows are ideal signals at the standard's pulse shape, symbol rate and
deviation; they set the thresholds. "off-air check" rows replay a committed off-air fixture as recorded (deviation
rescaled where it was remodulated) and only check the clean rows' order of magnitude. An off-air row whose clean run
passes fewer than 90 % of the frames dsd-neo received is reported as never reaching the criterion. A clean row whose
signal was regenerated from an off-air recording's own symbols keeps the errors baked into those symbols; it is swept
against its clean run's pass count either way, and its clean fraction is reported.

Where a parameter stays uncertain after research the tool measures every candidate (one row each) and keeps the
lower threshold for the mode, the safe direction for the skip calibration: YSF's deviation, P25 Phase 2's transmit
filter and D-STAR's Gaussian BT.

Clean-signal sources (sources per parameter in the issue #834 Task 4 report):

  p25p1, p25p2, nxdn96, nxdn48, ysf, m17   the symbols dsd-neo reads from the committed off-air fixture (its -c
           symbol capture, recovered by symbol_levels()), cut to the carrier and repeated to 10 s, re-synthesized:
             p25p1   C4FM: raised cosine (alpha 0.2) times inverse sinc, outer deviation 1800 Hz (TIA-102.BAAA)
             nxdn96, nxdn48   root raised cosine (alpha 0.2) times sinc, outer 2400 / 1050 Hz (NXDN TS 1-A 3.3-3.4)
             m17     root raised cosine (alpha 0.5), outer 2400 Hz (M17 specification 1.2-1.3)
             ysf     root raised cosine (alpha 0.2), outer 1800, 2290 or 2700 Hz (no public specification: C4FM's
                     1800 Hz; OP25 dv_tx.py's 0.42/0.33 of it; MMDVM's YSF_LEVELA_HI 1893 against DMR_LEVELA 1362
                     for 1944 Hz through the same filter)
             p25p2   H-DQPSK, the base station's outbound (+/-pi/4, +/-3pi/4 per symbol at 6000 symbol/s), through
                     the "harmonized" raised cosine (alpha 1, 3600 Hz) or a root raised cosine (alpha 0.2)
           Each FM transmit filter is applied to symbol impulses at unity DC gain, so a held level 3 is the outer
           deviation.
  dmr_bs   the DMR test generator (tests/protocol/dmr/dmr_synth.c, --dmr-synth-gen): slot 1 a clear voice call,
           slot 2 idle, remodulated with build_iq_fixtures.remodulate() at the ETSI outer deviation of 1944 Hz
  dmr_ms   the same generator with --ms (MS syncs, no CACH, slot 1's bursts alone, 27.5 ms on every 60 ms); the
           carrier is keyed off between the bursts
  dpmr     build_iq_fixtures.modulate_4fsk() over the dPMR CCH reference vectors (dpmr_symbol_stream())
  dstar    this tool's D-STAR DV transmission (dstar_*() below): bit sync, frame sync, the radio header (CRC-CCITT,
           rate-1/2 K=3 convolutional code, 24-bit interleave, x^7+x^4+1 scrambler), then voice frames of 72 AMBE
           bits and 24 data bits with the sync pattern every 21st frame, GMSK at 4800 bit/s, h = 0.5, BT 0.5 or
           0.35. The AMBE frames are random parameter bits (no special pitch codes) FEC-coded and interleaved by
           mbelib-neo's mbe_encodeAmbe3600x2400Frame() and mbe_encodeDStarDVData() (--mbelib)
  nfm, am  build_iq_fixtures.nfm_synth_samples() / am_synth_samples() at this tool's deviation, depth and length

Analog criterion: 12 dB SINAD of a 1 kHz tone in dsd-neo's monitor audio (TIA-603 for NFM, ETSI EN 300 676-1 for
AM). NFM carries the tone at 60 % of the maximum deviation (1.5 kHz on a 12.5 kHz channel, 3 kHz on a 25 kHz one),
AM at 30 % depth. The audio is scored by dsd-neo_test_analog_replay (tests/engine/analog_replay.c), which runs the
real engine and fits the tone to every delivered 20 ms block after the monitor's filters: SINAD is
10 log10(1 + S/(N+D)) from its tone_snr_db. Gain is fixed (-n 50) and the replay squelch is the -110 dB default,
which passes everything; noise alone must fail the metric, and the tool reports that it does.

Everything is deterministic: the noise seeds are fixed (--base-seed), the generators run with fixed arguments and
seeds, and dsd-neo replays at --iq-replay-rate fast.

Usage:
    python3 tools/scan_quiet_reception.py --dsd build/dev-release/apps/dsd-cli/dsd-neo \\
        --analog-host build/dev-debug/tests/dsd-neo_test_analog_replay \\
        --dmr-synth-gen build/dev-debug/tests/dsd-neo_test_dmr_synth_gen --out OUT_DIR [--modes LIST] [--jobs N]

It writes OUT_DIR/thresholds.json, OUT_DIR/thresholds.md (each row, then each mode's kept threshold) and
OUT_DIR/sweep.csv (every run). Exit status 0 when every requested row was measured or reported, 1 when a decoder or
generator run fails or a capture would clip, 2 on a usage error.
"""

import argparse
import collections
import concurrent.futures
import contextlib
import csv
import ctypes
import ctypes.util
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import wave
import zlib
from dataclasses import dataclass, field

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_iq_fixtures as bif

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE_DIR = os.path.join(ROOT, "tests", "fixtures", "iq")
RATE_HZ = bif.SAMPLE_RATE_HZ
DURATION_S = 10.0
NOISE_SIGMA = 0.015  # per I/Q component at full scale 1.0: far above the cu8 step, far below clipping
PASS_FRACTION = 0.9
SINAD_TARGET_DB = 12.0
STEP_DB = 0.5
COARSE_DB = 2.0
START_DB = 30.0
FLOOR_DB = -10.0
ANSI = re.compile(r"\x1b\[[0-9;]*m")
CLEAN = "clean signal"
OFF_AIR = "off-air check"

# Nominal outer symbol deviation (Hz) of the off-air fixtures remodulated from discriminator audio, for the off-air
# check rows. Their present outer level is estimated by outer_deviation_hz(), which reads 1805 Hz on the off-air C4FM
# I/Q (nominal 1800 Hz) and 1054 Hz on the dPMR synthesis (1050 Hz).
OFF_AIR_DEVIATION = {"p25p2_cc": 2250.0, "ysf": 2290.0, "m17": 2400.0, "dmr_voice": 1944.0}
OUTER_WINDOW = 4  # samples averaged (under half a 4800 symbol/s symbol at 48 kHz)
CARRIER_ON_FRACTION = 0.1  # a 10 ms window is carrier when its power exceeds this share of the clip's 90th percentile

DMR_OUTER_HZ = 1944.0  # ETSI TS 102 361-1 section 10.2.2.1
DMR_SYNTH_OUTER = bif.DMR_SYNTH_OUTER_LEVEL
DMR_GEN_ARGS = ["--cc", "2", "--s1", "voice", "--s2", "idle", "--snr", "200", "--seed", "7"]
DMR_SLOT_SYMBOLS = 144  # 12 CACH (or MS guard) + 132 burst dibits: 30 ms at 4800 symbol/s
DMR_LEAD_SYMBOLS = 960  # dmr_synth.c: the random (with --ms, silent) lead before the first period
NFM_TONE_DEVIATION_HZ = {12500: 1500.0, 25000: 3000.0}  # 60 % of +/-2.5 kHz and +/-5 kHz
AM_TONE_DEPTH = 0.3
SHAPER_SPAN_SYMBOLS = 8  # transmit filter taps either side of the centre

# D-STAR DV (JARL D-STAR standard; MMDVM DStarTX.cpp; OP25 dv_tx.py)
DSTAR_BIT_RATE = 4800
DSTAR_DEVIATION_HZ = 1200.0  # GMSK: h = 0.5 at 4800 bit/s
DSTAR_PREAMBLE_BITS = 64  # "1010" bit sync
DSTAR_FRAME_SYNC = "111011001010000"
DSTAR_DATA_SYNC = "101010101011010001101000"
DSTAR_END = "10101010101010101010101010101010" + "000100110101111" + "0"
DSTAR_SLOW_DATA_SCRAMBLE = (0x70, 0x4F, 0x93)
DSTAR_SLOW_DATA_FILLER = 0x66
DSTAR_SUPERFRAME = 21
DSTAR_SEED = 834


@dataclass
class Source:
    samples: np.ndarray
    note: str = ""
    on: np.ndarray = None  # sample mask of a burst train's bursts; None for a continuous signal
    expected: list = None  # generated AMBE frames (14 hex digits, 49 bits << 7) for an AMBE verdict


@dataclass
class Row:
    key: str
    mode: str  # rows of one mode keep the lowest clean-signal threshold
    label: str
    width_hz: int
    role: str
    args: list
    build: object  # callable(work_dir) -> Source
    pass_re: str = ""  # line verdict; empty for the AMBE verdict
    unit_re: str = ""
    unit_weight: int = 1
    analog: bool = False


@dataclass
class Result:
    key: str
    mode: str
    label: str
    width_hz: int
    role: str
    metric: str
    source: str = ""
    status: str = "measured"
    seeds: list = field(default_factory=list)
    threshold_db: float = None
    clean: dict = field(default_factory=dict)
    detail: str = ""


def sample_count():
    return round(DURATION_S * RATE_HZ)


def repeat_to_length(samples, count=None):
    n = sample_count() if count is None else count
    return np.tile(samples, n // len(samples) + 1)[:n]


def run_text(cmd, cwd=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=300, check=False, cwd=cwd)
    if proc.returncode != 0:
        raise SystemExit(f"{' '.join(cmd)} exited {proc.returncode}:\n{proc.stdout[-2000:]}{proc.stderr[-2000:]}")
    return ANSI.sub("", proc.stdout + proc.stderr)


def unique_path(work_dir, stem):
    """A fresh path in work_dir: rows and seeds run in parallel threads."""
    return tempfile.mkstemp(prefix=stem + "-", dir=work_dir)[1]


# ---------------------------------------------------------------- off-air fixtures


def outer_deviation_hz(samples):
    """Outer symbol level of a 4-level FM signal: the 75th percentile of |instantaneous frequency|, averaged over
    OUTER_WINDOW samples; with equiprobable symbols half sit on the outer levels."""
    freq = np.angle(samples[1:] * np.conj(samples[:-1])) * RATE_HZ / (2.0 * math.pi)
    smooth = np.convolve(freq, np.ones(OUTER_WINDOW) / OUTER_WINDOW, mode="valid")
    return float(np.percentile(np.abs(smooth - np.median(smooth)), 75))


def carrier_bounds(samples):
    """First and last sample of the carrier: the off-air clips start (and some end) before the transmitter keys up,
    and that silence is neither signal power nor frames."""
    window = RATE_HZ // 100
    power = np.convolve(np.abs(samples) ** 2, np.ones(window) / window, mode="same")
    on = np.flatnonzero(power > CARRIER_ON_FRACTION * np.percentile(power, 90))
    return int(on[0]), int(on[-1]) + 1


def load_fixture(name):
    samples = bif.load_cu8_fixture(os.path.join(FIXTURE_DIR, name + ".iq"))
    return samples - np.mean(samples)


def off_air_source(name):
    """A committed fixture's carrier at its mode's nominal deviation when remodulated, repeated to the length."""
    samples = load_fixture(name)
    first, last = carrier_bounds(samples)
    samples = samples[first:last]
    note = f"committed fixture {name} ({len(samples) / RATE_HZ:.2f} s carrier) as recorded, repeated"
    if name in OFF_AIR_DEVIATION:
        measured, nominal = outer_deviation_hz(samples), OFF_AIR_DEVIATION[name]
        samples = np.exp(1j * (nominal / measured) * np.unwrap(np.angle(samples)))
        note += f", deviation rescaled {measured:.0f} -> {nominal:.0f} Hz"
    return Source(repeat_to_length(samples), note=note)


# ---------------------------------------------------------------- regenerated from captured symbols


def captured_symbols(name, args, work_dir):
    """The symbols dsd-neo reads from a committed fixture, one per symbol period from the first sample (its -c symbol
    capture: a 16-byte DSDNSYM2 header, then 10-byte records holding the dibit and, in the last four bytes, the
    little-endian float the demodulator gave). Returns the dibits and the floats."""
    path = unique_path(work_dir, name + "-symbols")
    os.remove(path)
    sidecar = os.path.join(FIXTURE_DIR, name + ".iq.json")
    run_text([ARGS.dsd, "--frontend", "none"] + args + ["--iq-replay", sidecar, "-o", "null", "-c", path])
    with open(path, "rb") as handle:
        raw = handle.read()
    os.remove(path)
    if raw[:8] != b"DSDNSYM2" or (len(raw) - 16) % 10:
        raise SystemExit(f"{name}: unexpected symbol capture layout")
    records = np.frombuffer(raw[16:], dtype=np.uint8).reshape(-1, 10)
    return records[:, 0] & 3, records[:, 6:10].copy().view("<f4").ravel().astype(np.float64)


# Sync words with inner symbols, which dsd-neo's capture holds at outer levels (below).
#   NXDN FSW: -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 (CDF59, NXDN TS 1-A Table 4.4-2; sync_patterns.h NXDN_FSW)
#   YSF FS: D471C9634D (MMDVM YSFDefines.h YSF_SYNC_BYTES; sync_patterns.h FUSION_SYNC), dibits as every symbol
NXDN_FSW_LEVELS = (-3, 1, -3, 3, -3, -3, 3, 3, -1, 3)
YSF_FS_LEVELS = tuple(bif.DPMR_DIBIT_TO_LEVEL[(0xD471C9634D >> (38 - 2 * i)) & 3] for i in range(20))


INVERTED_SHARE = 0.25  # share of confident symbols whose dibit disagrees with the demodulator: an inverted recording


def symbol_levels(dibits, floats, sync_levels):
    """Each captured symbol's level (-3, -1, +1, +3) at the recording's own polarity.

    The magnitude is the dibit dsd-neo's adaptive slicer read (build_iq_fixtures.DPMR_DIBIT_TO_LEVEL, dsd-neo's
    convention: 1 and 3 outer, 0 and 2 inner), and so is the sign, unless the recording is inverted: dsd-neo undoes an
    inverted transmission's polarity inside the frames it decodes and nowhere else, so the dibits of such a recording
    mix both polarities, and the sign is then the demodulator float's about its median. A recording counts as inverted
    when more than INVERTED_SHARE of the symbols in the outer half of the float's spread disagree in sign with their
    dibits (the committed NXDN recordings; under 5 % for the others). dsd-neo also slices a sync it hunts for on its
    symbols' signs alone, so sync_levels (a sync word with inner symbols) goes back in wherever the signs spell it,
    at either polarity."""
    levels = np.array([bif.DPMR_DIBIT_TO_LEVEL[int(d)] for d in dibits], dtype=np.float64)
    offset = floats - np.median(floats)
    confident = np.abs(offset) > np.median(np.abs(offset))
    if np.mean((levels[confident] > 0) != (offset[confident] > 0)) > INVERTED_SHARE:
        levels = np.where(offset > 0, 1.0, -1.0) * np.abs(levels)
    if sync_levels is not None:
        text = "".join("1" if level > 0 else "3" for level in levels)
        for word in (np.asarray(sync_levels, dtype=np.float64), -np.asarray(sync_levels, dtype=np.float64)):
            for match in re.finditer("".join("1" if level > 0 else "3" for level in word), text):
                levels[match.start() : match.end()] = word
    return levels


def spectrum_taps(response, symbol_rate):
    """FIR taps (SHAPER_SPAN_SYMBOLS either side) of a zero-phase transfer function response(f) given in Hz on a
    1 Hz grid, summing to the samples per symbol: a held symbol level comes out at its own value."""
    freqs = np.arange(RATE_HZ // 2 + 1, dtype=np.float64)
    impulse = np.fft.fftshift(np.fft.irfft(response(freqs), RATE_HZ))
    sps = RATE_HZ // symbol_rate
    half = SHAPER_SPAN_SYMBOLS * sps
    centre = RATE_HZ // 2
    taps = impulse[centre - half : centre + half + 1]
    return taps * sps / taps.sum()


def raised_cosine(freqs, alpha, symbol_rate):
    low = (1.0 - alpha) * symbol_rate / 2.0
    high = (1.0 + alpha) * symbol_rate / 2.0
    edge = 0.5 * (1.0 + np.cos(math.pi * (freqs - low) / (alpha * symbol_rate)))
    return np.where(freqs <= low, 1.0, np.where(freqs <= high, edge, 0.0))


def sinc_response(freqs, symbol_rate):
    return np.sinc(freqs / symbol_rate)  # sin(pi f T) / (pi f T)


def c4fm_response(freqs):
    """TIA-102.BAAA C4FM: Nyquist raised cosine (alpha 0.2) times the inverse-sinc shaping filter, 0 above 2880 Hz."""
    return raised_cosine(freqs, 0.2, 4800) / sinc_response(np.minimum(freqs, 2880.0), 4800)


def nxdn_response(symbol_rate):
    """NXDN TS 1-A 3.4: square root raised cosine (alpha 0.2) times sinc."""
    return lambda freqs: np.sqrt(raised_cosine(freqs, 0.2, symbol_rate)) * sinc_response(freqs, symbol_rate)


def rrc_response(alpha, symbol_rate):
    return lambda freqs: np.sqrt(raised_cosine(freqs, alpha, symbol_rate))


def harmonized_response(freqs):
    """The H-DQPSK filter as the Viavi/Aeroflex P25 Phase 2 note gives it: a raised cosine of roll-off 1 whose
    bandwidth (the 1/2T point) is 3600 Hz, so its response falls to 0 at 7200 Hz."""
    return np.where(freqs <= 7200.0, np.cos(math.pi * freqs / 14400.0) ** 2, 0.0)


def symbol_span(name, symbol_rate):
    """The captured symbols inside the fixture's carrier."""
    first, last = carrier_bounds(load_fixture(name))
    sps = RATE_HZ / symbol_rate
    return math.ceil(first / sps), int(last // sps)


def fm_from_levels(levels, symbol_rate, response, outer_hz):
    sps = RATE_HZ // symbol_rate
    impulses = np.zeros(len(levels) * sps)
    impulses[::sps] = levels
    freq_hz = np.convolve(impulses, spectrum_taps(response, symbol_rate), mode="same") * outer_hz / 3.0
    return np.exp(2j * math.pi * np.cumsum(freq_hz) / RATE_HZ)


def hdqpsk_from_levels(levels, response):
    """pi/4-DQPSK at 6000 symbol/s: each level l turns the carrier by l * pi/4, through the transmit filter."""
    sps = RATE_HZ // 6000
    impulses = np.zeros(len(levels) * sps, dtype=np.complex128)
    impulses[::sps] = np.exp(1j * np.cumsum(levels) * math.pi / 4.0)
    return np.convolve(impulses, spectrum_taps(response, 6000), mode="same")


def regenerated_source(name, args, symbol_rate, synth, note, work_dir, sync_levels=None):
    first, last = symbol_span(name, symbol_rate)
    dibits, floats = captured_symbols(name, args, work_dir)
    levels = symbol_levels(dibits[first:last], floats[first:last], sync_levels)
    count = round(DURATION_S * symbol_rate)
    samples = synth(repeat_to_length(levels, count))[: sample_count()]
    return Source(samples, note=f"{name}: {last - first} captured symbols repeated, {note}")


# ---------------------------------------------------------------- DMR generator


def dmr_synth_audio(work_dir, extra):
    """The DMR test generator's discriminator audio and its expected AMBE frames (14 hex digits each)."""
    gen_dir = tempfile.mkdtemp(prefix="dmr-", dir=work_dir)
    args = ["-o", "dmr.wav", "-e", "dmr.txt", "-t", f"{DURATION_S:g}"] + DMR_GEN_ARGS + extra
    subprocess.run([os.path.abspath(ARGS.dmr_synth_gen)] + args, cwd=gen_dir, check=True, capture_output=True)
    with wave.open(os.path.join(gen_dir, "dmr.wav"), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getframerate() != RATE_HZ:
            raise SystemExit("expected 48 kHz mono s16 from the DMR generator")
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)
    with open(os.path.join(gen_dir, "dmr.txt"), encoding="ascii") as handle:
        expected = [line.split()[4] for line in handle if line.strip() and not line.startswith("#")]
    return audio, expected


def dmr_baseband(audio):
    # remodulate() maps the 99.5th percentile of |audio - mean| to its deviation; set that so the +/-3 level lands on
    # the standard's outer deviation (as build_iq_fixtures.dmr_synth_deviation() does).
    scale = np.percentile(np.abs(audio - np.mean(audio)), 99.5)
    return bif.remodulate(audio, DMR_OUTER_HZ * scale / DMR_SYNTH_OUTER / (RATE_HZ / 2.0))


def dmr_bs_source(work_dir):
    audio, _expected = dmr_synth_audio(work_dir, [])
    samples = repeat_to_length(dmr_baseband(audio))
    return Source(samples, note=f"dmr_synth.c BS, slot 1 voice, slot 2 idle, outer {DMR_OUTER_HZ:.0f} Hz")


def dmr_ms_source(work_dir):
    """dmr_synth.c --ms: a burst is dibits 12-143 of every even 144-dibit period after the 960-dibit lead; a period
    whose burst the generator left silent (the call over) stays keyed off."""
    audio, expected = dmr_synth_audio(work_dir, ["--ms", "--lead", "0", "--tail", "0"])
    n = sample_count()
    samples = dmr_baseband(audio)[:n]
    symbol = np.arange(n) // (RATE_HZ // 4800) - DMR_LEAD_SYMBOLS
    period = symbol // DMR_SLOT_SYMBOLS
    on = (symbol >= 0) & (period % 2 == 0) & (symbol % DMR_SLOT_SYMBOLS >= 12)
    level = np.abs(audio[:n])
    for p in np.unique(period[on]):
        span = on & (period == p)
        if np.mean(level[span]) < 0.1 * DMR_SYNTH_OUTER:
            on &= ~span
    note = "dmr_synth.c --ms, slot 1 voice bursts, carrier keyed off between them"
    return Source(np.where(on, samples, 0.0), note=note, on=on, expected=expected)


# ---------------------------------------------------------------- D-STAR generator


def dstar_lsb_bits(data):
    """Bytes as D-STAR sends them: each byte least significant bit first (JARL D-STAR standard Ap1.4)."""
    return [(byte >> bit) & 1 for byte in data for bit in range(8)]


def dstar_crc(data):
    """CRC-CCITT as D-STAR's P_FCS (x^16 + x^12 + x^5 + 1, reflected, initial 0xFFFF, inverted), low byte first."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    crc ^= 0xFFFF
    return bytes([crc & 0xFF, crc >> 8])


def dstar_header_bits():
    """The 660 transmitted bits of a radio header: CRC, rate-1/2 K=3 code (g1 = 1+D+D^2 sent first, g0 = 1+D^2),
    24-bit interleave, then the x^7+x^4+1 scrambler (JARL D-STAR standard 4.1.1, Ap1, Ap2)."""
    fields = b"\x00\x00\x00" + b"DIRECT  " + b"DIRECT  " + b"CQCQCQ  " + b"N0CALL  " + b"TEST"
    info = dstar_lsb_bits(fields + dstar_crc(fields)) + [0, 0]
    coded = []
    d1 = d2 = 0
    for d in info:
        coded += [d ^ d1 ^ d2, d ^ d2]
        d1, d2 = d, d1
    sent = []
    k = 0
    for _ in range(len(coded)):
        sent.append(coded[k])
        k += 24
        k = k - 671 if k >= 672 else k - 647 if k >= 660 else k
    reg = [1] * 7  # all ones (Ap1.1); the output is the feedback x^7 + x^4
    scrambled = []
    for bit in sent:
        out = reg[6] ^ reg[3]
        scrambled.append(bit ^ out)
        reg = [out] + reg[:6]
    return scrambled


def dstar_voice_frames(count, rng):
    """count random AMBE 2400 frames (pitch codes below the special values 120-127, spare bit 0): their 49 parameter
    bits and the 72 bits mbelib-neo codes and interleaves for each, in D-STAR DV byte order."""
    lib = ctypes.CDLL(ARGS.mbelib)
    frame_t = (ctypes.c_char * 24) * 4
    frames = []
    for _ in range(count):
        bits = rng.integers(0, 2, 49).astype(np.uint8)
        b0 = int(rng.integers(0, 120))
        bits[[0, 1, 2, 3, 4, 5, 48]] = [(b0 >> s) & 1 for s in (6, 5, 4, 3, 2, 1, 0)]
        bits[24] = 0
        ambe_d = (ctypes.c_char * 49)(*bits.tolist())
        fr = frame_t()
        out = (ctypes.c_ubyte * 9)()
        if lib.mbe_encodeAmbe3600x2400Frame(ambe_d, fr) != 0 or lib.mbe_encodeDStarDVData(fr, out) != 0:
            raise SystemExit("mbelib-neo refused to encode a D-STAR frame")
        value = int("".join(str(int(b)) for b in bits), 2) << 7
        frames.append((f"{value:014X}", dstar_lsb_bits(bytes(out))))
    return frames


def dstar_bits(rng):
    """One D-STAR DV transmission filling DURATION_S: preamble, frame sync, header, voice frames, end pattern."""
    head = [int(b) for b in "10" * (DSTAR_PREAMBLE_BITS // 2) + DSTAR_FRAME_SYNC] + dstar_header_bits()
    tail = [int(b) for b in DSTAR_END]
    count = (round(DURATION_S * DSTAR_BIT_RATE) - len(head) - len(tail)) // 96
    frames = dstar_voice_frames(count, rng)
    filler = bytes(DSTAR_SLOW_DATA_FILLER ^ s for s in DSTAR_SLOW_DATA_SCRAMBLE)
    bits = list(head)
    for index, (_hex, voice) in enumerate(frames):
        bits += voice
        bits += [int(b) for b in DSTAR_DATA_SYNC] if index % DSTAR_SUPERFRAME == 0 else dstar_lsb_bits(filler)
    return bits + tail, [frame[0] for frame in frames]


def gmsk(bits, bt):
    """GMSK at DSTAR_BIT_RATE: a bit 1 is positive deviation (JARL Ap1.5), a long run reaches +/-1200 Hz (h = 0.5)."""
    sps = RATE_HZ // DSTAR_BIT_RATE
    nrz = np.repeat(np.where(np.asarray(bits) > 0, 1.0, -1.0), sps)
    t = np.arange(-2 * sps, 2 * sps + 1) / sps
    sigma = math.sqrt(math.log(2.0)) / (2.0 * math.pi * bt)
    gauss = np.exp(-(t**2) / (2.0 * sigma**2))
    freq_hz = np.convolve(nrz, gauss / gauss.sum(), mode="same") * DSTAR_DEVIATION_HZ
    return np.exp(2j * math.pi * np.cumsum(freq_hz) / RATE_HZ)


def dstar_source(bt):
    bits, expected = dstar_bits(np.random.default_rng(DSTAR_SEED))
    samples = repeat_to_length(gmsk(bits, bt))
    note = f"D-STAR DV generator, GMSK BT {bt}, {len(expected)} random AMBE frames"
    return Source(samples, note=note, expected=expected)


# ---------------------------------------------------------------- dPMR and analog


def dpmr_source():
    vectors = bif.load_dpmr_reference_vectors()
    stream = bif.dpmr_symbol_stream(vectors)
    per_copy_s = len(stream) / bif.DPMR_SYNTH_SYMBOL_RATE
    dibits = stream * math.ceil(DURATION_S / per_copy_s)
    samples = bif.modulate_4fsk(dibits, np.random.default_rng(bif.DPMR_SYNTH_SEED))[: sample_count()]
    note = "modulate_4fsk() over the dPMR CCH reference vectors (its own noise 40 dB below the signal in 6.25 kHz)"
    return Source(samples, note=note)


BUILDER_LOCK = threading.Lock()


@contextlib.contextmanager
def builder_constants(**values):
    """Run the fixture builder's tone synthesizers with this tool's tone, length and no noise of their own (rows build
    their sources in parallel threads, so one at a time)."""
    with BUILDER_LOCK:
        saved = {name: getattr(bif, name) for name in values}
        for name, value in values.items():
            setattr(bif, name, value)
        try:
            yield
        finally:
            for name, value in saved.items():
                setattr(bif, name, value)


def nfm_source(width_hz):
    deviation = NFM_TONE_DEVIATION_HZ[width_hz]
    with builder_constants(
        NFM_SYNTH_DURATION_S=DURATION_S, NFM_SYNTH_DEVIATION_HZ=deviation, NFM_SYNTH_NOISE_DB=-math.inf
    ):
        samples = bif.nfm_synth_samples(0, None, None)
    return Source(samples, note=f"nfm_synth_samples(): 1 kHz tone at {deviation:.0f} Hz deviation")


def am_source():
    with builder_constants(AM_SYNTH_DURATION_S=DURATION_S, AM_SYNTH_DEPTH=AM_TONE_DEPTH, AM_SYNTH_NOISE_DB=-math.inf):
        samples = bif.am_synth_samples(0, None, None)
    return Source(samples, note=f"am_synth_samples(): 1 kHz tone at {AM_TONE_DEPTH:.0%} depth")


# ---------------------------------------------------------------- rows

P25P1_PASS = r"Group Voice Channel User - Group 1 Source 1|LDU2 ALG ID: 0x80 KEY ID: 0x0000 MI: 0x0000000000000000"
YSF_PASS = r"Sync: [+-]YSF (?!.*FICH \((?:CRC|FEC) ERR\))"


def line_row(key, mode, label, width, role, args, build, pass_re, unit_re, weight=1):
    return Row(key, mode, label, width, role, args, build, pass_re, unit_re, weight)


def regen_row(key, mode, label, width, args, name, rate, synth, note, pass_re, unit_re, sync_levels=None):
    def build(work_dir):
        return regenerated_source(name, args, rate, synth, note, work_dir, sync_levels)

    return line_row(key, mode, label, width, CLEAN, args, build, pass_re, unit_re)


def off_air_row(key, mode, label, width, args, name, pass_re, unit_re):
    return line_row(key, mode, label, width, OFF_AIR, args, lambda _w: off_air_source(name), pass_re, unit_re)


def digital_rows():
    p25p2_args = ["-f2", "-X", "BEE00164161"]
    rows = [
        regen_row(
            "p25p1",
            "p25p1",
            "P25 Phase 1 C4FM",
            12500,
            ["-f1"],
            "p25p1_c4fm_vc",
            4800,
            lambda lv: fm_from_levels(lv, 4800, c4fm_response, 1800.0),
            "C4FM, outer 1800 Hz",
            P25P1_PASS,
            r"Sync: [+-]P25p1 .*LDU[12]",
        ),
        off_air_row(
            "p25p1_offair",
            "p25p1",
            "P25 Phase 1 C4FM",
            12500,
            ["-f1"],
            "p25p1_c4fm_vc",
            P25P1_PASS,
            r"Sync: [+-]P25p1 .*LDU[12]",
        ),
    ]
    for tag, response, text in (
        ("harmonized", harmonized_response, "H-DQPSK, harmonized raised cosine (alpha 1, 3600 Hz)"),
        ("rrc", rrc_response(0.2, 6000), "H-DQPSK, root raised cosine (alpha 0.2)"),
    ):
        rows.append(
            regen_row(
                f"p25p2_{tag}",
                "p25p2",
                f"P25 Phase 2 ({tag} filter)",
                12500,
                p25p2_args,
                "p25p2_cc",
                6000,
                lambda lv, r=response: hdqpsk_from_levels(lv, r),
                text,
                r"P25p2 SACCH +MAC_",
                r"P25p2 SACCH",
            )
        )
    rows.append(
        off_air_row(
            "p25p2_offair", "p25p2", "P25 Phase 2", 12500, p25p2_args, "p25p2_cc", r"P25p2 SACCH +MAC_", r"P25p2 SACCH"
        )
    )
    dmr_pass, dmr_unit = r"Sync: [+-]DMR .*Color Code=02 \|", r"Sync: [+-]?DMR "
    rows += [
        line_row("dmr_bs", "dmr_bs", "DMR BS", 12500, CLEAN, ["-fs"], dmr_bs_source, dmr_pass, dmr_unit),
        off_air_row(
            "dmr_bs_offair", "dmr_bs", "DMR BS", 12500, ["-fs"], "dmr_voice", dmr_pass.replace("02", "01"), dmr_unit
        ),
        Row("dmr_ms", "dmr_ms", "DMR MS (burst-on S/N)", 12500, CLEAN, ["-fs"], dmr_ms_source),
    ]
    for key, label, width, args, rate, outer in (
        ("nxdn96", "NXDN96", 12500, ["-fn"], 4800, 2400.0),
        ("nxdn48", "NXDN48", 6250, ["-fi"], 2400, 1050.0),
    ):
        pass_re, unit_re = rf"Sync: {label} .*PF [1-4]/4", rf"Sync: {label} "
        rows.append(
            regen_row(
                key,
                key,
                label,
                width,
                args,
                key,
                rate,
                lambda lv, r=rate, o=outer: fm_from_levels(lv, r, nxdn_response(r), o),
                f"RRC 0.2 x sinc, outer {outer:.0f} Hz",
                pass_re,
                unit_re,
                NXDN_FSW_LEVELS,
            )
        )
        rows.append(off_air_row(f"{key}_offair", key, label, width, args, key, pass_re, unit_re))
    rows.append(
        line_row(
            "dpmr",
            "dpmr",
            "dPMR",
            6250,
            CLEAN,
            ["-fm", "-Z"],
            lambda _w: dpmr_source(),
            r"^ FN [1-4]/4",
            r"Sync: [+-]dPMR ",
            2,
        )
    )
    for bt in (0.5, 0.35):
        rows.append(
            Row(f"dstar_bt{bt}", "dstar", f"D-STAR (BT {bt})", 6250, CLEAN, ["-fd"], lambda _w, b=bt: dstar_source(b))
        )
    for outer in (1800.0, 2290.0, 2700.0):
        rows.append(
            regen_row(
                f"ysf_{outer:.0f}",
                "ysf",
                f"YSF (outer {outer:.0f} Hz)",
                12500,
                ["-fy"],
                "ysf",
                4800,
                lambda lv, o=outer: fm_from_levels(lv, 4800, rrc_response(0.2, 4800), o),
                f"RRC 0.2, outer {outer:.0f} Hz",
                YSF_PASS,
                r"Sync: [+-]YSF ",
                YSF_FS_LEVELS,
            )
        )
    rows.append(off_air_row("ysf_offair", "ysf", "YSF", 12500, ["-fy"], "ysf", YSF_PASS, r"Sync: [+-]YSF "))
    m17_pass, m17_unit = r"Sync: [+-]M17 STR LC: [1-6]/6", r"Sync: [+-]M17 STR"
    rows += [
        regen_row(
            "m17",
            "m17",
            "M17",
            12500,
            ["-fz"],
            "m17",
            4800,
            lambda lv: fm_from_levels(lv, 4800, rrc_response(0.5, 4800), 2400.0),
            "RRC 0.5, outer 2400 Hz",
            m17_pass,
            m17_unit,
        ),
        off_air_row("m17_offair", "m17", "M17", 12500, ["-fz"], "m17", m17_pass, m17_unit),
    ]
    return rows


def analog_rows():
    return [
        Row(
            "nfm_12k5",
            "nfm_12k5",
            "NFM 12.5 kHz",
            12500,
            CLEAN,
            ["-fA", "--nfm-bandwidth-hz", "12500"],
            lambda _w: nfm_source(12500),
            analog=True,
        ),
        Row(
            "nfm_25k",
            "nfm_25k",
            "NFM 25 kHz",
            25000,
            CLEAN,
            ["-fA", "--nfm-bandwidth-hz", "25000"],
            lambda _w: nfm_source(25000),
            analog=True,
        ),
        Row(
            "am_6k",
            "am_6k",
            "AM 6 kHz",
            6000,
            CLEAN,
            ["-fM", "--am-bandwidth-hz", "6000"],
            lambda _w: am_source(),
            analog=True,
        ),
        Row(
            "am_20k",
            "am_20k",
            "AM 20 kHz",
            20000,
            CLEAN,
            ["-fM", "--am-bandwidth-hz", "20000"],
            lambda _w: am_source(),
            analog=True,
        ),
    ]


# ---------------------------------------------------------------- measurement


def band_power(source, width_hz):
    """Mean power of the signal inside +/- width/2, over its bursts for a burst train."""
    spectrum = np.abs(np.fft.fft(source.samples)) ** 2 / len(source.samples) ** 2
    freqs = np.fft.fftfreq(len(source.samples), 1.0 / RATE_HZ)
    power = float(spectrum[np.abs(freqs) <= width_hz / 2.0].sum())
    return power if source.on is None else power * len(source.on) / int(np.count_nonzero(source.on))


def noise_for(key, seed):
    rng = np.random.default_rng([ARGS.base_seed, zlib.crc32(key.encode("ascii")), seed])
    n = sample_count()
    return rng.normal(0.0, NOISE_SIGMA, n) + 1j * rng.normal(0.0, NOISE_SIGMA, n)


def render(source, width_hz, noise, snr_db, path_stem):
    """Write source at snr_db over noise (no noise for snr_db None) as a cu8 capture; return its sidecar path."""
    noise_in_band = 2.0 * NOISE_SIGMA**2 * width_hz / RATE_HZ
    level = math.sqrt(
        10.0 ** ((START_DB if snr_db is None else snr_db) / 10.0) * noise_in_band / band_power(source, width_hz)
    )
    samples = level * source.samples if snr_db is None else level * source.samples + noise
    peak = np.maximum(np.abs(samples.real), np.abs(samples.imag)) * 0.9  # to_cu8's headroom without normalizing
    clipped = int(np.count_nonzero(peak > 1.0))
    if clipped > len(samples) // 1000:
        raise SystemExit(f"{path_stem}: {clipped} samples clip at {snr_db} dB; lower START_DB or NOISE_SIGMA")
    out_dir, name = os.path.split(path_stem)
    bif.write_fixture(out_dir, name, samples, normalize=False, notes="scan_quiet_reception.py")
    return path_stem + ".iq.json"


def ambe_prefixes(frames):
    """The 24 Golay-protected parameter bits of each frame: the first six of its 14 hex digits."""
    return collections.Counter(frame[:6] for frame in frames)


def digital_counts(row, source, sidecar, work_dir):
    cmd = [ARGS.dsd, "--frontend", "none"] + row.args + ["--iq-replay", sidecar, "-o", "null"]
    if source.expected is None:
        text = run_text(cmd)
        passed = len(re.findall(row.pass_re, text, flags=re.MULTILINE))
        return passed, len(re.findall(row.unit_re, text, flags=re.MULTILINE)) * row.unit_weight
    log = unique_path(work_dir, row.key + "-frames")
    os.remove(log)
    run_text(cmd + ["--frame-log", log])
    with open(log, encoding="ascii", errors="replace") as handle:
        decoded = re.findall(r"FRAME AMBE slot=\d+ data=([0-9A-F]{14})", handle.read())
    os.remove(log)
    return sum((ambe_prefixes(decoded) & ambe_prefixes(source.expected)).values()), len(source.expected)


def analog_sinad(row, sidecar):
    cmd = [ARGS.analog_host, "--frontend", "none"] + row.args + ["-n", "50", "--iq-replay", sidecar, "-o", "null"]
    text = run_text(cmd + ["--analog-expect-tone-hz", "1000"])
    match = re.search(r"ANALOG METRIC: .*captured_ms=([0-9.]+) .*tone_snr_db=(-?[0-9.]+)", text)
    if match is None:
        raise SystemExit(f"{sidecar}: no tone measurement in the analog host's output:\n{text[-2000:]}")
    snr = float(match.group(2))
    return 10.0 * math.log10(1.0 + 10.0 ** (snr / 10.0)), float(match.group(1))


def descend(measure, record):
    """Lowest grid S/N that meets the criterion, walking down from START_DB: 2 dB steps to the first miss, then 0.5 dB
    steps down from the last pass to the first miss. None when START_DB misses; FLOOR_DB when nothing misses."""
    last_pass = None
    snr = START_DB
    while snr >= FLOOR_DB - 1e-9:
        if not record(snr, measure(snr)):
            break
        last_pass = snr
        snr -= COARSE_DB
    else:
        return FLOOR_DB
    if last_pass is None:
        return None
    snr = last_pass - STEP_DB
    while snr > last_pass - COARSE_DB + 1e-9 and snr >= FLOOR_DB - 1e-9:
        if not record(snr, measure(snr)):
            break
        last_pass = snr
        snr -= STEP_DB
    return last_pass


def seed_threshold(row, source, reference, seed, work_dir, rows):
    noise = noise_for(row.key, seed)

    def measure(snr):
        stem = unique_path(work_dir, f"{row.key}_s{seed}")
        os.remove(stem)
        sidecar = render(source, row.width_hz, noise, snr, stem)
        result = analog_sinad(row, sidecar) if row.analog else digital_counts(row, source, sidecar, work_dir)
        for suffix in (".iq", ".iq.json"):
            os.remove(stem + suffix)
        return result

    def record(snr, measured):
        if row.analog:
            sinad, captured_ms = measured
            rows.append([row.key, seed, f"{snr:.1f}", "", "", "", f"{sinad:.2f}", f"{captured_ms:.0f}"])
            return sinad >= SINAD_TARGET_DB
        passed, units = measured
        ratio = passed / reference
        rows.append([row.key, seed, f"{snr:.1f}", passed, units, reference, f"{ratio:.3f}", ""])
        return ratio >= PASS_FRACTION

    return descend(measure, record)


def summarize(result, thresholds):
    result.seeds = thresholds
    if any(value is None for value in thresholds):
        result.status = "never reached"
        result.detail = f"misses the criterion at {START_DB:g} dB on some seed"
        return
    result.threshold_db = statistics.median(thresholds)
    if any(value <= FLOOR_DB for value in thresholds):
        result.detail = f"meets the criterion down to the {FLOOR_DB:g} dB floor on some seed"


def clean_check(row, source, result, work_dir, rows):
    """The clean run: the reference count, or None when the row is not swept."""
    if row.analog:
        stem = unique_path(work_dir, f"{row.key}_noise")
        os.remove(stem)
        bif.write_fixture(work_dir, os.path.basename(stem), noise_for(row.key, 0), normalize=False)
        sinad, captured_ms = analog_sinad(row, stem + ".iq.json")
        result.clean = {"noise_only_sinad_db": round(sinad, 2), "noise_only_captured_ms": captured_ms}
        rows.append([row.key, "noise", "", "", "", "", f"{sinad:.2f}", f"{captured_ms:.0f}"])
        if sinad >= SINAD_TARGET_DB:
            result.status = "invalid"
            result.detail = f"noise alone reaches {sinad:.1f} dB SINAD"
            return None
        return 1
    stem = unique_path(work_dir, f"{row.key}_clean")
    os.remove(stem)
    passed, units = digital_counts(row, source, render(source, row.width_hz, None, None, stem), work_dir)
    fraction = round(passed / units, 3) if units else 0.0
    result.clean = {"passed": passed, "received": units, "fraction": fraction}
    rows.append([row.key, "clean", "", passed, units, passed, "", ""])
    below = units == 0 or passed < PASS_FRACTION * units
    if passed == 0 or (below and row.role == OFF_AIR):
        result.status = "never reached"
        result.detail = f"the clean signal passes {passed} of {units} frames"
        return None
    if below:
        result.detail = f"the clean signal passes {passed} of {units} frames (errors in the source's own symbols)"
    return passed


def measure_row(row, work_dir, rows, pool):
    metric = "12 dB SINAD, 1 kHz tone" if row.analog else "frames passing CRC/FEC >= 90 % of clean"
    result = Result(row.key, row.mode, row.label, row.width_hz, row.role, metric)
    source = row.build(work_dir)
    if len(source.samples) != sample_count():
        raise SystemExit(f"{row.key}: the source holds {len(source.samples)} samples, not {sample_count()}")
    result.source = source.note
    reference = clean_check(row, source, result, work_dir, rows)
    if reference is None:
        return result
    futures = [pool.submit(seed_threshold, row, source, reference, seed, work_dir, rows) for seed in range(ARGS.seeds)]
    summarize(result, [future.result() for future in futures])
    return result


def kept_thresholds(results):
    """Per mode, the lowest clean-signal threshold among its candidates (None when none was measured)."""
    kept = {}
    for result in results:
        if result.role != CLEAN:
            continue
        best = kept.get(result.mode)
        if result.threshold_db is not None and (best is None or best[0] is None or result.threshold_db < best[0]):
            kept[result.mode] = (result.threshold_db, result.key)
        elif best is None:
            kept[result.mode] = (None, result.key)
    return kept


def write_reports(results, rows):
    with open(os.path.join(ARGS.out, "sweep.csv"), "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["row", "seed", "snr_db", "passed", "received", "reference", "ratio_or_sinad_db", "audio_ms"])
        # Runs finish in any order; sort so a rerun writes the same file.
        writer.writerows(sorted(rows, key=lambda row: [str(cell).zfill(8) for cell in row[:3]]))
    kept = kept_thresholds(results)
    payload = {
        "tool": "tools/scan_quiet_reception.py",
        "dsd": ARGS.dsd,
        "analog_host": ARGS.analog_host,
        "duration_s": DURATION_S,
        "noise_sigma": NOISE_SIGMA,
        "base_seed": ARGS.base_seed,
        "seeds": ARGS.seeds,
        "step_db": STEP_DB,
        "kept": {mode: {"threshold_db": value, "row": key} for mode, (value, key) in kept.items()},
        "rows": [vars(result) for result in results],
    }
    with open(os.path.join(ARGS.out, "thresholds.json"), "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")
    lines = [
        "| row | class width | threshold S/N dB | seeds | role | metric | notes |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for result in results:
        threshold = "n/a" if result.threshold_db is None else f"{result.threshold_db:.1f}"
        seeds = ", ".join("n/a" if value is None else f"{value:.1f}" for value in result.seeds) or "n/a"
        notes = "; ".join(part for part in (result.status, result.detail, result.source) if part)
        lines.append(
            f"| {result.label} | {result.width_hz / 1000:g} kHz | {threshold} | {seeds} | {result.role} | "
            f"{result.metric} | {notes} |"
        )
    lines += ["", "| mode | kept threshold S/N dB (lowest clean-signal row) | from row |", "| --- | --- | --- |"]
    for mode, (value, key) in kept.items():
        lines.append(f"| {mode} | {'n/a' if value is None else f'{value:.1f}'} | {key} |")
    with open(os.path.join(ARGS.out, "thresholds.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("\n".join(lines))


def all_rows():
    return digital_rows() + analog_rows()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    names = [row.key for row in all_rows()]
    modes = sorted({row.mode for row in all_rows()})
    parser.add_argument("--dsd", required=True, help="the dsd-neo binary (a Release build runs the sweep fastest)")
    parser.add_argument("--analog-host", help="dsd-neo_test_analog_replay, needed for the analog rows")
    parser.add_argument("--dmr-synth-gen", help="dsd-neo_test_dmr_synth_gen, needed for dmr_bs and dmr_ms")
    parser.add_argument(
        "--mbelib",
        default=ctypes.util.find_library("mbe-neo"),
        help="mbelib-neo's shared library, which codes the D-STAR AMBE frames (default: found on the library path)",
    )
    parser.add_argument("--out", required=True, help="directory for thresholds.json, thresholds.md and sweep.csv")
    parser.add_argument(
        "--modes",
        default=",".join(modes),
        help=f"comma-separated modes ({', '.join(modes)}) or rows ({', '.join(names)}); default every mode",
    )
    parser.add_argument("--seeds", type=int, default=5, help="noise seeds per row (default 5)")
    parser.add_argument("--base-seed", type=int, default=834, help="noise seed base (default 834)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="decoder runs in parallel")
    args = parser.parse_args()
    wanted = [name.strip() for name in args.modes.split(",") if name.strip()]
    unknown = sorted(set(wanted) - set(names) - set(modes))
    if unknown:
        parser.error(f"unknown mode(s) or row(s): {', '.join(unknown)}")
    args.rows = [row for row in all_rows() if row.key in wanted or row.mode in wanted]
    if args.seeds < 1:
        parser.error("--seeds must be at least 1")
    if any(row.analog for row in args.rows) and not args.analog_host:
        parser.error("the analog rows need --analog-host")
    if any(row.mode in ("dmr_bs", "dmr_ms") and row.role == CLEAN for row in args.rows) and not args.dmr_synth_gen:
        parser.error("dmr_bs and dmr_ms need --dmr-synth-gen")
    if any(row.mode == "dstar" for row in args.rows) and not args.mbelib:
        parser.error("dstar needs --mbelib (mbelib-neo's shared library was not found)")
    return args


def main():
    os.makedirs(ARGS.out, exist_ok=True)
    rows = []
    # One thread per row runs its clean check and waits on its seeds, which run in the pool.
    with (
        tempfile.TemporaryDirectory(prefix="scan-quiet-") as work_dir,
        concurrent.futures.ThreadPoolExecutor(max_workers=max(1, ARGS.jobs)) as pool,
        concurrent.futures.ThreadPoolExecutor(max_workers=len(ARGS.rows)) as row_threads,
    ):
        pending = [row_threads.submit(measure_row, row, work_dir, rows, pool) for row in ARGS.rows]
        results = [future.result() for future in pending]
    write_reports(results, rows)
    return 0


if __name__ == "__main__":
    ARGS = parse_args()
    sys.exit(main())
