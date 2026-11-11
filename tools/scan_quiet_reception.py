#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure dsd-neo's own reception threshold per mode, for calibrating the scan skip-silent check (issue #834).

The skip check may call a channel quiet only well below the weakest signal dsd-neo still decodes. This tool finds that
weakest signal: for each mode it renders a 10 s signal, adds complex white Gaussian noise at a set S/N, replays the
noisy I/Q through the real decoder (--iq-replay, the path the DECODE_IQ_* tests use) and finds the lowest S/N, on a
0.5 dB grid, at which the decoder still meets the mode's criterion. The threshold of a mode is the median over the
noise seeds (five by default).

S/N is the signal power over the noise power inside the mode's class width (12.5 kHz or 6.25 kHz for digital modes,
the configured channel width for analog), both measured at the replayed 48 kHz baseband. The noise is fixed (its
power and the cu8 scale never change); the signal is scaled.

Digital criterion: at least 90 % of the frames the clean signal passes still pass CRC/FEC. Each mode counts one kind
of frame from dsd-neo's own output, a line it prints only when that frame's check passed:

  p25p1   LDU1 link control and LDU2 encryption sync, decoded to the transmitted values (RS)
  p25p2   SACCH MAC PDUs (RS + CRC), with the scrambling parameters hand-set (-X), so the threshold is the
          channel's and not that of recovering the scrambling code from one voice burst
  dmr_bs  bursts whose slot type or EMB decodes to the transmitted colour code (Golay / QR); dmr_bs_offair the same
          on the off-air DMR fixture (colour code 1), to show how much an off-air source moves a threshold
  nxdn96, nxdn48   frames whose SACCH passes CRC (dsd-neo prints the part number "PF n/4", "PF X/4" otherwise)
  dpmr    half-frames whose CCH verdict holds ("FN n/4" under -Z)
  ysf     frames whose FICH passes Golay and CRC
  m17     stream frames whose LICH passes Golay ("LC: n/6")

The clean run (the same signal with no added noise) fixes the denominator. A mode whose clean run passes fewer than
90 % of the frames dsd-neo received at all is reported as never reaching the criterion, and no sweep runs for it.

Analog criterion: 12 dB SINAD of a 1 kHz tone in dsd-neo's monitor audio (TIA-603 for NFM, ETSI EN 300 676-1 for
AM). NFM carries the tone at 60 % of the maximum deviation (1.5 kHz on a 12.5 kHz channel, 3 kHz on a 25 kHz one),
AM at 30 % depth. The audio is scored by dsd-neo_test_analog_replay (tests/engine/analog_replay.c), which runs the
real engine and fits the tone to every delivered 20 ms block after the monitor's filters: SINAD is
10 log10(1 + S/(N+D)) from its tone_snr_db. Gain is fixed (-n 50) and the replay squelch is the -110 dB default,
which passes everything; noise alone must fail the metric, and the tool reports that it does.

Signals come from tools/build_iq_fixtures.py and its committed output, never from a new modulator:

  dmr_bs   the DMR test generator (tests/protocol/dmr/dmr_synth.c, --dmr-synth-gen): slot 1 a clear voice call,
           slot 2 idle, remodulated with build_iq_fixtures.remodulate() at the ETSI outer deviation of 1944 Hz
  dpmr     build_iq_fixtures.modulate_4fsk() over the dPMR CCH reference vectors (dpmr_symbol_stream())
  nfm, am  build_iq_fixtures.nfm_synth_samples() / am_synth_samples() at this tool's deviation, depth and length
  others   a committed fixture (tests/fixtures/iq) cut to its carrier (the off-air clips start before key-up) and
           repeated to 10 s. Off-air I/Q (p25p1_c4fm_vc, nxdn96, nxdn48) is used as recorded. Fixtures remodulated
           from discriminator audio (p25p2_cc, dmr_voice, ysf, m17) were built at a deviation chosen for decoding,
           not the standard's; their phase is rescaled so their outer symbol level (outer_deviation_hz()) sits at the
           mode's nominal deviation (SOURCE_DEVIATION below). An off-air source carries its own impairments, so its
           threshold is an upper bound of a clean signal's: dmr_bs_offair shows by how much for DMR.

