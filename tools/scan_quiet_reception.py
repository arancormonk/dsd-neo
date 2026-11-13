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
the signal power is the power during the bursts' symbols, not their power ramps. The noise is fixed (its power and
the cu8 scale never change); the signal is scaled.

Digital criterion: at least 90 % of the frames sent still pass CRC/FEC. A generator knows what it sent, so that is
the denominator, and its clean run (the same signal, no added noise) must itself pass 90 % of them, or the row is
reported "decoder-limited: re-measure after the slicer fix" with no threshold. A signal re-synthesized from a
recording's symbols carries that recording's errors, so its denominator is its clean run's pass count instead, and its
threshold is an upper bound ("baked-in symbol errors"). Each row counts one kind of frame:

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

Rows come in four roles; only "clean signal" rows set a mode's threshold:

  clean signal       an ideal signal at the standard's pulse shape, symbol rate and deviation: a generator, or for
                     P25 Phase 2 (which has none) the recording's own symbols re-synthesized
  regenerated check  a recording's own symbols re-synthesized where a generator sets the threshold
  off-air check      a committed off-air fixture as recorded (deviation rescaled where it was remodulated); one whose
                     clean run passes fewer than 90 % of the frames dsd-neo received is reported as never reaching
                     the criterion
  diagnostic         a generator with one deliberate departure from the standard, which shows why a threshold sits
                     where it does: the YSF frame sync or the NXDN FSW with every symbol at an outer level, as
                     dsd-neo's sign-only sync patterns draw them

Where a parameter stays uncertain after research the tool measures every candidate (one row each) and keeps the
lower threshold for the mode, the safe direction for the skip calibration: YSF's deviation (1800 Hz, C4FM's own;
2290 Hz, OP25 dv_tx.py's 0.42/0.33 of it; 2700 Hz, MMDVM's YSF_LEVELA_HI 1893 against DMR_LEVELA 1362 for 1944 Hz
through the same filter), P25 Phase 2's transmit filter and D-STAR's Gaussian BT.

Signal sources. Each FM transmit filter is applied to symbol
impulses at unity DC gain, so a held level 3 is the outer deviation:

  p25p1    this tool's LDU1/LDU2 generator (p25_*() below, TIA-102.BAAA as MMDVMHost lays it out): frame sync, NID
           (BCH (63,16) from tests/test_support/p25_nid_generator.hpp), link control (RS (24,12,13)) or encryption
           sync (RS (24,16,9)) in Hamming (10,6,3) words, status symbols, random voice; C4FM: raised cosine
           (alpha 0.2) times inverse sinc, outer 1800 Hz
  p25p2    the symbols dsd-neo reads from p25p2_cc (its -c symbol capture, recovered by symbol_levels()), cut to the
           carrier and repeated to 10 s: H-DQPSK, the base station's outbound (+/-pi/4, +/-3pi/4 per symbol at 6000
           symbol/s), through the "harmonized" raised cosine (alpha 1, 3600 Hz) or a root raised cosine (alpha 0.2)
  dmr_bs   the DMR test generator's symbols (tests/protocol/dmr/dmr_synth.c, --dmr-synth-gen; slot 1 a clear voice
           call, slot 2 idle) through ETSI TS 102 361-1 10.2.2.2's square root raised cosine (alpha 0.2) at the
           outer deviation of 1944 Hz (10.2.2.1)
  dmr_ms   the same generator with --ms (MS syncs, no CACH, slot 1's bursts alone, 27.5 ms on every 60 ms); the
           carrier is at full power for each burst's 27.5 ms and ramps up and down over 1.5 ms either side, inside the
           guard time (ETSI TS 102 361-1 10.2.3.1.1, figure 10.3)
  nxdn96, nxdn48   this tool's NXDN generator (nxdn_*() below, NXDN TS 1-A and MMDVMHost): RDCH voice frames of
           FSW, LICH, SACCH (CRC-6, K=5 rate-1/2 code, punctured, interleaved) carrying a VCALL over the four-frame
           superframe, and random voice bits, scrambled by the X^9+X^4+1 PN per frame; RRC 0.2 x sinc, outer 2400 /
           1050 Hz (NXDN TS 1-A 3.3-3.4)
  ysf      this tool's YSF generator (ysf_*() below, MMDVMHost and dsd-neo's decoder): frame sync, FICH (Golay (24,12),
           CRC-16, K=5 rate-1/2 code, interleave) of a V/D mode 2 frame, then random payload bits; RRC 0.2 at
           1800, 2290 or 2700 Hz
  dpmr     the dPMR CCH reference frames (build_iq_fixtures.dpmr_symbol_stream()) through ETSI TS 102 658 14.2.2.2's
           square root raised cosine (alpha 0.2) on held symbols (x sinc), outer 1050 Hz
  dstar    this tool's D-STAR DV transmission (dstar_*() below): bit sync, frame sync, the radio header (CRC-CCITT,
           rate-1/2 K=3 convolutional code, 24-bit interleave, x^7+x^4+1 scrambler), then voice frames of 72 AMBE
           bits and 24 data bits with the sync pattern every 21st frame, GMSK at 4800 bit/s, h = 0.5, BT 0.5 or
           0.35. The AMBE frames are random parameter bits (no special pitch codes) FEC-coded and interleaved by
           mbelib-neo's mbe_encodeAmbe3600x2400Frame() and mbe_encodeDStarDVData() (--mbelib)
  m17      the symbols dsd-neo's own M17 stream encoder (-fZ) transmits, from its -c capture; root raised cosine
           (alpha 0.5), outer 2400 Hz (M17 specification 1.2-1.3)
  nfm, am  build_iq_fixtures.nfm_synth_samples() / am_synth_samples() at this tool's deviation, depth and length
  regenerated and off-air checks   p25p1_c4fm_vc, nxdn96, nxdn48, ysf and m17 re-synthesized from their captured
           symbols at their mode's pulse and deviation, or the committed fixtures as recorded

