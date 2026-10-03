#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Design the analog monitor's voice band-pass and print the figures its tests hold it to.

src/dsp/analog_voice.c builds two band-passes at whatever rate the monitor runs at:

  FM  a 6th-order elliptic high-pass (0.5 dB ripple, 40 dB stopband, passband edge 300 Hz), which takes every CTCSS
      tone (67.0-254.1 Hz), and everything else below 254.1 Hz, at least 40 dB down, then a 4th-order Butterworth
      low-pass at 3400 Hz. A DCS signal (134.4 bit/s NRZ) has most of its energy there, but the low-pass a transmitter
      shapes it with leaves some between 254 and 350 Hz, so a shaped DCS signal comes out about 32 dB down overall;
  AM  a 4th-order Butterworth high-pass at 200 Hz (AM carries no sub-audible signalling) and the same low-pass.

The elliptic high-pass is stored as its normalised analog low-pass prototype (passband edge 1 rad/s), three
second-order sections H(s) = (s^2 + wz2) / (s^2 + b1 s + b0), each scaled to unity at DC. The C code turns each into a
high-pass with s -> Wc/s, Wc = 2 fs tan(pi 300 / fs) (prewarped, so the 300 Hz edge lands exactly at every rate), and
discretises it with the bilinear transform. The whole cascade carries the prototype's DC gain, 10^(-rp/20) for an
even order, so the passband ripples between -0.5 and 0 dB. The Butterworth sections are RBJ cookbook biquads with
Q = 1 / (2 cos(pi/8)) and 1 / (2 cos(3 pi/8)). A section whose corner is at or above 0.45 fs is left out.

This script prints the prototype table the C file carries (--c), and for each rate the figures the tests in
tests/dsp/test_dsp_analog_voice.c bound (default): the smallest attenuation at any CTCSS tone and across 1-254.1 Hz,
the FM and AM passband range over 400-2500 Hz, the gain at 300 Hz, and the RMS rejection of a D023N DCS signal shaped
with a 300 Hz Blackman windowed-sinc low-pass (dcs_shaped_rejection_db()). Take every bound from here with an explicit
tolerance; raise the order or rs below if a stricter target is wanted. --golden RATE prints the digital coefficients
at that rate for the golden-coefficient test.

It needs numpy and scipy, and is not run by CI.

Usage:
    python3 tools/design_voice_filters.py [--c] [--golden RATE]
"""

import argparse
import math

import numpy as np
from scipy import signal

HP_ORDER = 6
HP_RIPPLE_DB = 0.5
HP_STOP_DB = 40.0
FM_HP_EDGE_HZ = 300.0
AM_HP_HZ = 200.0
LP_HZ = 3400.0
SKIP_CORNER_FRACTION = 0.45

CTCSS_HZ = [
    67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8,
    118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3,
    179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3,
    254.1,
]
RATES_HZ = [8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000]
BUTTERWORTH4_Q = [1.0 / (2.0 * math.cos(math.pi / 8.0)), 1.0 / (2.0 * math.cos(3.0 * math.pi / 8.0))]


def elliptic_prototype():
    """The normalised analog low-pass prototype as (wz2, b1, b0) sections and its overall DC gain."""
    z, p, k = signal.ellipap(HP_ORDER, HP_RIPPLE_DB, HP_STOP_DB)
    zeros = sorted([zz for zz in z if zz.imag > 0], key=lambda zz: zz.imag)
    poles = sorted([pp for pp in p if pp.imag > 0], key=lambda pp: pp.imag)
    if len(zeros) != HP_ORDER // 2 or len(poles) != HP_ORDER // 2:
        raise SystemExit("expected an even order with conjugate pairs only")
    sections = []
    for zz, pp in zip(zeros, poles):
        sections.append((abs(zz) ** 2, -2.0 * pp.real, abs(pp) ** 2))
    # Each section is scaled to unity at DC; the product of the raw sections' DC gains times k is the prototype's.
    dc = k
    for wz2, _b1, b0 in sections:
        dc *= wz2 / b0
    return sections, float(np.real(dc))


def bilinear_section(num, den, fs):
    """Discretise A s^2 + B s + C over A' s^2 + B' s + C' with s = 2 fs (1 - z^-1) / (1 + z^-1)."""
    k = 2.0 * fs
    a, b, c = num
    ad, bd, cd = den
    n0 = a * k * k + b * k + c
    n1 = 2.0 * (c - a * k * k)
    n2 = a * k * k - b * k + c
    d0 = ad * k * k + bd * k + cd
    d1 = 2.0 * (cd - ad * k * k)
    d2 = ad * k * k - bd * k + cd
    return [n0 / d0, n1 / d0, n2 / d0], [1.0, d1 / d0, d2 / d0]