Two modes are not measured (NOT_MEASURED). DMR MS: no generator writes MS bursts (MS syncs, no CACH), and the BS
signal cut to one slot, with or without its CACH, is not a substitute: dsd-neo's BS path decodes 77 of its 166 bursts
with no noise at all.
D-STAR: dsd-neo prints no per-frame CRC/FEC verdict for D-STAR voice, the only source is an MP3 of a receiver's audio
whose clean decode already has AMBE FEC errors and a garbled header, and build_iq_fixtures.py has no D-STAR generator.

Everything is deterministic: the noise seeds are fixed (--base-seed), the generator runs with fixed arguments, and
dsd-neo replays at --iq-replay-rate fast.

Usage:
    python3 tools/scan_quiet_reception.py --dsd build/dev-release/apps/dsd-cli/dsd-neo \\
        --analog-host build/dev-debug/tests/dsd-neo_test_analog_replay \\
        --dmr-synth-gen build/dev-debug/tests/dsd-neo_test_dmr_synth_gen --out OUT_DIR [--modes LIST] [--jobs N]

It writes OUT_DIR/thresholds.json, OUT_DIR/thresholds.md (mode, class width, threshold S/N dB, seeds) and
OUT_DIR/sweep.csv (every run). Exit status 0 when every requested mode was measured or reported, 1 when a decoder
run fails or a capture would clip, 2 on a usage error.
"""

import argparse
import concurrent.futures
import contextlib
import csv
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
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

# Nominal outer symbol deviation (Hz) of the sources remodulated from discriminator audio. Their present outer level
# is estimated by outer_deviation_hz(), which reads 1805 Hz on the off-air C4FM I/Q (nominal 1800 Hz) and 1054 Hz on
# the dPMR synthesis (1050 Hz).
#   p25p2_cc  2250 Hz: H-DQPSK's +/-3 pi/4 symbol phase step at 6000 symbol/s, the FM view of its outer symbol
#   ysf       2290 Hz: Yaesu publishes none; OP25's transmitter (dv_tx.py mod_adjust) sets YSF at 0.42/0.33 of
#             C4FM's 1800 Hz
#   m17       2400 Hz: M17 specification section 1.2 (h = 1/3, +/-800 and +/-2400 Hz)
#   dmr_voice 1944 Hz: ETSI TS 102 361-1 section 10.2.1 (DMR_OUTER_HZ)
SOURCE_DEVIATION = {"p25p2_cc": 2250.0, "ysf": 2290.0, "m17": 2400.0, "dmr_voice": 1944.0}
OUTER_WINDOW = 4  # samples averaged (under half a 4800 symbol/s symbol at 48 kHz)
CARRIER_ON_FRACTION = 0.1  # a 10 ms window is carrier when its power exceeds this share of the clip's 90th percentile

DMR_OUTER_HZ = SOURCE_DEVIATION["dmr_voice"]  # ETSI TS 102 361-1 section 10.2.1: outer symbol at +/-1.944 kHz
DMR_SYNTH_OUTER = 9000.0  # dmr_synth.c writes the +/-3 level at 3 x 3000 in its s16 WAV
NFM_TONE_DEVIATION_HZ = {12500: 1500.0, 25000: 3000.0}  # 60 % of +/-2.5 kHz and +/-5 kHz
AM_TONE_DEPTH = 0.3


@dataclass
class Source:
    samples: np.ndarray
    note: str = ""


@dataclass
class DigitalMode:
    key: str
    label: str
    width_hz: int
    args: list
    pass_re: str
    unit_re: str
    unit_weight: int = 1


@dataclass
class AnalogMode:
    key: str
    label: str
    width_hz: int
    args: list


@dataclass
class Result:
    key: str
    label: str
    width_hz: int
    metric: str
    source: str
    status: str = "measured"
    seeds: list = field(default_factory=list)
    threshold_db: float = None
    clean: dict = field(default_factory=dict)
    detail: str = ""


DIGITAL = [
    DigitalMode(
        "p25p1",
        "P25 Phase 1 C4FM",
        12500,
        ["-f1"],
        r"Group Voice Channel User - Group 1 Source 1|LDU2 ALG ID: 0x80 KEY ID: 0x0000 MI: 0x0000000000000000",
        r"Sync: [+-]P25p1 .*LDU[12]",
    ),
    DigitalMode("p25p2", "P25 Phase 2", 12500, ["-f2", "-X", "BEE00164161"], r"P25p2 SACCH +MAC_", r"P25p2 SACCH"),
    DigitalMode("dmr_bs", "DMR BS", 12500, ["-fs"], r"Sync: [+-]DMR .*Color Code=02 \|", r"Sync: [+-]?DMR "),
    DigitalMode(
        "dmr_bs_offair",
        "DMR BS (off-air source)",
        12500,
        ["-fs"],
        r"Sync: [+-]DMR .*Color Code=01 \|",
        r"Sync: [+-]?DMR ",
    ),
    DigitalMode("nxdn96", "NXDN96", 12500, ["-fn"], r"Sync: NXDN96 .*PF [1-4]/4", r"Sync: NXDN96 "),
    DigitalMode("nxdn48", "NXDN48", 6250, ["-fi"], r"Sync: NXDN48 .*PF [1-4]/4", r"Sync: NXDN48 "),
    DigitalMode("dpmr", "dPMR", 6250, ["-fm", "-Z"], r"^ FN [1-4]/4", r"Sync: [+-]dPMR ", unit_weight=2),
    DigitalMode("ysf", "YSF", 12500, ["-fy"], r"Sync: [+-]YSF (?!.*FICH \((?:CRC|FEC) ERR\))", r"Sync: [+-]YSF "),
    DigitalMode("m17", "M17", 12500, ["-fz"], r"Sync: [+-]M17 STR LC: [1-6]/6", r"Sync: [+-]M17 STR"),
]

ANALOG = [
    AnalogMode("nfm_12k5", "NFM 12.5 kHz", 12500, ["-fA", "--nfm-bandwidth-hz", "12500"]),
    AnalogMode("nfm_25k", "NFM 25 kHz", 25000, ["-fA", "--nfm-bandwidth-hz", "25000"]),
    AnalogMode("am_6k", "AM 6 kHz", 6000, ["-fM", "--am-bandwidth-hz", "6000"]),
    AnalogMode("am_20k", "AM 20 kHz", 20000, ["-fM", "--am-bandwidth-hz", "20000"]),
]

NOT_MEASURED = {
    "dmr_ms": (
        "DMR MS",
        12500,
        (
            "no generator writes MS bursts (MS syncs, no CACH); slot 1 of the BS signal alone, with or without its "
            "CACH, decodes 77 of 166 bursts through the BS path with no noise added, so it is no substitute"
        ),
    ),
    "dstar": (
        "D-STAR",
        6250,
        (
            "no per-frame CRC/FEC verdict in dsd-neo's D-STAR voice output, no generator in build_iq_fixtures.py, "
            "and the off-air source decodes with AMBE FEC errors and a garbled header even without added noise"
        ),
    ),
}


def sample_count():
    return round(DURATION_S * RATE_HZ)


def repeat_to_length(samples):
    n = sample_count()
    return np.tile(samples, n // len(samples) + 1)[:n]


def outer_deviation_hz(samples):
    """Outer symbol level of a 4-level FM signal: the 75th percentile of |instantaneous frequency|, averaged over
    OUTER_WINDOW samples; with equiprobable symbols half sit on the outer levels."""
    freq = np.angle(samples[1:] * np.conj(samples[:-1])) * RATE_HZ / (2.0 * math.pi)
    smooth = np.convolve(freq, np.ones(OUTER_WINDOW) / OUTER_WINDOW, mode="valid")
    return float(np.percentile(np.abs(smooth - np.median(smooth)), 75))


def carrier_span(samples):
    """The fixture from its carrier's first to its last 10 ms: the off-air clips start (and some end) before the
    transmitter keys up, and that silence is neither signal power nor frames."""
    window = RATE_HZ // 100
    power = np.convolve(np.abs(samples) ** 2, np.ones(window) / window, mode="same")
    on = np.flatnonzero(power > CARRIER_ON_FRACTION * np.percentile(power, 90))
    return samples[on[0] : on[-1] + 1]


def fixture_source(name):
    """A committed fixture's carrier, DC removed, at its mode's nominal deviation when remodulated, repeated to the
    length."""
    samples = bif.load_cu8_fixture(os.path.join(FIXTURE_DIR, name + ".iq"))
    samples = carrier_span(samples - np.mean(samples))
    note = f"committed fixture {name} ({len(samples) / RATE_HZ:.2f} s carrier), repeated to {DURATION_S:g} s"
    if name in SOURCE_DEVIATION:
        measured, nominal = outer_deviation_hz(samples), SOURCE_DEVIATION[name]
        samples = np.exp(1j * (nominal / measured) * np.unwrap(np.angle(samples)))
        note += f", deviation rescaled {measured:.0f} -> {nominal:.0f} Hz"
    return Source(repeat_to_length(samples), note=note)


def dmr_synth_audio(generator, work_dir):
    """The DMR test generator's discriminator audio: slot 1 a clear voice call, slot 2 idle, no noise of its own."""
    wav_path = os.path.join(work_dir, "dmr_synth.wav")
    if os.path.exists(wav_path):
        os.remove(wav_path)
    args = ["-o", "dmr_synth.wav", "-t", f"{DURATION_S:g}", "--cc", "2", "--s1", "voice", "--s2", "idle"]
    subprocess.run([os.path.abspath(generator)] + args + ["--snr", "200", "--seed", "7"], cwd=work_dir, check=True)
    with wave.open(wav_path, "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getframerate() != RATE_HZ:
            raise SystemExit(f"{wav_path}: expected 48 kHz mono s16 from the DMR generator")
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)
    return audio


