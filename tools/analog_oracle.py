#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Label the CTCSS tone or DCS code carried by NFM I/Q fixtures, independently of dsd-neo.

The off-air analog excerpts in tests/fixtures/iq come unlabelled, and the C tone and code
detectors must not grade themselves. This oracle shares no code with them: it demodulates
the fixture with numpy (brick-wall channel filter, phase-difference discriminator), then

  CTCSS  takes one zero-padded, Hann-windowed FFT of the whole excerpt's sub-audible band
         and accepts its strongest line only when it stands clear of everything else in
         60-260 Hz and lies within 0.5% of exactly one tone of the 50-tone EIA table;
  DCS    low-passes the discriminator to 300 Hz, slices it at 134.4 bit/s at sixteen bit
         phases and both polarities, and checks every 23-bit window against its own
         Golay (23,12) encoder and the DCS word layout, accepting a code only when it
         repeats through most of the excerpt.

When neither is found it reports the sub-audible bit rate and repetition period it can
measure, so a non-DCS signalling scheme is identified as such rather than mislabelled.

DCS word layout, as cited by https://yo3iiu.ro/blog/?p=753 and the Signal Identification
Wiki's DCS page: 12 data bits (the 9-bit octal code, then a fixed 1 0 0 marker as the
three high data bits) followed by 11 check bits, sent least significant bit first at
134.4 bit/s. With bit i of the word as the coefficient of x^i -- bit 0 is the first sent
-- a word is a codeword when it is divisible by g(x) = x^11 + x^10 + x^6 + x^5 + x^4 +
x^2 + 1. --self-test checks that this reproduces the cited word for code 023
(11101100011100000010011, check bits first as written) and its cited aliases (the
rotations 340 and 766; 047 when the bits are inverted).

Polarity: a reading takes positive deviation as a 1 and is labelled N; the inverted
reading is labelled I. Inverting a DCS word yields another valid word, so every waveform
carries both an N and an I label (023N and 047I are the same signal); both are printed.
Which of them a given radio calls "normal" still needs checking against a transmitter with
a known code.

Offline and numpy-only; CI does not run it.

Usage:
    python3 tools/analog_oracle.py [--offset-hz HZ] [fixture.iq.json ...]   (default: the NFM fixtures)
    python3 tools/analog_oracle.py --self-test

--offset-hz labels the channel HZ away from the centre instead, for example the
neighbour kept 12.5 kHz below nfm_ctcss_real's channel (--offset-hz -12500).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np

CHANNEL_HALF_WIDTH_HZ = 7500.0
SUBAUDIBLE_LO_HZ = 60.0
SUBAUDIBLE_HI_HZ = 260.0
SUBAUDIBLE_LPF_HZ = 300.0
CTCSS_TOLERANCE = 0.005
CTCSS_MIN_PROMINENCE_DB = 10.0
CTCSS_MIN_FLOOR_DB = 20.0

# The 50-tone EIA table (Hz). 150.0 Hz is deliberately absent.
CTCSS_TONES = (
    67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8,
    118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8,
    177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6,
    241.8, 250.3, 254.1,
)

DCS_BIT_RATE = 134.4
DCS_WORD_BITS = 23
DCS_GENERATOR = 0xC75  # x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1
DCS_MARKER = 0b100  # word bits 11..9
DCS_PHASES = 16
DCS_MIN_WORDS = 3
DATA_MAX_FIT = 0.08  # mean distance of zero-crossing intervals from whole bits
DATA_MIN_PERIOD_CORR = 0.8

# The standard 104 DCS codes (octal).
DCS_CODES = tuple(int(code, 8) for code in (
    "023 025 026 031 032 036 043 047 051 053 054 065 071 072 073 074 114 115 116 122 125 131 132 134 143 145 "
    "152 155 156 162 165 172 174 205 212 223 225 226 243 244 245 246 251 252 255 261 263 265 266 271 274 306 "
    "311 315 325 331 332 343 346 351 356 364 365 371 411 412 413 423 431 432 445 446 452 454 455 462 464 465 "
    "466 503 506 516 523 526 532 546 565 606 612 624 627 631 632 654 662 664 703 712 723 731 732 734 743 754"
).split())