Analog criterion: 12 dB SINAD of a 1 kHz tone in dsd-neo's monitor audio (TIA-603 for NFM, ETSI EN 300 676-1 for
AM), at each configured receive width the scan check calibrates: NFM 8, 11.25, 12.5, 16, 20 and 25 kHz, AM 5, 6, 8,
10, 15 and 20 kHz (the Radio sheet's width steps). NFM carries the tone at 60 % of the transmitter's maximum deviation:
1.5 kHz for a 12.5 kHz channel's transmitter (the 8 and 12.5 kHz widths), 3 kHz for a 25 kHz channel's (the 25 kHz
width), both at 11.25, 16 and 20 kHz, where either kind is received and the lower threshold is kept. AM is at 30 %
depth. The audio is scored by dsd-neo_test_analog_replay (tests/engine/analog_replay.c), which runs the real engine and
fits the tone to every delivered 20 ms block after the monitor's filters: SINAD is 10 log10(1 + S/(N+D)) from its
tone_snr_db. Gain is fixed (-n 50) and the replay squelch
is the -110 dB default, which passes everything; noise alone must fail the metric, and the tool reports that it does.

Everything is deterministic: the noise seeds are fixed (--base-seed), the generators run with fixed arguments and
seeds, and dsd-neo replays at --iq-replay-rate fast.

Usage:
    python3 tools/scan_quiet_reception.py --dsd build/dev-release/apps/dsd-cli/dsd-neo \\
        --analog-host build/dev-debug/tests/dsd-neo_test_analog_replay \\
        --dmr-synth-gen build/dev-debug/tests/dsd-neo_test_dmr_synth_gen --out OUT_DIR [--modes LIST] [--jobs N]

It writes OUT_DIR/thresholds.json, OUT_DIR/thresholds.md (each row, then each mode's kept threshold) and
OUT_DIR/sweep.csv (every run). With --export-sources DIR instead of --out it measures nothing and writes the requested
rows' clean sources for the scan check's calibration driver (tests/io/signal_quiet_calibrate.cpp; see export_sources()
and docs/signal-search.md "Quiet-channel calibration"). Exit status 0 when every requested row was measured or
reported, 1 when a decoder or generator run fails or a capture would clip, 2 on a usage error.
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
REGEN_CHECK = "regenerated check"
DIAGNOSTIC = "diagnostic"
OFF_AIR = "off-air check"

# Nominal outer symbol deviation (Hz) of the off-air fixtures remodulated from discriminator audio, for the off-air
# check rows. Their present outer level is estimated by outer_deviation_hz(), which reads 1805 Hz on the off-air C4FM
# I/Q (nominal 1800 Hz) and 1054 Hz on the dPMR synthesis (1050 Hz).
OFF_AIR_DEVIATION = {"p25p2_cc": 2250.0, "ysf": 2290.0, "m17": 2400.0, "dmr_voice": 1944.0}
OUTER_WINDOW = 4  # samples averaged (under half a 4800 symbol/s symbol at 48 kHz)
CARRIER_ON_FRACTION = 0.1  # a 10 ms window is carrier when its power exceeds this share of the clip's 90th percentile

DMR_OUTER_HZ = 1944.0  # ETSI TS 102 361-1 section 10.2.2.1
DMR_SYNTH_OUTER = bif.DMR_SYNTH_OUTER_LEVEL
DMR_GEN_ARGS = ["--cc", "2", "--s1", "voice", "--s2", "idle", "--snr", "200", "--seed", "7", "--pulse", "rc"]
DMR_SLOT_SYMBOLS = 144  # 12 CACH (or MS guard) + 132 burst dibits: 30 ms at 4800 symbol/s
DMR_LEAD_SYMBOLS = 960  # dmr_synth.c: the random (with --ms, silent) lead before the first period
DMR_RAMP_S = 0.0015  # ETSI TS 102 361-1 10.2.3.1.1, figure 10.3: regions A and C
# The test tone at 60 % of the transmitter's maximum deviation: +/-2.5 kHz for a 12.5 kHz channel's (narrowband),
# +/-5 kHz for a 25 kHz channel's (wideband). The 11.25, 16 and 20 kHz widths, at which either kind is received, are
# measured with both and keep the lower threshold.
NFM_NARROW_TONE_HZ = 1500.0
NFM_WIDE_TONE_HZ = 3000.0
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
    sent: int = None  # frames a generator sent, the criterion's denominator; None for a recording


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
    regenerated: bool = False  # from a recording's captured symbols, which keep its errors


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
    """The DMR test generator's discriminator audio and its expectation rows (slot, superframe, VC, frame, hex)."""
    gen_dir = tempfile.mkdtemp(prefix="dmr-", dir=work_dir)
    args = ["-o", "dmr.wav", "-e", "dmr.txt", "-t", f"{DURATION_S:g}"] + DMR_GEN_ARGS + extra
    subprocess.run([os.path.abspath(ARGS.dmr_synth_gen)] + args, cwd=gen_dir, check=True, capture_output=True)
    with wave.open(os.path.join(gen_dir, "dmr.wav"), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getframerate() != RATE_HZ:
            raise SystemExit("expected 48 kHz mono s16 from the DMR generator")
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)
    with open(os.path.join(gen_dir, "dmr.txt"), encoding="ascii") as handle:
        expected = [line.split() for line in handle if line.strip() and not line.startswith("#")]
    return audio, expected


def dmr_levels(audio):
    """The generator's symbol levels: it centres symbol k on sample k * sps with raised-cosine pulses (--pulse rc in
    DMR_GEN_ARGS), which cross zero at every other centre, so the audio there is the level times DMR_SYNTH_OUTER / 3 (0
    for an --ms silent symbol)."""
    levels = np.round(audio[:: RATE_HZ // 4800] * 3.0 / DMR_SYNTH_OUTER)
    if not set(np.unique(levels)) <= {-3.0, -1.0, 0.0, 1.0, 3.0}:
        raise SystemExit("the DMR generator's audio does not sit on its symbol levels at the symbol centres")
    return levels


def dmr_baseband(levels):
    """ETSI TS 102 361-1 section 10.2.2.2: impulses through the square root raised cosine (alpha 0.2), the outer
    level at +/-1944 Hz (section 10.2.2.1)."""
    return fm_from_levels(levels, 4800, rrc_response(0.2, 4800), DMR_OUTER_HZ)[: sample_count()]


def dmr_bs_source(work_dir):
    audio, _expected = dmr_synth_audio(work_dir, [])
    levels = dmr_levels(audio)
    sent = (sample_count() // (RATE_HZ // 4800) - DMR_LEAD_SYMBOLS) // DMR_SLOT_SYMBOLS
    note = f"dmr_synth.c BS symbols, slot 1 voice, slot 2 idle, SRRC 0.2, outer {DMR_OUTER_HZ:.0f} Hz"
    return Source(dmr_baseband(levels), note=note, sent=sent)


def dmr_ms_voice_period(superframe, vc):
    """The 30 ms period an MS voice burst occupies: slot 1's burst list is the voice LC header, then the superframes
    (dmr_synth.c build_schedule() with --lead 0, one header and no encryption)."""
    return 2 * (1 + 6 * superframe + vc - 1)


def dmr_ms_source(work_dir, ramp=True):
    """dmr_synth.c --ms: a burst is dibits 12-143 of every even 144-dibit period after the 960-dibit lead, symbol k
    centred on sample k * sps; a period whose burst the generator left silent (the call over) stays keyed off.

    The carrier is the burst's 27.5 ms symbol transmission period (region B of ETSI TS 102 361-1 figure 10.3: from
    half a symbol before the first symbol's centre to half a symbol after the last's) at full power, with a raised
    cosine power ramp in the 1.5 ms before it and after it (regions A and C, clause 10.2.3.1.1), inside the guard
    time; nothing else. ramp=False keys it on and off at the region B edges instead. The frames sent are the voice
    frames whose burst ends inside the clip."""
    audio, expected = dmr_synth_audio(work_dir, ["--ms", "--lead", "0", "--tail", "0"])
    levels = dmr_levels(audio)
    n = sample_count()
    sps = RATE_HZ // 4800
    samples = dmr_baseband(levels)
    symbol = (np.arange(n) + sps // 2) // sps - DMR_LEAD_SYMBOLS
    period = np.floor_divide(symbol, DMR_SLOT_SYMBOLS)
    on = (symbol >= 0) & (period % 2 == 0) & (np.mod(symbol, DMR_SLOT_SYMBOLS) >= 12)
    keyed = np.abs(levels[np.clip((np.arange(n) + sps // 2) // sps, 0, len(levels) - 1)]) > 0
    envelope = np.zeros(n)
    ramp_n = round(DMR_RAMP_S * RATE_HZ)
    shape = 0.5 - 0.5 * np.cos(math.pi * (np.arange(ramp_n) + 0.5) / ramp_n)
    for p in np.unique(period[on]):
        span = np.flatnonzero(on & (period == p))
        if not np.any(keyed[span]):
            on[span] = False
            continue
        first, last = int(span[0]), int(span[-1]) + 1
        envelope[first:last] = 1.0
        if ramp:
            envelope[max(0, first - ramp_n) : first] = shape[ramp_n - (first - max(0, first - ramp_n)) :]
            envelope[last : min(n, last + ramp_n)] = shape[::-1][: min(n, last + ramp_n) - last]
    last_symbol = n // sps
    frames = [
        row[4]
        for row in expected
        if DMR_LEAD_SYMBOLS + (dmr_ms_voice_period(int(row[1]), int(row[2])) + 1) * DMR_SLOT_SYMBOLS <= last_symbol
    ]
    note = "dmr_synth.c --ms symbols, slot 1 voice bursts, SRRC 0.2" + (
        ", 1.5 ms power ramps" if ramp else ", keyed hard"
    )
    return Source(samples * envelope, note=note, on=on, expected=frames, sent=len(frames))


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
    return Source(samples, note=note, expected=expected, sent=len(expected))


# ---------------------------------------------------------------- shared FEC (YSF and NXDN generators)


def msb_bits(value, width):
    return [(value >> (width - 1 - i)) & 1 for i in range(width)]


def k5_convolve(bits):
    """Rate-1/2, K = 5 convolutional code of YSF and NXDN: g1 = 1+D^3+D^4 sent first, g2 = 1+D+D^2+D^4, register
    from zero (MMDVMHost YSFConvolution.cpp and NXDNConvolution.cpp encode(); NXDN TS 1-A 4.5)."""
    d1 = d2 = d3 = d4 = 0
    out = []
    for d in bits:
        out += [d ^ d3 ^ d4, d ^ d1 ^ d2 ^ d4]
        d1, d2, d3, d4 = d, d1, d2, d3
    return out


def bits_to_levels(bits):
    """Bit pairs to 4FSK levels, first bit most significant (01 +3, 00 +1, 10 -1, 11 -3; NXDN TS 1-A Table 3.3-1,
    the YSF and P25 mapping alike)."""
    return np.array([bif.DPMR_DIBIT_TO_LEVEL[2 * bits[i] + bits[i + 1]] for i in range(0, len(bits), 2)])


# ---------------------------------------------------------------- YSF generator
#
# Yaesu publishes no System Fusion air interface, so this follows two independent implementations: MMDVMHost
# (YSFFICH.cpp, YSFConvolution.cpp, Golay24128.cpp, CRC.cpp addCCITT162) and dsd-neo's decoder
# (src/protocol/ysf/ysf.c ysf_conv_fich(), ysf_crc16()). A frame is 480 symbols (100 ms at 4800 symbol/s): the
# frame sync, the FICH (200 bits), then 720 payload bits.

YSF_FS = 0xD471C9634D  # MMDVM YSFDefines.h YSF_SYNC_BYTES; dsd-neo FUSION_SYNC
YSF_FRAMES_PER_SECOND = 10
YSF_PAYLOAD_BITS = 720
YSF_SEED = 834
GOLAY_23_12 = 0xC75  # x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1


def golay_24_12(data):
    """Extended Golay (24,12): data in the top 12 bits, the (23,12) parity, then even overall parity (MMDVM
    ENCODING_TABLE_24128 layout)."""
    rem = data << 11
    for shift in range(22, 10, -1):
        if rem >> shift & 1:
            rem ^= GOLAY_23_12 << (shift - 11)
    word = ((data << 11) | rem) << 1
    return word | (word.bit_count() & 1)


def crc16_ccitt_inverted(bits):
    """CRC-16 x^16+x^12+x^5+1, register from zero, inverted (MMDVM addCCITT162; dsd-neo ysf_crc16())."""
    crc = 0
    for bit in bits:
        top = (crc >> 15) & 1
        crc = (crc << 1) & 0xFFFF
        if top ^ bit:
            crc ^= 0x1021
    return crc ^ 0xFFFF


def ysf_fich_bits(fn):
    """The 200 transmitted FICH bits of a V/D mode 2 communications frame numbered fn of 8: FI 1, CS 0, CM 0 (group),
    BN 0, BT 0, FN, FT 7, MR 2, VoIP 0, DT 2, SQL 0."""
    info = msb_bits(1, 2) + msb_bits(0, 2) + msb_bits(0, 2) + msb_bits(0, 2) + msb_bits(0, 2) + msb_bits(fn, 3)
    info += msb_bits(7, 3) + [0, 0] + msb_bits(2, 3) + [0] + msb_bits(2, 2) + [0] + msb_bits(0, 7)
    info += msb_bits(crc16_ccitt_inverted(info), 16)
    words = [int("".join(map(str, info[12 * k : 12 * k + 12])), 2) for k in range(4)]
    coded = k5_convolve([b for w in words for b in msb_bits(golay_24_12(w), 24)] + [0, 0, 0, 0])
    sent = [0] * 200
    for i in range(100):
        position = 40 * (i % 5) + 2 * (i // 5)  # MMDVM YSFFICH.cpp INTERLEAVE_TABLE
        sent[position], sent[position + 1] = coded[2 * i], coded[2 * i + 1]
    return sent


def ysf_levels(frames, outer_sync=False):
    """The symbol levels of the generator's first frames (also the I/Q fixture ysf_synth in
    tools/build_iq_fixtures.py)."""
    rng = np.random.default_rng(YSF_SEED)
    levels = []
    for frame in range(frames):
        sync = bits_to_levels(msb_bits(YSF_FS, 40))
        if outer_sync:
            sync = 3.0 * np.sign(sync)
        rest = ysf_fich_bits(frame % 8) + rng.integers(0, 2, YSF_PAYLOAD_BITS).tolist()
        levels += [sync, bits_to_levels(rest)]
    return np.concatenate(levels)


def ysf_source(outer_hz, outer_sync=False):
    """outer_sync=True (a diagnostic, not a YSF signal) sends the frame sync's symbols at outer levels, as dsd-neo's
    sign-only sync pattern draws them; the standard frame sync has inner symbols too."""
    levels = ysf_levels(round(DURATION_S * YSF_FRAMES_PER_SECOND), outer_sync)
    samples = fm_from_levels(levels, 4800, rrc_response(0.2, 4800), outer_hz)[: sample_count()]
    note = f"YSF V/D mode 2 generator, RRC 0.2, outer {outer_hz:.0f} Hz" + (
        ", sync at outer levels" if outer_sync else ""
    )
    return Source(samples, note=note, sent=round(DURATION_S * YSF_FRAMES_PER_SECOND))


# ---------------------------------------------------------------- NXDN generator
#
# NXDN TS 1-A (frame, LICH, SACCH coding, scrambler) and MMDVMHost (NXDNLICH.cpp, NXDNSACCH.cpp, NXDNCRC.cpp
# createCRC6(), NXDNConvolution.cpp, NXDNControl.cpp SCRAMBLER), checked against dsd-neo's decoder
# (src/protocol/nxdn). A frame is 384 bits (192 symbols): FSW 20, LICH 16, SACCH 60, then 288 bits of voice. NXDN96
# and NXDN48 share it at 4800 and 2400 symbol/s (40 and 80 ms).

NXDN_FSW = 0xCDF59  # NXDN TS 1-A Table 4.4-2
# LICH: RF channel RDCH (2), functional channel SACCH superframe (2), option voice in both halves (3), outbound (1)
NXDN_LICH = (2 << 6) | (2 << 4) | (3 << 2) | (1 << 1)
NXDN_RAN = 1
NXDN_SEED = 834
# The 72-bit layer 3 message the superframe's four SACCHs carry: VCALL (type 0x01), group, source 101, destination
# 1, clear (NXDN TS 1-A 6.4.1.1 field order, as MMDVM NXDNLayer3).
NXDN_SACCH_MESSAGE = bytes([0x01, 0x00, 0x20, 0x00, 0x65, 0x00, 0x01, 0x00, 0x00])
NXDN_SACCH_PUNCTURE = (5, 11, 17, 23, 29, 35, 41, 47, 53, 59, 65, 71)  # MMDVM NXDNSACCH.cpp PUNCTURE_LIST


def nxdn_scrambler_signs():
    """The per-symbol sign of the scrambler over a frame's 192 symbols: X^9 + X^4 + 1 from S8..S0 = 011100100, reset
    every frame, nothing on the FSW's 10 symbols (NXDN TS 1-A 4.6)."""
    reg = [0, 0, 1, 0, 0, 1, 1, 1, 0]  # S0..S8
    signs = [1] * 10
    for _ in range(182):
        out = reg[0]
        signs.append(-1 if out else 1)
        reg = reg[1:] + [reg[0] ^ reg[4]]
    return np.array(signs, dtype=np.float64)


def nxdn_crc6(bits):
    """CRC-6 of the SACCH: x^6 + x^5 + x^2 + x + 1, register 0x3F (MMDVM createCRC6(); NXDN TS 1-A 4.5.1)."""
    crc = 0x3F
    for bit in bits:
        top = (crc >> 5) & 1
        crc = (crc << 1) & 0x3F
        if bit ^ top:
            crc ^= 0x27
    return msb_bits(crc, 6)


def nxdn_frame_bits(part, rng):
    """One RDCH voice frame carrying part (0-3) of the superframe's SACCH message; 384 bits before scrambling."""
    lich = NXDN_LICH | ((NXDN_LICH >> 4).bit_count() & 1)  # even parity over the 4 most significant bits
    bits = msb_bits(NXDN_FSW, 20)
    for b in msb_bits(lich, 8):
        bits += [b, 1]  # NXDN TS 1-A 4.5.3: each LICH bit sent as the dibit (b, 1)
    message = [b for byte in NXDN_SACCH_MESSAGE for b in msb_bits(byte, 8)]
    sacch = msb_bits(3 - part, 2) + msb_bits(NXDN_RAN, 6) + message[18 * part : 18 * part + 18]
    coded = k5_convolve(sacch + nxdn_crc6(sacch) + [0, 0, 0, 0])
    punctured = [b for i, b in enumerate(coded) if i not in NXDN_SACCH_PUNCTURE]
    interleaved = [0] * 60
    for i, b in enumerate(punctured):
        interleaved[5 * (i % 12) + i // 12] = b  # MMDVM NXDNSACCH.cpp INTERLEAVE_TABLE
    return bits + interleaved + rng.integers(0, 2, 288).tolist()


def nxdn_levels(frames, outer_sync=False):
    """The symbol levels of the generator's first frames, 192 a frame at either rate (also the I/Q fixtures
    nxdn96_synth and nxdn48_synth, tools/build_iq_fixtures.py)."""
    rng = np.random.default_rng(NXDN_SEED)
    signs = nxdn_scrambler_signs()
    levels = []
    for frame in range(frames):
        frame_levels = bits_to_levels(nxdn_frame_bits(frame % 4, rng)) * signs
        if outer_sync:
            frame_levels[:10] = 3.0 * np.sign(frame_levels[:10])
        levels.append(frame_levels)
    return np.concatenate(levels)


def nxdn_source(symbol_rate, outer_hz, outer_sync=False):
    """outer_sync=True (a diagnostic, not an NXDN signal) sends the FSW's two inner symbols at outer levels, as
    dsd-neo's sign-only sync pattern draws them."""
    frames = round(DURATION_S * symbol_rate / 192)
    synth = fm_from_levels(nxdn_levels(frames, outer_sync), symbol_rate, nxdn_response(symbol_rate), outer_hz)
    note = f"NXDN RDCH voice generator, RRC 0.2 x sinc, outer {outer_hz:.0f} Hz"
    note += ", FSW at outer levels" if outer_sync else ""
    return Source(repeat_to_length(synth), note=note, sent=frames)


# ---------------------------------------------------------------- P25 Phase 1 generator
#
# Voice LDU1/LDU2 frames after TIA-102.BAAA (frame sync, NID, link control and encryption sync words, status symbols),
# laid out as MMDVMHost writes them (P25Data.cpp encodeLDU1()/encodeLDU2(), P25Utils.cpp encode(), P25NID.cpp,
# RS634717.cpp, Hamming.cpp encode1063()), with the NID's BCH parity from the repository's own
# tests/test_support/p25_nid_generator.hpp. The IMBE voice and low speed data bits are seeded random: the verdict reads
# only the link control (LDU1) and encryption sync (LDU2). An LDU is 1728 bits (864 symbols, 180 ms at 4800 symbol/s).

P25_FS = 0x5575F5FF77FF  # MMDVM P25_SYNC_BYTES; TIA-102.BAAA frame sync
P25_NAC = 0x293
P25_LDU_BITS = 1728
P25_SEED = 834
P25_STATUS_FIRST = 70  # MMDVM P25_SS0_START: a status symbol after every 70 bits, 72 bits apart
P25_NID_PARITY_ROWS = (
    0xCD930BDD3B2A,
    0xAB5A8E33A6BE,
    0x983E4CC4E874,
    0x4C1F2662743A,
    0xEB9C98EC0136,
    0xB85D47AB3BB0,
    0x5C2EA3D59DD8,
    0x2E1751EACEEC,
    0x170BA8F56776,
    0xC616DFA78890,
    0x630B6FD3C448,
    0x3185B7E9E224,
    0x18C2DBF4F112,
    0xC1F2662743A2,
    0xAD6A38CE9AFB,
    0x9B2617BA7657,
)
# Where each 40-bit Hamming-coded group of the link control or encryption sync goes (MMDVM encodeLDU1()/encodeLDU2())
P25_LDU_HEX_SPANS = ((410, 452), (600, 640), (788, 830), (978, 1020), (1168, 1208), (1356, 1398))


def gf64_mul(a, b):
    """GF(2^6) product, x^6 + x + 1 (MMDVM RS634717.cpp gf6Mult())."""
    product = 0
    for _ in range(6):
        if b & 1:
            product ^= a
        a <<= 1
        if a & 0x40:
            a ^= 0x43
        b >>= 1
    return product


def rs64_encode(data, parity):
    """Systematic Reed-Solomon over GF(64) with generator roots alpha^1..alpha^parity: the P25 (24,12,13) and
    (24,16,9) codes (equal to MMDVM's ENCODE_MATRIX_241213 and ENCODE_MATRIX_24169 products)."""
    generator = [1]
    root = 1
    for _ in range(parity):
        root = gf64_mul(root, 2)
        generator = [gf64_mul(a, root) ^ b for a, b in zip(generator + [0], [0] + generator)]
    generator = generator[::-1]
    remainder = list(data) + [0] * parity
    for i in range(len(data)):
        factor = remainder[i]
        if factor:
            for j in range(1, len(generator)):
                remainder[i + j] ^= gf64_mul(generator[j], factor)
    return list(data) + remainder[len(data) :]


def p25_hamming_10_6_3(d):
    """P25's Hamming (10,6,3) (MMDVM encode1063(); dsd-neo hamming_encode_parity())."""
    return d + [
        d[0] ^ d[1] ^ d[2] ^ d[5],
        d[0] ^ d[1] ^ d[3] ^ d[5],
        d[0] ^ d[2] ^ d[3] ^ d[4],
        d[1] ^ d[2] ^ d[3] ^ d[4],
    ]


def p25_place(frame, bits, start, stop):
    """Write bits into frame[start:stop], skipping the status symbol positions (MMDVM P25Utils encode())."""
    n = 0
    for position in range(start, stop):
        if position >= P25_STATUS_FIRST and (position - P25_STATUS_FIRST) % 72 < 2:
            continue
        frame[position] = bits[n]
        n += 1
    if n != len(bits):
        raise SystemExit(f"P25 span {start}-{stop} holds {n} bits, not {len(bits)}")


def p25_nid_bits(duid):
    """NAC and DUID, BCH (63,16) parity (tests/test_support/p25_nid_generator.hpp), then the parity bit LDUs set."""
    info = msb_bits(P25_NAC, 12) + msb_bits(duid, 4)
    packed = 0
    for bit, row in zip(info, P25_NID_PARITY_ROWS):
        if bit:
            packed ^= row >> 1
    packed |= int("".join(map(str, info)), 2) << 47
    return msb_bits(packed, 63) + [1]


def p25_ldu_bits(duid, hexbits, rng):
    frame = rng.integers(0, 2, P25_LDU_BITS).tolist()
    p25_place(frame, msb_bits(P25_FS, 48), 0, 48)
    p25_place(frame, p25_nid_bits(duid), 48, 114)
    bits = [b for h in hexbits for b in msb_bits(h, 6)]
    for group, (start, stop) in enumerate(P25_LDU_HEX_SPANS):
        words = bits[24 * group : 24 * group + 24]
        coded = [b for k in range(4) for b in p25_hamming_10_6_3(words[6 * k : 6 * k + 6])]
        p25_place(frame, coded, start, stop)
    return frame


def p25_hexbits(data):
    """Bytes as 6-bit symbols, most significant first (MMDVM bin2Hex())."""
    bits = [b for byte in data for b in msb_bits(byte, 8)]
    return [int("".join(map(str, bits[i : i + 6])), 2) for i in range(0, len(bits), 6)]


def p25p1_source():
    """LDU1 (group voice, group 1, source 1) and LDU2 (clear: ALGID 0x80, key 0, MI 0) in turn, C4FM."""
    lc = bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01])  # LCF, MFID, service, group, source
    es = bytes(9) + bytes([0x80, 0x00, 0x00])  # MI, ALGID, KID
    ldu1 = rs64_encode(p25_hexbits(lc), 12)
    ldu2 = rs64_encode(p25_hexbits(es), 8)
    rng = np.random.default_rng(P25_SEED)
    count = math.ceil(sample_count() / (P25_LDU_BITS // 2 * (RATE_HZ // 4800)))
    bits = []
    for k in range(count):
        bits += p25_ldu_bits(0x5, ldu1, rng) if k % 2 == 0 else p25_ldu_bits(0xA, ldu2, rng)
    samples = fm_from_levels(bits_to_levels(bits), 4800, c4fm_response, 1800.0)[: sample_count()]
    sent = sample_count() // (P25_LDU_BITS // 2 * (RATE_HZ // 4800))
    return Source(samples, note="P25 Phase 1 LDU1/LDU2 generator, C4FM, outer 1800 Hz", sent=sent)


# ---------------------------------------------------------------- M17 generator


M17_ENCODER_SECONDS = 12  # audio fed to the encoder: more than DURATION_S of frames after its preamble and LSF
M17_STREAM_SYNC = (-3, -3, -3, -3, 3, 3, -3, 3)  # 0xFF5D (M17 specification 2.5; dsd-neo M17_STR)


def m17_source(work_dir):
    """dsd-neo's own M17 stream encoder (-fZ, VOX keyed by a steady 440 Hz tone on stdin) writes the symbols it
    transmits to its -c capture; they are re-synthesized at the M17 specification's RRC (alpha 0.5) and +/-2400 Hz.
    The encoder is deterministic: the same audio gives the same capture byte for byte. The clip starts at the preamble
    (the encoder's dead air before it is no transmission), and the frames sent are the stream frames inside it."""
    t = np.arange(M17_ENCODER_SECONDS * RATE_HZ) / RATE_HZ
    pcm = np.round(0.3 * 32767.0 * np.sin(2.0 * math.pi * 440.0 * t)).astype("<i2").tobytes()
    path = unique_path(work_dir, "m17-symbols")
    os.remove(path)
    cmd = [ARGS.dsd, "--frontend", "none", "-fZ", "-M", "M17:7:N0CALL:ALL:48000:1", "-i", "-", "-o", "null", "-c", path]
    proc = subprocess.run(cmd, input=pcm, capture_output=True, timeout=300, check=False)
    if proc.returncode != 0:
        raise SystemExit(f"the M17 encoder exited {proc.returncode}")
    with open(path, "rb") as handle:
        raw = handle.read()
    os.remove(path)
    records = np.frombuffer(raw[16:], dtype=np.uint8).reshape(-1, 10)
    levels = records[:, 6:10].copy().view("<f4").ravel().astype(np.float64)
    # The encoder sends dead air (a constant +1 symbol) before the transmission and after it; start at the preamble.
    preamble = np.array([3.0, -3.0] * 4)
    start = next(k for k in range(len(levels) - 8) if np.array_equal(levels[k : k + 8], preamble))
    levels = levels[start:]
    symbols = round(DURATION_S * 4800)
    if len(levels) < symbols:
        raise SystemExit(f"the M17 encoder wrote {len(levels)} symbols, fewer than {symbols}")
    levels = levels[:symbols]
    starts = range(0, symbols - 191, 192)
    sent = sum(1 for k in starts if tuple(levels[k : k + 8]) == M17_STREAM_SYNC)
    samples = fm_from_levels(levels, 4800, rrc_response(0.5, 4800), 2400.0)[: sample_count()]
    return Source(samples, note="dsd-neo's M17 stream encoder symbols, RRC 0.5, outer 2400 Hz", sent=sent)


# ---------------------------------------------------------------- dPMR and analog


def dpmr_source():
    """The dPMR CCH reference frames (build_iq_fixtures.dpmr_symbol_stream()) from the first sample, through ETSI TS
    102 658 section 14.2.2.2's transmit filter: the square root raised cosine (alpha 0.2) on symbols held for a symbol
    period, whose receive side undoes that hold's sinc (the same PHY and filter as NXDN48, NXDN TS 1-A 3.4). The
    frames sent are the half-frames (two per frame, each with its own CCH) inside the clip."""
    vectors = bif.load_dpmr_reference_vectors()
    stream = bif.dpmr_symbol_stream(vectors)
    frame_symbols = len(stream) // bif.DPMR_SYNTH_FRAME_REPEATS
    symbols = round(DURATION_S * bif.DPMR_SYNTH_SYMBOL_RATE)
    levels = np.array([bif.DPMR_DIBIT_TO_LEVEL[d] for d in stream * math.ceil(symbols / len(stream))])[:symbols]
    samples = fm_from_levels(levels, 2400, nxdn_response(2400), bif.DPMR_SYNTH_OUTER_DEVIATION_HZ)[: sample_count()]
    note = "dPMR CCH reference frames, SRRC 0.2 x sinc, outer 1050 Hz"
    return Source(samples, note=note, sent=2 * (symbols // frame_symbols))


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


def nfm_source(deviation):
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


def regen_row(key, mode, label, width, args, name, rate, synth, note, pass_re, unit_re, sync_levels=None, role=CLEAN):
    def build(work_dir):
        return regenerated_source(name, args, rate, synth, note, work_dir, sync_levels)

    row = line_row(key, mode, label, width, role, args, build, pass_re, unit_re)
    row.regenerated = True
    return row


def off_air_row(key, mode, label, width, args, name, pass_re, unit_re):
    return line_row(key, mode, label, width, OFF_AIR, args, lambda _w: off_air_source(name), pass_re, unit_re)


def digital_rows():
    p25p2_args = ["-f2", "-X", "BEE00164161"]
    rows = [
        line_row(
            "p25p1",
            "p25p1",
            "P25 Phase 1 C4FM",
            12500,
            CLEAN,
            ["-f1"],
            lambda _w: p25p1_source(),
            P25P1_PASS,
            r"Sync: [+-]P25p1 .*LDU[12]",
        ),
        regen_row(
            "p25p1_regen",
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
            role=REGEN_CHECK,
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
            line_row(
                key,
                key,
                label,
                width,
                CLEAN,
                args,
                lambda _w, r=rate, o=outer: nxdn_source(r, o),
                pass_re,
                unit_re,
            )
        )
        rows.append(
            line_row(
                f"{key}_outer_sync",
                key,
                f"{label} (FSW at outer levels)",
                width,
                DIAGNOSTIC,
                args,
                lambda _w, r=rate, o=outer: nxdn_source(r, o, outer_sync=True),
                pass_re,
                unit_re,
            )
        )
        rows.append(
            regen_row(
                f"{key}_regen",
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
                REGEN_CHECK,
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
            line_row(
                f"ysf_{outer:.0f}",
                "ysf",
                f"YSF (outer {outer:.0f} Hz)",
                12500,
                CLEAN,
                ["-fy"],
                lambda _w, o=outer: ysf_source(o),
                YSF_PASS,
                r"Sync: [+-]YSF ",
            )
        )
    rows.append(
        line_row(
            "ysf_outer_sync",
            "ysf",
            "YSF (outer 2700 Hz, sync at outer levels)",
            12500,
            DIAGNOSTIC,
            ["-fy"],
            lambda _w: ysf_source(2700.0, outer_sync=True),
            YSF_PASS,
            r"Sync: [+-]YSF ",
        )
    )
    rows.append(
        regen_row(
            "ysf_regen",
            "ysf",
            "YSF (outer 2290 Hz)",
            12500,
            ["-fy"],
            "ysf",
            4800,
            lambda lv: fm_from_levels(lv, 4800, rrc_response(0.2, 4800), 2290.0),
            "RRC 0.2, outer 2290 Hz",
            YSF_PASS,
            r"Sync: [+-]YSF ",
            YSF_FS_LEVELS,
            REGEN_CHECK,
        )
    )
    rows.append(off_air_row("ysf_offair", "ysf", "YSF", 12500, ["-fy"], "ysf", YSF_PASS, r"Sync: [+-]YSF "))
    m17_pass, m17_unit = r"Sync: [+-]M17 STR LC: [1-6]/6", r"Sync: [+-]M17 STR"
    rows += [
        line_row("m17", "m17", "M17", 12500, CLEAN, ["-fz"], m17_source, m17_pass, m17_unit),
        regen_row(
            "m17_regen",
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
            role=REGEN_CHECK,
        ),
        off_air_row("m17_offair", "m17", "M17", 12500, ["-fz"], "m17", m17_pass, m17_unit),
    ]
    return rows


NARROW_NOTE = " (12.5 kHz channel's tone)"
WIDE_NOTE = " (25 kHz channel's tone)"


def nfm_row(key, mode, width_hz, deviation, label_note=""):
    return Row(
        key,
        mode,
        f"NFM {width_hz / 1000:g} kHz{label_note}",
        width_hz,
        CLEAN,
        ["-fA", "--nfm-bandwidth-hz", str(width_hz)],
        lambda _w, d=deviation: nfm_source(d),
        analog=True,
    )


def am_row(width_hz):
    key = f"am_{width_hz // 1000}k"
    return Row(
        key,
        key,
        f"AM {width_hz / 1000:g} kHz",
        width_hz,
        CLEAN,
        ["-fM", "--am-bandwidth-hz", str(width_hz)],
        lambda _w: am_source(),
        analog=True,
    )


def analog_rows():
    """NFM and AM at every width the Radio sheet offers (src/ui/qt/qml/Util.js NFM_WIDTHS_HZ, AM_WIDTHS_HZ), the scan
    check's calibrated widths: a 12.5 kHz channel's transmitter at 8 and 12.5 kHz, a 25 kHz channel's at 25 kHz, and
    both at 11.25, 16 and 20 kHz, where either is received, the lower threshold kept."""
    return [
        nfm_row("nfm_8k", "nfm_8k", 8000, NFM_NARROW_TONE_HZ),
        nfm_row("nfm_11k25_narrow", "nfm_11k25", 11250, NFM_NARROW_TONE_HZ, NARROW_NOTE),
        nfm_row("nfm_11k25_wide", "nfm_11k25", 11250, NFM_WIDE_TONE_HZ, WIDE_NOTE),
        nfm_row("nfm_12k5", "nfm_12k5", 12500, NFM_NARROW_TONE_HZ),
        nfm_row("nfm_16k_narrow", "nfm_16k", 16000, NFM_NARROW_TONE_HZ, NARROW_NOTE),
        nfm_row("nfm_16k_wide", "nfm_16k", 16000, NFM_WIDE_TONE_HZ, WIDE_NOTE),
        nfm_row("nfm_20k_narrow", "nfm_20k", 20000, NFM_NARROW_TONE_HZ, NARROW_NOTE),
        nfm_row("nfm_20k_wide", "nfm_20k", 20000, NFM_WIDE_TONE_HZ, WIDE_NOTE),
        nfm_row("nfm_25k", "nfm_25k", 25000, NFM_WIDE_TONE_HZ),
        am_row(5000),
        am_row(6000),
        am_row(8000),
        am_row(10000),
        am_row(15000),
        am_row(20000),
    ]


# ---------------------------------------------------------------- measurement


def band_power(source, width_hz):
    """Mean power of the signal inside +/- width/2; for a burst train, during its bursts (region B), not its ramps."""
    spectrum = np.abs(np.fft.fft(source.samples)) ** 2 / len(source.samples) ** 2
    freqs = np.fft.fftfreq(len(source.samples), 1.0 / RATE_HZ)
    power = float(spectrum[np.abs(freqs) <= width_hz / 2.0].sum())
    if source.on is None:
        return power
    in_band = power / float(np.mean(np.abs(source.samples) ** 2))
    return float(np.mean(np.abs(source.samples[source.on]) ** 2)) * in_band


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
    if source.sent is not None:
        # A generator knows what it sent: the criterion is 90 % of the frames sent, and the clean run must meet it.
        result.clean = {
            "passed": passed,
            "received": units,
            "sent": source.sent,
            "fraction": round(passed / source.sent, 3),
        }
        rows.append([row.key, "clean", "", passed, units, source.sent, f"{passed / source.sent:.3f}", ""])
        if passed < PASS_FRACTION * source.sent:
            result.status = "decoder-limited: re-measure after the slicer fix"
            result.detail = f"the clean signal passes {passed} of {source.sent} frames sent"
            return None
        return source.sent
    fraction = round(passed / units, 3) if units else 0.0
    result.clean = {"passed": passed, "received": units, "fraction": fraction}
    rows.append([row.key, "clean", "", passed, units, passed, "", ""])
    below = units == 0 or passed < PASS_FRACTION * units
    if passed == 0 or (below and row.role == OFF_AIR):
        result.status = "never reached"
        result.detail = f"the clean signal passes {passed} of {units} frames"
        return None
    if row.regenerated:
        result.detail = f"upper bound: baked-in symbol errors; the clean signal passes {passed} of {units} frames"
    elif below:
        result.detail = f"the clean signal passes {passed} of {units} frames"
    return passed


def measure_row(row, work_dir, rows, pool):
    result = Result(row.key, row.mode, row.label, row.width_hz, row.role, "")
    source = row.build(work_dir)
    if len(source.samples) != sample_count():
        raise SystemExit(f"{row.key}: the source holds {len(source.samples)} samples, not {sample_count()}")
    result.source = source.note
    if row.analog:
        result.metric = "12 dB SINAD, 1 kHz tone"
    elif source.sent is not None:
        result.metric = "frames passing CRC/FEC >= 90 % of frames sent"
    else:
        result.metric = "frames passing CRC/FEC >= 90 % of the clean run's passes"
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


def export_sources(work_dir):
    """Write each requested row's clean source for tests/io/signal_quiet_calibrate.cpp, which renders the scan check's
    calibration trials from the same signals: DIR/<row>.cf32 (complex64, little-endian, RATE_HZ, the signal at 0 Hz),
    DIR/<row>.on for a burst train (one byte per sample, 1 inside a burst's symbol transmission period), and one line
    per row in DIR/sources.tsv: row, mode, class width (Hz), sample rate (Hz), samples, and band_power() in that width,
    the power the S/N above is defined on."""
    os.makedirs(ARGS.export_sources, exist_ok=True)
    lines = ["row\tmode\twidth_hz\trate_hz\tsamples\tband_power\tburst\tsource"]
    for row in ARGS.rows:
        source = row.build(work_dir)
        stem = os.path.join(ARGS.export_sources, row.key)
        source.samples.astype("<c8").tofile(stem + ".cf32")
        if source.on is not None:
            source.on.astype(np.uint8).tofile(stem + ".on")
        burst = int(source.on is not None)
        power = band_power(source, row.width_hz)
        fields = [row.key, row.mode, row.width_hz, RATE_HZ, len(source.samples), f"{power:.9e}", burst, source.note]
        lines.append("\t".join(str(value) for value in fields))
    with open(os.path.join(ARGS.export_sources, "sources.tsv"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    return 0


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
    parser.add_argument("--out", help="directory for thresholds.json, thresholds.md and sweep.csv")
    parser.add_argument(
        "--export-sources",
        metavar="DIR",
        help="write the requested rows' clean sources to DIR for the scan check's calibration and measure nothing",
    )
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
    if not args.out and not args.export_sources:
        parser.error("give --out (measure) or --export-sources (export)")
    if not args.export_sources and any(row.analog for row in args.rows) and not args.analog_host:
        parser.error("the analog rows need --analog-host")
    if any(row.mode in ("dmr_bs", "dmr_ms") and row.role == CLEAN for row in args.rows) and not args.dmr_synth_gen:
        parser.error("dmr_bs and dmr_ms need --dmr-synth-gen")
    if any(row.mode == "dstar" for row in args.rows) and not args.mbelib:
        parser.error("dstar needs --mbelib (mbelib-neo's shared library was not found)")
    return args


def main():
    if ARGS.export_sources:
        with tempfile.TemporaryDirectory(prefix="scan-quiet-") as work_dir:
            return export_sources(work_dir)
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