def dmr_source(generator, work_dir):
    audio = dmr_synth_audio(generator, work_dir)
    # remodulate() maps the 99.5th percentile of |audio - mean| to its deviation; set that so the +/-3 level lands on
    # the standard's outer deviation.
    scale = np.percentile(np.abs(audio - np.mean(audio)), 99.5)
    deviation = DMR_OUTER_HZ * scale / DMR_SYNTH_OUTER / (RATE_HZ / 2.0)
    samples = repeat_to_length(bif.remodulate(audio, deviation))
    return Source(samples, note=f"dmr_synth.c, slot 1 voice, slot 2 idle, outer deviation {DMR_OUTER_HZ:.0f} Hz")


def dpmr_source():
    vectors = bif.load_dpmr_reference_vectors()
    stream = bif.dpmr_symbol_stream(vectors)
    per_copy_s = len(stream) / bif.DPMR_SYNTH_SYMBOL_RATE
    dibits = stream * math.ceil(DURATION_S / per_copy_s)
    samples = bif.modulate_4fsk(dibits, np.random.default_rng(bif.DPMR_SYNTH_SEED))[: sample_count()]
    note = "modulate_4fsk() over the dPMR CCH reference vectors (its own noise 40 dB below the signal in 6.25 kHz)"
    return Source(samples, note=note)