DEFAULT_FIXTURES = ("nfm_ctcss_real", "nfm_dcs_real_a", "nfm_dcs_real_b", "nfm_tone_synth", "nfm_adjacent_synth")


# ---- demodulation -------------------------------------------------------------------------------------------------

def load_fixture(path):
    """Read a dsd-neo-iq cu8 fixture: returns (complex samples, sample rate)."""
    with open(path, encoding="utf-8") as handle:
        meta = json.load(handle)
    if meta.get("sample_format") != "cu8":
        raise SystemExit(f"{path}: only cu8 fixtures are supported, not {meta.get('sample_format')}")
    data = os.path.join(os.path.dirname(os.path.abspath(path)), meta["data_file"])
    raw = (np.fromfile(data, dtype=np.uint8).astype(np.float64) - 127.5) / 127.5
    return raw[0::2] + 1j * raw[1::2], float(meta["sample_rate_hz"])


def band_limit(signal, rate_hz, lo_hz, hi_hz):
    """Keep |f| within [lo_hz, hi_hz] by zeroing FFT bins (both sides for real input)."""
    spectrum = np.fft.fft(signal)
    freq = np.abs(np.fft.fftfreq(len(signal), 1.0 / rate_hz))
    spectrum[(freq < lo_hz) | (freq > hi_hz)] = 0.0
    out = np.fft.ifft(spectrum)
    return np.real(out) if np.isrealobj(signal) else out


def discriminate(samples, rate_hz, offset_hz=0.0):
    """Instantaneous frequency in Hz of the channel at offset_hz."""
    if offset_hz:
        samples = samples * np.exp(-2j * math.pi * offset_hz * np.arange(len(samples)) / rate_hz)
    channel = band_limit(samples, rate_hz, 0.0, CHANNEL_HALF_WIDTH_HZ)
    return np.angle(channel[1:] * np.conj(channel[:-1])) * rate_hz / (2.0 * math.pi)


# ---- CTCSS --------------------------------------------------------------------------------------------------------

def ctcss_label(freq_hz, rate_hz):
    """Return a dict describing the CTCSS verdict for discriminator output freq_hz."""
    audio = freq_hz - np.mean(freq_hz)
    window = np.hanning(len(audio))
    size = 1 << int(math.ceil(math.log2(len(audio) * 8)))
    spectrum = np.abs(np.fft.rfft(audio * window, size))
    axis = np.fft.rfftfreq(size, 1.0 / rate_hz)
    band = (axis >= SUBAUDIBLE_LO_HZ) & (axis <= SUBAUDIBLE_HI_HZ)
    power = spectrum[band] ** 2
    freqs = axis[band]
    peak = int(np.argmax(power))
    peak_hz = float(freqs[peak])
    others = power[np.abs(freqs - peak_hz) > 2.0]
    prominence = 10.0 * math.log10(power[peak] / max(float(np.max(others)), 1e-30))
    floor = 10.0 * math.log10(power[peak] / max(float(np.median(power)), 1e-30))
    deviation = 2.0 * spectrum[band][peak] / float(np.sum(window))
    matches = [tone for tone in CTCSS_TONES if abs(peak_hz - tone) <= CTCSS_TOLERANCE * tone]
    result = {"peak_hz": peak_hz, "prominence_db": prominence, "floor_db": floor, "deviation_hz": deviation}
    if prominence < CTCSS_MIN_PROMINENCE_DB or floor < CTCSS_MIN_FLOOR_DB:
        result["label"] = None
        result["why"] = "no line stands clear of the sub-audible band"
    elif len(matches) != 1:
        result["label"] = None
        near_150 = abs(peak_hz - 150.0) <= CTCSS_TOLERANCE * 150.0
        result["why"] = "150.0 Hz is not an EIA tone" if near_150 else "line matches no EIA tone"
    else:
        result["label"] = f"{matches[0]:.1f}"
    return result