def rbj_section(kind, f0, q, fs):
    w0 = 2.0 * math.pi * f0 / fs
    cw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    if kind == "lp":
        b = [(1.0 - cw) / 2.0, 1.0 - cw, (1.0 - cw) / 2.0]
    else:
        b = [(1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0]
    a0 = 1.0 + alpha
    return [x / a0 for x in b], [1.0, -2.0 * cw / a0, (1.0 - alpha) / a0]


def design(kind, fs):
    """The digital sections (b, a) the C code builds for kind 'fm' or 'am' at fs."""
    sections = []
    limit = SKIP_CORNER_FRACTION * fs
    if kind == "fm":
        proto, dc = elliptic_prototype()
        if FM_HP_EDGE_HZ < limit:
            wc = 2.0 * fs * math.tan(math.pi * FM_HP_EDGE_HZ / fs)
            for i, (wz2, b1, b0) in enumerate(proto):
                g = b0 / wz2
                b, a = bilinear_section((g * wz2, 0.0, g * wc * wc), (b0, b1 * wc, wc * wc), fs)
                if i == 0:
                    b = [x * dc for x in b]
                sections.append((b, a))
    else:
        if AM_HP_HZ < limit:
            for q in BUTTERWORTH4_Q:
                sections.append(rbj_section("hp", AM_HP_HZ, q, fs))
    if LP_HZ < limit:
        for q in BUTTERWORTH4_Q:
            sections.append(rbj_section("lp", LP_HZ, q, fs))
    return sections


def gain_db(sections, hz, fs):
    w = 2.0 * math.pi * hz / fs
    z1 = complex(math.cos(w), -math.sin(w))
    z2 = z1 * z1
    h = 1.0 + 0.0j
    for b, a in sections:
        h *= (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2)
    return 20.0 * math.log10(max(abs(h), 1e-300))


DCS_BAUD = 134.4
DCS_SHAPING_HZ = 300.0
DCS_SHAPING_TAPS_S = 0.1
DCS_SIGNAL_S = 4.0
DCS_SETTLE_S = 2.0


def dcs_word(code):
    """The 23-bit DCS word of a 9-bit code (bit 0 first), as tools/build_iq_fixtures.py builds it."""
    data = code | 0x800
    rem = data << 11
    for bit in range(22, 10, -1):
        if (rem >> bit) & 1:
            rem ^= 0xC75 << (bit - 11)
    return data | (rem << 12)


def dcs_shaped_rejection_db(fs, sections):
    """RMS rejection of D023N's word, repeated at 134.4 bit/s as +/-1 NRZ from bit 0 and shaped with a windowed-sinc
    low-pass (300 Hz, Blackman, DC gain 1, DCS_SHAPING_TAPS_S long, odd), measured after DCS_SETTLE_S. The C test
    builds the same signal sample for sample."""
    n = int(DCS_SIGNAL_S * fs)
    word = dcs_word(0o023)
    bits = np.array([(word >> (int(k * DCS_BAUD / fs) % 23)) & 1 for k in range(n)])
    nrz = np.where(bits == 1, 1.0, -1.0)
    taps = signal.firwin(int(fs * DCS_SHAPING_TAPS_S) | 1, DCS_SHAPING_HZ, fs=fs, window="blackman")
    shaped = signal.lfilter(taps, 1.0, nrz)
    out = shaped.copy()
    for b, a in sections:
        out = signal.lfilter(b, a, out)
    settle = int(DCS_SETTLE_S * fs)
    return -20.0 * math.log10(np.sqrt(np.mean(out[settle:] ** 2)) / np.sqrt(np.mean(shaped[settle:] ** 2)))


def report():
    print("rate    | FM worst CTCSS     | FM 1-254.1 Hz | FM 300 Hz | FM 400-2500 Hz    | AM 300 Hz | AM 400-2500 Hz "
          "| FM shaped DCS")
    sweep = np.arange(1.0, 254.1 + 1e-9, 0.1)
    band = np.arange(400.0, 2500.0 + 1e-9, 1.0)
    for fs in RATES_HZ:
        fm = design("fm", fs)
        am = design("am", fs)
        tones = [(-gain_db(fm, t, fs), t) for t in CTCSS_HZ]
        worst = min(tones)
        sweep_worst = min(-gain_db(fm, f, fs) for f in sweep)
        fm_band = [gain_db(fm, f, fs) for f in band]
        am_band = [gain_db(am, f, fs) for f in band]
        print(
            f"{fs:7d} | {worst[0]:6.3f} dB @ {worst[1]:5.1f} | {sweep_worst:9.3f} dB | {gain_db(fm, 300.0, fs):6.3f} dB "
            f"| {min(fm_band):6.3f}..{max(fm_band):6.3f} dB | {gain_db(am, 300.0, fs):6.3f} dB "
            f"| {min(am_band):6.3f}..{max(am_band):6.3f} dB | {dcs_shaped_rejection_db(fs, fm):6.3f} dB"
        )


def print_c_table():
    proto, dc = elliptic_prototype()
    print(f"/* {HP_ORDER}th-order elliptic low-pass prototype, {HP_RIPPLE_DB} dB ripple, {HP_STOP_DB:.0f} dB stopband,")
    print("   passband edge 1 rad/s (tools/design_voice_filters.py): {wz2, b1, b0} per section. */")
    for wz2, b1, b0 in proto:
        print(f"    {{{wz2:.17g}, {b1:.17g}, {b0:.17g}}},")
    print(f"/* DC gain: {dc:.17g} */")


def print_golden(fs):
    for kind in ("fm", "am"):
        print(f"/* {kind.upper()} at {fs} Hz: {{b0, b1, b2, a1, a2}} per section. */")
        for b, a in design(kind, fs):
            print(f"    {{{b[0]:.17g}, {b[1]:.17g}, {b[2]:.17g}, {a[1]:.17g}, {a[2]:.17g}}},")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--c", action="store_true", help="print the prototype table analog_voice.c carries")
    ap.add_argument("--golden", type=int, metavar="RATE", help="print the digital coefficients at RATE")
    args = ap.parse_args()
    if args.c:
        print_c_table()
    elif args.golden:
        print_golden(args.golden)
    else:
        report()


if __name__ == "__main__":
    main()