@contextlib.contextmanager
def builder_constants(**values):
    """Run the fixture builder's tone synthesizers with this tool's tone, length and no noise of their own."""
    saved = {name: getattr(bif, name) for name in values}
    for name, value in values.items():
        setattr(bif, name, value)
    try:
        yield
    finally:
        for name, value in saved.items():
            setattr(bif, name, value)


def analog_source(mode):
    if mode.key.startswith("nfm"):
        deviation = NFM_TONE_DEVIATION_HZ[mode.width_hz]
        with builder_constants(
            NFM_SYNTH_DURATION_S=DURATION_S, NFM_SYNTH_DEVIATION_HZ=deviation, NFM_SYNTH_NOISE_DB=-math.inf
        ):
            samples = bif.nfm_synth_samples(0, None, None)
        return Source(samples, note=f"nfm_synth_samples(): 1 kHz tone at {deviation:.0f} Hz deviation")
    with builder_constants(AM_SYNTH_DURATION_S=DURATION_S, AM_SYNTH_DEPTH=AM_TONE_DEPTH, AM_SYNTH_NOISE_DB=-math.inf):
        samples = bif.am_synth_samples(0, None, None)
    return Source(samples, note=f"am_synth_samples(): 1 kHz tone at {AM_TONE_DEPTH:.0%} depth")