# ---- DCS ----------------------------------------------------------------------------------------------------------

def golay_remainder(word):
    """word mod g(x), with bit i of word as the coefficient of x^i."""
    degree = DCS_GENERATOR.bit_length() - 1
    while word.bit_length() - 1 >= degree:
        word ^= DCS_GENERATOR << (word.bit_length() - 1 - degree)
    return word


def dcs_encode(code):
    """23-bit DCS word for a 9-bit code: the 11 check bits above the marker and the code.

    Exactly one of the 2^11 check-bit patterns makes the word divisible by g(x); searching for
    it keeps the encoder a direct restatement of the definition.
    """
    data = (DCS_MARKER << 9) | code
    for check in range(1 << 11):
        word = (check << 12) | data
        if golay_remainder(word) == 0:
            return word
    raise ValueError(f"no Golay completion for data {data:#05x}")


def dcs_code_of(word):
    """The 9-bit code if word is a valid DCS word, else None."""
    if golay_remainder(word) != 0 or ((word >> 9) & 0b111) != DCS_MARKER:
        return None
    return word & 0x1FF


def rotate(word, count):
    count %= DCS_WORD_BITS
    return ((word >> count) | (word << (DCS_WORD_BITS - count))) & ((1 << DCS_WORD_BITS) - 1)


def slice_bits(subaudible, rate_hz, bit_rate, phase):
    """Sign of the sub-audible signal at the centre of each bit, starting phase/DCS_PHASES of a bit in."""
    per_bit = rate_hz / bit_rate
    count = int((len(subaudible) / per_bit) - 1)
    centres = ((np.arange(count) + 0.5 + (phase / DCS_PHASES)) * per_bit).astype(int)
    return (subaudible[centres] > 0.0).astype(np.int64)


def dcs_counts(bits):
    """{code: hits} over every alignment of consecutive 23-bit words, plus the most words any alignment held."""
    counts = {}
    most = 0
    for offset in range(DCS_WORD_BITS):
        words = (len(bits) - offset) // DCS_WORD_BITS
        most = max(most, words)
        for index in range(words):
            chunk = bits[offset + index * DCS_WORD_BITS: offset + (index + 1) * DCS_WORD_BITS]
            word = int(np.sum(chunk.astype(np.int64) << np.arange(DCS_WORD_BITS)))
            code = dcs_code_of(word)
            if code is not None:
                counts[code] = counts.get(code, 0) + 1
    return counts, most