def band_power(source, width_hz):
    """Mean power of the signal inside +/- width/2."""
    spectrum = np.abs(np.fft.fft(source.samples)) ** 2 / len(source.samples) ** 2
    freqs = np.fft.fftfreq(len(source.samples), 1.0 / RATE_HZ)
    return float(spectrum[np.abs(freqs) <= width_hz / 2.0].sum())


def noise_for(key, seed):
    rng = np.random.default_rng([ARGS.base_seed, zlib.crc32(key.encode("ascii")), seed])
    n = sample_count()
    return rng.normal(0.0, NOISE_SIGMA, n) + 1j * rng.normal(0.0, NOISE_SIGMA, n)


def render(source, width_hz, noise, snr_db, path_stem):
    """Write source at snr_db over noise (no noise for snr_db None) as a cu8 capture; return its sidecar path."""
    noise_in_band = 2.0 * NOISE_SIGMA**2 * width_hz / RATE_HZ
    if snr_db is None:
        level = math.sqrt(10.0 ** (START_DB / 10.0) * noise_in_band / band_power(source, width_hz))
        samples = level * source.samples
    else:
        level = math.sqrt(10.0 ** (snr_db / 10.0) * noise_in_band / band_power(source, width_hz))
        samples = level * source.samples + noise
    peak = np.maximum(np.abs(samples.real), np.abs(samples.imag)) * 0.9  # to_cu8's headroom without normalizing
    clipped = int(np.count_nonzero(peak > 1.0))
    if clipped > len(samples) // 1000:
        raise SystemExit(f"{path_stem}: {clipped} samples clip at {snr_db} dB; lower START_DB or NOISE_SIGMA")
    out_dir, name = os.path.split(path_stem)
    bif.write_fixture(out_dir, name, samples, normalize=False, notes="scan_quiet_reception.py")
    return path_stem + ".iq.json"


def run_text(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=300, check=False)
    if proc.returncode != 0:
        raise SystemExit(f"{' '.join(cmd)} exited {proc.returncode}:\n{proc.stdout[-2000:]}{proc.stderr[-2000:]}")
    return ANSI.sub("", proc.stdout + proc.stderr)


def digital_counts(mode, sidecar):
    text = run_text([ARGS.dsd, "--frontend", "none"] + mode.args + ["--iq-replay", sidecar, "-o", "null"])
    passed = len(re.findall(mode.pass_re, text, flags=re.MULTILINE))
    units = len(re.findall(mode.unit_re, text, flags=re.MULTILINE)) * mode.unit_weight
    return passed, units


def analog_sinad(mode, sidecar):
    cmd = [ARGS.analog_host, "--frontend", "none"] + mode.args + ["-n", "50", "--iq-replay", sidecar, "-o", "null"]
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