def dcs_label(freq_hz, rate_hz):
    """Return a dict describing the DCS verdict for discriminator output freq_hz."""
    subaudible = band_limit(freq_hz - np.mean(freq_hz), rate_hz, 0.0, SUBAUDIBLE_LPF_HZ)
    best = {}
    words_available = 0
    for phase in range(DCS_PHASES):
        bits = slice_bits(subaudible, rate_hz, DCS_BIT_RATE, phase)
        for polarity, stream in (("N", bits), ("I", 1 - bits)):
            counts, most = dcs_counts(stream)
            words_available = max(words_available, most)
            for code, hits in counts.items():
                key = (code, polarity)
                best[key] = max(best.get(key, 0), hits)
    needed = max(DCS_MIN_WORDS, words_available // 2)
    solid = {key: hits for key, hits in best.items() if hits >= needed}
    standard = sorted((key for key in solid if key[0] in DCS_CODES), key=lambda k: (-solid[k], k[1], k[0]))
    result = {"words": words_available, "needed": needed,
              "readings": sorted(f"D{code:03o}{pol}:{hits}" for (code, pol), hits in solid.items())}
    if not standard:
        result["label"] = None
        result["why"] = "no DCS word repeats" if not solid else "only non-standard aliases repeat"
        return result
    result["label"] = " = ".join(f"D{code:03o}{pol}" for code, pol in standard)
    return result


def data_signature(freq_hz, rate_hz):
    """Bit rate and repetition period of whatever NRZ data sits in the sub-audible band."""
    subaudible = band_limit(freq_hz - np.mean(freq_hz), rate_hz, 0.0, SUBAUDIBLE_LPF_HZ)
    crossings = np.nonzero(np.diff(np.sign(subaudible)))[0]
    if len(crossings) < 20:
        return None
    intervals = np.diff(crossings) / rate_hz
    rates = np.arange(100.0, 200.05, 0.1)
    scores = []
    for rate in rates:
        units = intervals * rate
        units = units[units > 0.5]
        scores.append(float(np.mean(np.abs(units - np.round(units)))) if len(units) else 1.0)
    bit_rate = float(rates[int(np.argmin(scores))])
    fit = min(scores)
    bits = slice_bits(subaudible, rate_hz, bit_rate, DCS_PHASES // 2) * 2 - 1
    lags = range(5, 41)
    corr = [float(np.mean(bits[:-lag] * bits[lag:])) for lag in lags]
    period = list(lags)[int(np.argmax(corr))]
    # Noise fits a random rate at about 0.25 of a bit and repeats at no lag.
    if fit > DATA_MAX_FIT or max(corr) < DATA_MIN_PERIOD_CORR:
        return None
    return {"bit_rate": bit_rate, "fit": fit, "period_bits": period, "period_corr": max(corr)}


# ---- reporting ----------------------------------------------------------------------------------------------------

def label_samples(samples, rate_hz, offset_hz=0.0):
    freq_hz = discriminate(samples, rate_hz, offset_hz)
    result = {"ctcss": ctcss_label(freq_hz, rate_hz), "dcs": dcs_label(freq_hz, rate_hz)}
    if result["ctcss"]["label"] is None and result["dcs"]["label"] is None:
        result["data"] = data_signature(freq_hz, rate_hz)
    return result


def describe(name, result):
    ctcss = result["ctcss"]
    dcs = result["dcs"]
    lines = [f"{name}:"]
    tone = ctcss["label"] or f"none ({ctcss['why']})"
    lines.append(f"  CTCSS  {tone}; strongest line {ctcss['peak_hz']:.2f} Hz, {ctcss['prominence_db']:.1f} dB over "
                 f"the next, {ctcss['floor_db']:.1f} dB over the band median, ~{ctcss['deviation_hz']:.0f} Hz deviation")
    code = dcs["label"] or f"none ({dcs['why']})"
    readings = ", ".join(dcs["readings"]) if dcs["readings"] else "-"
    lines.append(f"  DCS    {code}; needs {dcs['needed']} of {dcs['words']} words; repeating readings: {readings}")
    data = result.get("data")
    if data:
        lines.append(f"  data   sub-audible NRZ near {data['bit_rate']:.1f} bit/s (fit {data['fit']:.3f}), repeating every "
                     f"{data['period_bits']} bits (correlation {data['period_corr']:.2f}); DCS is 134.4 bit/s, 23-bit words")
    return "\n".join(lines)


def run_fixtures(paths, offset_hz):
    for path in paths:
        samples, rate_hz = load_fixture(path)
        name = os.path.basename(path).replace(".iq.json", "")
        if offset_hz:
            name += f" at {offset_hz:+.0f} Hz"
        print(describe(name, label_samples(samples, rate_hz, offset_hz)))
    return 0


# ---- self-test ----------------------------------------------------------------------------------------------------

def fm_modulate(deviation_hz, rate_hz, rng, cnr_db=25.0):
    phase = np.cumsum(2.0 * math.pi * deviation_hz / rate_hz)
    sigma = math.sqrt(10.0 ** (-cnr_db / 10.0) / 2.0)
    return np.exp(1j * phase) + rng.normal(0.0, sigma, len(phase)) + 1j * rng.normal(0.0, sigma, len(phase))


def speech_like(count, rate_hz, rng, rms_hz=1500.0):
    noise = band_limit(rng.normal(0.0, 1.0, count), rate_hz, 300.0, 3000.0)
    return noise / np.std(noise) * rms_hz


def dcs_waveform(code, count, rate_hz, inverted=False, deviation_hz=600.0):
    word = dcs_encode(code)
    per_bit = rate_hz / DCS_BIT_RATE
    index = (np.arange(count) / per_bit).astype(int)
    bits = (word >> (index % DCS_WORD_BITS)) & 1
    if inverted:
        bits = 1 - bits
    levels = np.where(bits == 1, 1.0, -1.0)
    return band_limit(levels, rate_hz, 0.0, SUBAUDIBLE_LPF_HZ) * deviation_hz


def self_test():
    failures = []

    def check(what, ok):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            failures.append(what)

    print("Golay and DCS word layout against the cited 023 example:")
    word = dcs_encode(0o23)
    check("D023 word is 11101100011100000010011", format(word, "023b") == "11101100011100000010011")
    aliases = {dcs_code_of(rotate(word, k)) for k in range(DCS_WORD_BITS)} - {None}
    check("rotations of D023 read as 023, 340 and 766", aliases == {0o23, 0o340, 0o766})
    inverted = {dcs_code_of(rotate(word ^ 0x7FFFFF, k)) for k in range(DCS_WORD_BITS)} - {None}
    check("the inverted D023 word reads as 047", 0o47 in inverted)
    check("the DCS table holds 104 codes", len(set(DCS_CODES)) == 104)
    check("the CTCSS table holds 50 tones without 150.0", len(CTCSS_TONES) == 50 and 150.0 not in CTCSS_TONES)

    rate = 48000.0
    count = int(4 * rate)
    rng = np.random.default_rng(518)
    cases = [
        ("CTCSS 151.4 Hz under speech", 500.0 * np.sin(2 * math.pi * 151.4 * np.arange(count) / rate), "151.4", None),
        ("CTCSS 150.0 Hz (not in the table)", 500.0 * np.sin(2 * math.pi * 150.0 * np.arange(count) / rate), None, None),
        ("D023N under speech", dcs_waveform(0o23, count, rate), None, "D023N"),
        ("D023I under speech", dcs_waveform(0o23, count, rate, inverted=True), None, "D023I"),
        ("speech alone", np.zeros(count), None, None),
    ]
    print("Synthetic NFM (4 s, 25 dB CNR, 1.5 kHz rms speech-band noise):")
    for name, subaudible, want_tone, want_code in cases:
        result = label_samples(fm_modulate(subaudible + speech_like(count, rate, rng), rate, rng), rate)
        tone = result["ctcss"]["label"]
        code = result["dcs"]["label"]
        check(f"{name}: CTCSS {tone}", tone == want_tone)
        check(f"{name}: DCS {code}", (code is None) if want_code is None else (code is not None and want_code in code))
    print("self-test passed" if not failures else f"self-test FAILED ({len(failures)})")
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("fixtures", nargs="*", help="dsd-neo-iq cu8 sidecars (default: the committed NFM fixtures)")
    parser.add_argument("--offset-hz", type=float, default=0.0, help="label the channel this far from the centre")
    parser.add_argument("--self-test", action="store_true", help="check the oracle against synthetic signals")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    paths = args.fixtures
    if not paths:
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures", "iq")
        paths = [os.path.normpath(os.path.join(root, name + ".iq.json")) for name in DEFAULT_FIXTURES]
    return run_fixtures(paths, args.offset_hz)


if __name__ == "__main__":
    sys.exit(main())