def seed_threshold_digital(mode, source, reference, seed, work_dir, rows):
    noise = noise_for(mode.key, seed)

    def measure(snr):
        stem = os.path.join(work_dir, f"{mode.key}_s{seed}_{snr:+05.1f}")
        passed, units = digital_counts(mode, render(source, mode.width_hz, noise, snr, stem))
        for suffix in (".iq", ".iq.json"):
            os.remove(stem + suffix)
        return passed, units

    def record(snr, counts):
        passed, units = counts
        ratio = passed / reference
        rows.append([mode.key, seed, f"{snr:.1f}", passed, units, reference, f"{ratio:.3f}", ""])
        return ratio >= PASS_FRACTION

    return descend(measure, record)


def seed_threshold_analog(mode, source, seed, work_dir, rows):
    noise = noise_for(mode.key, seed)

    def measure(snr):
        stem = os.path.join(work_dir, f"{mode.key}_s{seed}_{snr:+05.1f}")
        sinad = analog_sinad(mode, render(source, mode.width_hz, noise, snr, stem))
        for suffix in (".iq", ".iq.json"):
            os.remove(stem + suffix)
        return sinad

    def record(snr, measured):
        sinad, captured_ms = measured
        rows.append([mode.key, seed, f"{snr:.1f}", "", "", "", f"{sinad:.2f}", f"{captured_ms:.0f}"])
        return sinad >= SINAD_TARGET_DB

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


def measure_digital(mode, source, work_dir, rows, pool):
    result = Result(mode.key, mode.label, mode.width_hz, "frames passing CRC/FEC >= 90 % of clean", source.note)
    clean = render(source, mode.width_hz, None, None, os.path.join(work_dir, f"{mode.key}_clean"))
    passed, units = digital_counts(mode, clean)
    result.clean = {"passed": passed, "received": units, "fraction": round(passed / units, 3) if units else 0.0}
    rows.append([mode.key, "clean", "", passed, units, passed, "", ""])
    if units == 0 or passed < PASS_FRACTION * units:
        result.status = "never reached"
        result.detail = f"the clean signal passes {passed} of {units} received frames"
        return result
    futures = [
        pool.submit(seed_threshold_digital, mode, source, passed, seed, work_dir, rows) for seed in range(ARGS.seeds)
    ]
    summarize(result, [future.result() for future in futures])
    return result


def measure_analog(mode, source, work_dir, rows, pool):
    result = Result(mode.key, mode.label, mode.width_hz, "12 dB SINAD, 1 kHz tone", source.note)
    stem = os.path.join(work_dir, f"{mode.key}_noise")
    bif.write_fixture(work_dir, os.path.basename(stem), noise_for(mode.key, 0), normalize=False)
    sinad, captured_ms = analog_sinad(mode, stem + ".iq.json")
    result.clean = {"noise_only_sinad_db": round(sinad, 2), "noise_only_captured_ms": captured_ms}
    rows.append([mode.key, "noise", "", "", "", "", f"{sinad:.2f}", f"{captured_ms:.0f}"])
    if sinad >= SINAD_TARGET_DB:
        result.status = "invalid"
        result.detail = f"noise alone reaches {sinad:.1f} dB SINAD"
        return result
    futures = [pool.submit(seed_threshold_analog, mode, source, seed, work_dir, rows) for seed in range(ARGS.seeds)]
    summarize(result, [future.result() for future in futures])
    return result


def write_reports(results, rows):
    with open(os.path.join(ARGS.out, "sweep.csv"), "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["mode", "seed", "snr_db", "passed", "received", "reference", "ratio_or_sinad_db", "audio_ms"])
        # Runs finish in any order; sort so a rerun writes the same file.
        writer.writerows(sorted(rows, key=lambda row: [str(cell).zfill(8) for cell in row[:3]]))
    payload = {
        "tool": "tools/scan_quiet_reception.py",
        "dsd": ARGS.dsd,
        "analog_host": ARGS.analog_host,
        "duration_s": DURATION_S,
        "noise_sigma": NOISE_SIGMA,
        "base_seed": ARGS.base_seed,
        "seeds": ARGS.seeds,
        "step_db": STEP_DB,
        "modes": [vars(result) for result in results],
    }
    with open(os.path.join(ARGS.out, "thresholds.json"), "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")
    lines = [
        "| mode | class width | threshold S/N dB | seeds | metric | notes |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for result in results:
        threshold = "n/a" if result.threshold_db is None else f"{result.threshold_db:.1f}"
        seeds = ", ".join("n/a" if value is None else f"{value:.1f}" for value in result.seeds) or "n/a"
        notes = "; ".join(part for part in (result.status, result.detail, result.source) if part)
        lines.append(
            f"| {result.label} | {result.width_hz / 1000:g} kHz | {threshold} | {seeds} | {result.metric} | {notes} |"
        )
    with open(os.path.join(ARGS.out, "thresholds.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("\n".join(lines))


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    names = [mode.key for mode in DIGITAL] + [mode.key for mode in ANALOG] + list(NOT_MEASURED)
    parser.add_argument("--dsd", required=True, help="the dsd-neo binary (a Release build runs the sweep fastest)")
    parser.add_argument("--analog-host", help="dsd-neo_test_analog_replay, needed for the analog modes")
    parser.add_argument("--dmr-synth-gen", help="dsd-neo_test_dmr_synth_gen, needed for dmr_bs")
    parser.add_argument("--out", required=True, help="directory for thresholds.json, thresholds.md and sweep.csv")
    parser.add_argument("--modes", default=",".join(names), help=f"comma-separated subset of: {', '.join(names)}")
    parser.add_argument("--seeds", type=int, default=5, help="noise seeds per mode (default 5)")
    parser.add_argument("--base-seed", type=int, default=834, help="noise seed base (default 834)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="decoder runs in parallel")
    args = parser.parse_args()
    args.modes = [name.strip() for name in args.modes.split(",") if name.strip()]
    unknown = sorted(set(args.modes) - set(names))
    if unknown:
        parser.error(f"unknown mode(s): {', '.join(unknown)}")
    if args.seeds < 1:
        parser.error("--seeds must be at least 1")
    if any(mode.key in args.modes for mode in ANALOG) and not args.analog_host:
        parser.error("the analog modes need --analog-host")
    if "dmr_bs" in args.modes and not args.dmr_synth_gen:
        parser.error("dmr_bs needs --dmr-synth-gen")
    return args


def build_source(key, work_dir):
    if key == "dmr_bs":
        return dmr_source(ARGS.dmr_synth_gen, work_dir)
    if key == "dpmr":
        return dpmr_source()
    return fixture_source({"p25p1": "p25p1_c4fm_vc", "p25p2": "p25p2_cc", "dmr_bs_offair": "dmr_voice"}.get(key, key))


def main():
    os.makedirs(ARGS.out, exist_ok=True)
    rows = []
    results = []
    # One thread per mode runs its clean check and waits on its seeds, which run in the pool.
    with (
        tempfile.TemporaryDirectory(prefix="scan-quiet-") as work_dir,
        concurrent.futures.ThreadPoolExecutor(max_workers=max(1, ARGS.jobs)) as pool,
        concurrent.futures.ThreadPoolExecutor(max_workers=len(ARGS.modes)) as modes,
    ):
        pending = []
        for mode in DIGITAL:
            if mode.key in ARGS.modes:
                source = build_source(mode.key, work_dir)
                pending.append(modes.submit(measure_digital, mode, source, work_dir, rows, pool))
        for mode in ANALOG:
            if mode.key in ARGS.modes:
                pending.append(modes.submit(measure_analog, mode, analog_source(mode), work_dir, rows, pool))
        results = [future.result() for future in pending]
    for key in ARGS.modes:
        if key in NOT_MEASURED:
            label, width, why = NOT_MEASURED[key]
            results.append(Result(key, label, width, "not measured", "", status="not measured", detail=why))
    write_reports(results, rows)
    return 0


if __name__ == "__main__":
    ARGS = parse_args()
    sys.exit(main())
