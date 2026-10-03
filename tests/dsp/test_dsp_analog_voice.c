// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog monitor's voice band-pass and AGC (issue #518). Every response bound comes from
 * tools/design_voice_filters.py, which computes the design's figures at each rate: the elliptic high-pass's stopband
 * is exactly 40 dB (the worst CTCSS tone reads 40.0 dB), the combined FM passband reaches -0.72 dB at 48 kHz and
 * -0.74 dB at 96 kHz, and a DCS signal shaped below 300 Hz comes out 32.09 dB down at 8 kHz and 32.24 dB at 16 kHz,
 * so the bounds are 39.9 dB, +/-0.8 dB and 31.9 dB.
 */

#include <dsd-neo/dsp/analog_voice.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/safe_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static const double k_ctcss_hz[] = {
    67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,  94.8,  97.4,  100.0,
    103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7,
    159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6,
    199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1,
};
static const int k_rates[] = {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000};

/* The fixed RTL monitor gain at -n 50 and the input it takes to the -12 dBFS reference peak. */
static const double k_ref_gain = 32924.0;
static const double k_ref_peak = 8231.0;

static double
db(double v) {
    return 20.0 * log10(v > 1e-300 ? v : 1e-300);
}

/* Deterministic noise in [-1, 1). */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static double
noise(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    const uint64_t r = g_rng * 2685821657736338717ULL;
    return ((double)(r >> 11) / 9007199254740992.0) * 2.0 - 1.0;
}

/* Bit-identical, sample by sample (NaN never compares equal, and comparing float object bytes is not portable). */
static int
same_bits(const float* a, const float* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t ua = 0;
        uint32_t ub = 0;
        DSD_MEMCPY(&ua, &a[i], sizeof ua);
        DSD_MEMCPY(&ub, &b[i], sizeof ub);
        if (ua != ub) {
            return 0;
        }
    }
    return 1;
}

/* ---- band-pass ---- */

/* tools/design_voice_filters.py --golden 48000: {b0, b1, b2, a1, a2} per section. */
static const double k_golden_fm_48k[5][5] = {
    {0.90037024594870119, -1.7996802268478755, 0.90037024594870119, -1.9045639340574942, 0.90919512447893358},
    {0.99224757041004918, -1.9836705363105136, 0.99224757041004918, -1.9831222225526433, 0.98504345457796882},
    {0.9984018525040903, -1.9966491714879357, 0.9984018525040903, -1.9959671959453864, 0.99748568055073017},
    {0.0348472135051454, 0.069694427010290799, 0.0348472135051454, -1.2914920256320417, 0.43088087965262334},
    {0.04181788386450435, 0.0836357677290087, 0.04181788386450435, -1.5498359296888882, 0.71710746514690549},
};
static const double k_golden_am_48k[4][5] = {
    {0.97621943319638138, -1.9524388663927628, 0.97621943319638138, -1.9521042830479529, 0.95277344973757305},
    {0.98991223211814083, -1.9798244642362817, 0.98991223211814083, -1.9794851879071824, 0.98016374056538136},
    {0.0348472135051454, 0.069694427010290799, 0.0348472135051454, -1.2914920256320417, 0.43088087965262334},
    {0.04181788386450435, 0.0836357677290087, 0.04181788386450435, -1.5498359296888882, 0.71710746514690549},
};

static void
check_golden(const char* label, dsd_voice_band_kind kind, const double (*golden)[5], int count) {
    dsd_voice_bandpass bp;
    CHECK(dsd_voice_bandpass_design(&bp, kind, 48000) == 0, "%s: design", label);
    CHECK(bp.sections == count, "%s: %d sections, want %d", label, bp.sections, count);
    for (int i = 0; i < count && i < bp.sections; i++) {
        const double got[5] = {bp.sec[i].b0, bp.sec[i].b1, bp.sec[i].b2, bp.sec[i].a1, bp.sec[i].a2};
        for (int j = 0; j < 5; j++) {
            CHECK(fabs(got[j] - golden[i][j]) < 1e-12, "%s: section %d coefficient %d %.17g, want %.17g", label, i, j,
                  got[j], golden[i][j]);
        }
    }
}

static void
test_golden_coefficients(void) {
    check_golden("FM 48k", DSD_VOICE_BAND_FM, k_golden_fm_48k, 5);
    check_golden("AM 48k", DSD_VOICE_BAND_AM, k_golden_am_48k, 4);
}

static void
test_fm_response(void) {
    for (size_t r = 0; r < sizeof k_rates / sizeof k_rates[0]; r++) {
        const int fs = k_rates[r];
        dsd_voice_bandpass bp;
        CHECK(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, fs) == 0, "FM design at %d", fs);
        for (size_t t = 0; t < sizeof k_ctcss_hz / sizeof k_ctcss_hz[0]; t++) {
            const double g = dsd_voice_bandpass_gain_db(&bp, k_ctcss_hz[t]);
            CHECK(g <= -39.9, "FM %d Hz: CTCSS %.1f Hz only %.3f dB down", fs, k_ctcss_hz[t], -g);
        }
        /* Everything below the highest tone, where a DCS signal's main lobe lies. */
        for (int tenth = 10; tenth <= 2541; tenth++) {
            const double hz = (double)tenth / 10.0;
            const double g = dsd_voice_bandpass_gain_db(&bp, hz);
            if (!(g <= -39.9)) {
                CHECK(0, "FM %d Hz: %.1f Hz only %.3f dB down", fs, hz, -g);
                break;
            }
        }
        double lo = 1e9;
        double hi = -1e9;
        for (int hz = 400; hz <= 2500; hz++) {
            const double g = dsd_voice_bandpass_gain_db(&bp, (double)hz);
            lo = g < lo ? g : lo;
            hi = g > hi ? g : hi;
        }
        CHECK(lo >= -0.8 && hi <= 0.01, "FM %d Hz: 400-2500 Hz spans %.3f..%.3f dB", fs, lo, hi);
        const double edge = dsd_voice_bandpass_gain_db(&bp, 300.0);
        CHECK(fabs(edge + 0.5) < 0.01, "FM %d Hz: 300 Hz reads %.3f dB, want -0.5", fs, edge);
        if (fs >= 16000) {
            /* The 4th-order low-pass at 3400 Hz: 6 kHz is 19.8 dB down, more where the bilinear warp steepens it. */
            const double stop = dsd_voice_bandpass_gain_db(&bp, 6000.0);
            CHECK(stop <= -19.0, "FM %d Hz: 6 kHz only %.3f dB down", fs, -stop);
        }
    }
}

static void
test_am_response(void) {
    for (size_t r = 0; r < sizeof k_rates / sizeof k_rates[0]; r++) {
        const int fs = k_rates[r];
        dsd_voice_bandpass bp;
        CHECK(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_AM, fs) == 0, "AM design at %d", fs);
        const double at300 = dsd_voice_bandpass_gain_db(&bp, 300.0);
        CHECK(at300 >= -0.3 && at300 <= 0.01, "AM %d Hz: 300 Hz reads %.3f dB", fs, at300);
        double lo = 1e9;
        for (int hz = 400; hz <= 2500; hz++) {
            const double g = dsd_voice_bandpass_gain_db(&bp, (double)hz);
            lo = g < lo ? g : lo;
        }
        CHECK(lo >= -0.4, "AM %d Hz: 400-2500 Hz reaches %.3f dB", fs, lo);
        /* The 200 Hz Butterworth high-pass: 100 Hz is 24 dB down. */
        const double at100 = dsd_voice_bandpass_gain_db(&bp, 100.0);
        CHECK(at100 <= -23.0, "AM %d Hz: 100 Hz only %.3f dB down", fs, -at100);
    }
}

/* Steady-state RMS gain of a sinusoid through the real filter, against the designed response. */
static double
measured_gain_db(dsd_voice_band_kind kind, int fs, double hz) {
    dsd_voice_bandpass bp;
    (void)dsd_voice_bandpass_design(&bp, kind, fs);
    const int settle = fs * 2;
    const int measure = fs;
    float block[256];
    double in_sq = 0.0;
    double out_sq = 0.0;
    int n = 0;
    while (n < settle + measure) {
        int len = (settle + measure - n) < 256 ? (settle + measure - n) : 256;
        for (int i = 0; i < len; i++) {
            block[i] = (float)(10000.0 * sin(2.0 * M_PI * hz * (double)(n + i) / (double)fs));
        }
        float in_copy[256];
        DSD_MEMCPY(in_copy, block, sizeof(float) * (size_t)len);
        dsd_voice_bandpass_process(&bp, block, (size_t)len);
        for (int i = 0; i < len; i++) {
            if (n + i >= settle) {
                in_sq += (double)in_copy[i] * (double)in_copy[i];
                out_sq += (double)block[i] * (double)block[i];
            }
        }
        n += len;
    }
    return 10.0 * log10(out_sq / in_sq);
}

static void
test_process_matches_design(void) {
    const struct {
        dsd_voice_band_kind kind;
        int fs;
        double hz;
    } cases[] = {
        {DSD_VOICE_BAND_FM, 48000, 1000.0}, {DSD_VOICE_BAND_FM, 48000, 151.4}, {DSD_VOICE_BAND_FM, 8000, 254.1},
        {DSD_VOICE_BAND_FM, 22050, 3000.0}, {DSD_VOICE_BAND_AM, 48000, 300.0}, {DSD_VOICE_BAND_AM, 8000, 1000.0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_voice_bandpass bp;
        (void)dsd_voice_bandpass_design(&bp, cases[i].kind, cases[i].fs);
        const double want = dsd_voice_bandpass_gain_db(&bp, cases[i].hz);
        const double got = measured_gain_db(cases[i].kind, cases[i].fs, cases[i].hz);
        CHECK(fabs(got - want) < 0.05, "kind %d at %d Hz, %.1f Hz: measured %.3f dB, designed %.3f dB",
              (int)cases[i].kind, cases[i].fs, cases[i].hz, got, want);
    }
}

/* Every section's poles inside the unit circle (Jury), and an impulse response that dies away, at 192 kHz too. */
static void
test_stability(void) {
    const int rates[] = {8000, 48000, 96000, 192000};
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        for (int kind = 0; kind < 2; kind++) {
            dsd_voice_bandpass bp;
            (void)dsd_voice_bandpass_design(&bp, (dsd_voice_band_kind)kind, rates[r]);
            for (int s = 0; s < bp.sections; s++) {
                const double a1 = bp.sec[s].a1;
                const double a2 = bp.sec[s].a2;
                CHECK(fabs(a2) < 1.0 && fabs(a1) < 1.0 + a2,
                      "kind %d at %d Hz: section %d unstable (a1 %.17g a2 %.17g)", kind, rates[r], s, a1, a2);
            }
            /* 4 s of impulse response: the last 100 ms holds next to nothing. */
            const int total = rates[r] * 4;
            const int tail = rates[r] / 10;
            double tail_peak = 0.0;
            float x[1024];
            int n = 0;
            while (n < total) {
                const int len = (total - n) < 1024 ? (total - n) : 1024;
                DSD_MEMSET(x, 0, sizeof x);
                if (n == 0) {
                    x[0] = 30000.0f;
                }
                dsd_voice_bandpass_process(&bp, x, (size_t)len);
                for (int i = 0; i < len; i++) {
                    if (n + i >= total - tail && fabs((double)x[i]) > tail_peak) {
                        tail_peak = fabs((double)x[i]);
                    }
                }
                n += len;
            }
            CHECK(tail_peak < 1e-3, "kind %d at %d Hz: impulse response still %.3g after 3.9 s", kind, rates[r],
                  tail_peak);
        }
    }
}

static void
test_section_skip_and_passthrough(void) {
    dsd_voice_bandpass bp;
    /* 7 kHz: the low-pass corner (3400 Hz) is above 0.45 fs, so only the high-pass runs. */
    CHECK(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, 7000) == 0 && bp.sections == 3, "FM 7 kHz: %d sections",
          bp.sections);
    CHECK(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_AM, 7000) == 0 && bp.sections == 2, "AM 7 kHz: %d sections",
          bp.sections);
    /* 600 Hz: both corners out of reach. */
    CHECK(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, 600) == 0 && bp.sections == 0, "FM 600 Hz: %d sections",
          bp.sections);
    /* No rate: a pass-through. */
    CHECK(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, 0) == 0 && bp.sections == 0, "FM rate 0: %d sections",
          bp.sections);
    float x[4] = {1.0f, -2.0f, 3.5f, 0.25f};
    const float want[4] = {1.0f, -2.0f, 3.5f, 0.25f};
    dsd_voice_bandpass_process(&bp, x, 4);
    CHECK(same_bits(x, want, 4), "pass-through changed the samples");
    CHECK(fabs(dsd_voice_bandpass_gain_db(&bp, 1000.0)) < 1e-12, "pass-through gain not 0 dB");
    CHECK(dsd_voice_bandpass_design(NULL, DSD_VOICE_BAND_FM, 48000) == -1, "NULL design accepted");
    /* An out-of-range kind on purpose: the design refuses it. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    CHECK(dsd_voice_bandpass_design(&bp, (dsd_voice_band_kind)7, 48000) == -1, "unknown kind accepted");
}

static void
test_bandpass_reset_and_nonfinite(void) {
    dsd_voice_bandpass bp;
    (void)dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, 48000);
    float x[64];
    DSD_MEMSET(x, 0, sizeof x);
    x[0] = 30000.0f;
    dsd_voice_bandpass_process(&bp, x, 64);
    dsd_voice_bandpass_reset(&bp);
    DSD_MEMSET(x, 0, sizeof x);
    dsd_voice_bandpass_process(&bp, x, 64);
    int quiet = 1;
    for (int i = 0; i < 64; i++) {
        quiet &= fabs((double)x[i]) < 1e-30;
    }
    CHECK(quiet, "reset left the impulse's tail in the state");

    /* A non-finite sample comes out as 0 and is skipped: the rest matches a run without it. */
    dsd_voice_bandpass a;
    dsd_voice_bandpass b;
    (void)dsd_voice_bandpass_design(&a, DSD_VOICE_BAND_FM, 48000);
    b = a;
    float clean[198];
    float with_bad[200];
    for (int i = 0; i < 198; i++) {
        clean[i] = (float)(5000.0 * noise());
    }
    for (int i = 0, j = 0; i < 200; i++) {
        if (i == 100) {
            with_bad[i] = NAN;
        } else if (i == 150) {
            with_bad[i] = INFINITY;
        } else {
            with_bad[i] = clean[j++];
        }
    }
    dsd_voice_bandpass_process(&a, with_bad, 200);
    dsd_voice_bandpass_process(&b, clean, 198);
    int same = 1;
    for (int i = 0, j = 0; i < 200; i++) {
        if (i == 100 || i == 150) {
            CHECK(fabs((double)with_bad[i]) < 1e-30, "non-finite sample %d came out as %g", i, (double)with_bad[i]);
            continue;
        }
        same &= same_bits(&with_bad[i], &clean[j++], 1);
    }
    CHECK(same, "a non-finite sample changed the filter state");
}

/* D023N's 23-bit word, repeated at 134.4 bit/s as +/-1 NRZ from bit 0, shaped with a 300 Hz windowed-sinc low-pass
   (Blackman, 0.1 s, DC gain 1) the way a transmitter keeps its signalling out of the voice band: the signal
   tools/design_voice_filters.py measures in dcs_shaped_rejection_db(). Its main lobe sits under 254 Hz, which the FM
   band-pass takes 40 dB down; the shaping's skirt reaches past 300 Hz, so the whole signal comes out about 32 dB
   down. */
static double
shaped_dcs_rejection_db(int fs) {
    const unsigned int word = 0x763813U; /* D023N, bit 0 first (RUNTIME_ANALOG_TONES pins the encoder) */
    const int n = 4 * fs;
    const int settle = 2 * fs;
    const int taps = ((int)(0.1 * (double)fs)) | 1;
    double* h = (double*)calloc((size_t)taps, sizeof(double));
    float* x = (float*)calloc((size_t)n, sizeof(float));
    float* y = (float*)calloc((size_t)n, sizeof(float));
    if (!h || !x || !y) {
        free(h);
        free(x);
        free(y);
        return 0.0;
    }
    const double fc = 2.0 * 300.0 / (double)fs;
    const double mid = 0.5 * (double)(taps - 1);
    double sum = 0.0;
    for (int k = 0; k < taps; k++) {
        const double t = (double)k - mid;
        const double arg = M_PI * fc * t;
        const double sinc = fabs(t) < 1e-12 ? 1.0 : sin(arg) / arg;
        const double w = 0.42 - (0.5 * cos(2.0 * M_PI * (double)k / (double)(taps - 1)))
                         + (0.08 * cos(4.0 * M_PI * (double)k / (double)(taps - 1)));
        h[k] = fc * sinc * w;
        sum += h[k];
    }
    for (int k = 0; k < taps; k++) {
        h[k] /= sum;
    }
    for (int i = 0; i < n; i++) {
        double acc = 0.0;
        for (int k = 0; k < taps && k <= i; k++) {
            const long bit_index = (long)(((double)(i - k) * 134.4) / (double)fs);
            const double nrz = ((word >> (unsigned)(bit_index % 23L)) & 1U) ? 1.0 : -1.0;
            acc += h[k] * nrz;
        }
        x[i] = (float)acc;
    }
    DSD_MEMCPY(y, x, sizeof(float) * (size_t)n);
    dsd_voice_bandpass bp;
    (void)dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, fs);
    dsd_voice_bandpass_process(&bp, y, (size_t)n);
    double in_sq = 0.0;
    double out_sq = 0.0;
    for (int i = settle; i < n; i++) {
        in_sq += (double)x[i] * (double)x[i];
        out_sq += (double)y[i] * (double)y[i];
    }
    free(h);
    free(x);
    free(y);
    return in_sq > 0.0 ? -10.0 * log10(out_sq / in_sq) : 0.0;
}

static void
test_shaped_dcs_rejection(void) {
    const int rates[] = {8000, 16000};
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        const double rej = shaped_dcs_rejection_db(rates[r]);
        CHECK(rej >= 31.9, "%d Hz: shaped DCS only %.2f dB down", rates[r], rej);
    }
}

static void
test_bandpass_partition_invariance(void) {
    enum { N = 48000 };

    static float whole[N];
    static float parts[N];
    for (int i = 0; i < N; i++) {
        whole[i] = (float)(8000.0 * noise() + 6000.0 * sin(2.0 * M_PI * 151.4 * (double)i / 48000.0));
    }
    DSD_MEMCPY(parts, whole, sizeof whole);
    dsd_voice_bandpass a;
    dsd_voice_bandpass b;
    (void)dsd_voice_bandpass_design(&a, DSD_VOICE_BAND_FM, 48000);
    b = a;
    dsd_voice_bandpass_process(&a, whole, N);
    int at = 0;
    const int cuts[] = {1, 7, 960, 13, 4097, 2, 333};
    for (int c = 0; at < N; c = (c + 1) % 7) {
        const int len = (N - at) < cuts[c] ? (N - at) : cuts[c];
        dsd_voice_bandpass_process(&b, parts + at, (size_t)len);
        at += len;
    }
    CHECK(same_bits(whole, parts, N), "band-pass output depends on the call partitioning");
}

/* ---- AGC ---- */

/* A 1 kHz tone at @p level_db relative to the reference input, @p seconds long, at @p fs; returns the output peak
   over the last half second. */
static double
agc_tone_peak(dsd_voice_agc* agc, int fs, double level_db, double seconds, double* phase) {
    const double amp = (k_ref_peak / k_ref_gain) * pow(10.0, level_db / 20.0);
    const int total = (int)(seconds * (double)fs);
    const int last = fs / 2;
    double peak = 0.0;
    float x[480];
    int n = 0;
    while (n < total) {
        const int len = (total - n) < 480 ? (total - n) : 480;
        for (int i = 0; i < len; i++) {
            x[i] = (float)(amp * sin(*phase));
            *phase += 2.0 * M_PI * 1000.0 / (double)fs;
        }
        dsd_voice_agc_process(agc, x, (size_t)len, 1);
        for (int i = 0; i < len; i++) {
            if (n + i >= total - last && fabs((double)x[i]) > peak) {
                peak = fabs((double)x[i]);
            }
        }
        n += len;
    }
    return peak;
}

static void
test_agc_convergence(void) {
    const int rates[] = {8000, 22050, 48000};
    const double levels[] = {-20.0, 0.0, 10.0};
    const double gmax = k_ref_gain * pow(10.0, DSD_VOICE_AGC_MAX_BOOST_DB / 20.0);
    for (size_t r = 0; r < 3; r++) {
        for (size_t l = 0; l < 3; l++) {
            dsd_voice_agc agc;
            CHECK(dsd_voice_agc_init(&agc, rates[r], k_ref_gain) == 0, "init at %d", rates[r]);
            double phase = 0.0;
            const double peak = agc_tone_peak(&agc, rates[r], levels[l], 3.0, &phase);
            const double amp = (k_ref_peak / k_ref_gain) * pow(10.0, levels[l] / 20.0);
            double want = amp * gmax;
            if (want > (double)DSD_VOICE_AGC_TARGET_PEAK) {
                want = (double)DSD_VOICE_AGC_TARGET_PEAK;
            }
            CHECK(fabs(db(peak) - db(want)) < 0.5, "%d Hz, %+.0f dB: settled at %.1f dBFS peak, want %.1f", rates[r],
                  levels[l], db(peak / 32768.0), db(want / 32768.0));
        }
    }
    /* The reference tone, settled, gets the same gain at every rate. */
    double gains[3];
    for (size_t r = 0; r < 3; r++) {
        dsd_voice_agc agc;
        (void)dsd_voice_agc_init(&agc, rates[r], k_ref_gain);
        double phase = 0.0;
        (void)agc_tone_peak(&agc, rates[r], 0.0, 3.0, &phase);
        gains[r] = dsd_voice_agc_gain(&agc);
    }
    CHECK(fabs(db(gains[0] / gains[2])) < 0.1 && fabs(db(gains[1] / gains[2])) < 0.1,
          "settled gain differs across rates: %.3f / %.3f / %.3f dB", db(gains[0]), db(gains[1]), db(gains[2]));
}

static void
test_agc_never_exceeds_target(void) {
    dsd_voice_agc agc;
    (void)dsd_voice_agc_init(&agc, 48000, k_ref_gain);
    double phase = 0.0;
    (void)agc_tone_peak(&agc, 48000, 0.0, 2.0, &phase);
    /* +30 dB step, then a full-scale click and noise at every level. */
    const double amp = (k_ref_peak / k_ref_gain) * pow(10.0, 30.0 / 20.0);
    float x[480];
    double peak = 0.0;
    for (int blk = 0; blk < 100; blk++) {
        for (int i = 0; i < 480; i++) {
            x[i] = (float)(amp * sin(phase));
            phase += 2.0 * M_PI * 1000.0 / 48000.0;
            if (blk == 50 && i == 7) {
                x[i] = 32767.0f;
            }
            if (blk > 70) {
                x[i] = (float)(noise() * pow(10.0, (double)(blk - 85) / 5.0));
            }
        }
        dsd_voice_agc_process(&agc, x, 480, blk % 3 != 0);
        for (int i = 0; i < 480; i++) {
            peak = fabs((double)x[i]) > peak ? fabs((double)x[i]) : peak;
        }
    }
    CHECK(peak <= (double)DSD_VOICE_AGC_TARGET_PEAK * 1.0001, "output peak %.1f above the target", peak);
}

static void
test_agc_partition_invariance(void) {
    enum { N = 96000 };

    static float whole[N];
    static float parts[N];
    static int playing[N];
    for (int i = 0; i < N; i++) {
        const double env = (i / 4800) % 3 == 2 ? 0.02 : 1.0;
        whole[i] = (float)(0.3 * env * sin(2.0 * M_PI * 700.0 * (double)i / 48000.0) + 0.01 * noise());
        playing[i] = (i / 7000) % 4 != 3;
    }
    DSD_MEMCPY(parts, whole, sizeof whole);
    dsd_voice_agc a;
    dsd_voice_agc b;
    (void)dsd_voice_agc_init(&a, 48000, k_ref_gain);
    b = a;
    /* One call per run of equal playing flags, against random cuts that never straddle a flag change. */
    int at = 0;
    while (at < N) {
        int end = at;
        while (end < N && playing[end] == playing[at]) {
            end++;
        }
        dsd_voice_agc_process(&a, whole + at, (size_t)(end - at), playing[at]);
        at = end;
    }
    const int cuts[] = {1, 960, 17, 4096, 3, 480, 129};
    at = 0;
    for (int c = 0; at < N; c = (c + 1) % 7) {
        int len = (N - at) < cuts[c] ? (N - at) : cuts[c];
        for (int i = 1; i < len; i++) {
            if (playing[at + i] != playing[at]) {
                len = i;
                break;
            }
        }
        dsd_voice_agc_process(&b, parts + at, (size_t)len, playing[at]);
        at += len;
    }
    CHECK(same_bits(whole, parts, N), "AGC output depends on the call partitioning");
}

static void
test_agc_floor_freeze_and_gmax(void) {
    dsd_voice_agc agc;
    (void)dsd_voice_agc_init(&agc, 48000, k_ref_gain);
    double phase = 0.0;
    (void)agc_tone_peak(&agc, 48000, 0.0, 2.0, &phase);
    const double settled = dsd_voice_agc_gain(&agc);

    /* Exact zeros (a squelched span) and input under the floor leave the gain where it was, for any length. */
    float x[4800];
    for (int s = 0; s < 100; s++) {
        DSD_MEMSET(x, 0, sizeof x);
        dsd_voice_agc_process(&agc, x, 4800, 1);
    }
    CHECK(fabs(dsd_voice_agc_gain(&agc) - settled) < 1e-9 * settled, "10 s of zeros moved the gain %.3f dB",
          db(dsd_voice_agc_gain(&agc) / settled));
    const double under = (k_ref_peak / k_ref_gain) * pow(10.0, (DSD_VOICE_AGC_FLOOR_DB - 1.0) / 20.0);
    for (int s = 0; s < 100; s++) {
        for (int i = 0; i < 4800; i++) {
            x[i] = (float)(under * sin(2.0 * M_PI * 500.0 * (double)i / 48000.0));
        }
        dsd_voice_agc_process(&agc, x, 4800, 1);
    }
    CHECK(fabs(dsd_voice_agc_gain(&agc) - settled) < 1e-9 * settled, "input under the floor moved the gain %.3f dB",
          db(dsd_voice_agc_gain(&agc) / settled));

    /* Not playing: neither a loud burst nor a long quiet span moves it, and the output stays limited. */
    double peak = 0.0;
    for (int s = 0; s < 50; s++) {
        for (int i = 0; i < 4800; i++) {
            x[i] = (float)((s % 2 ? 2.0 : 0.001) * sin(2.0 * M_PI * 800.0 * (double)i / 48000.0));
        }
        dsd_voice_agc_process(&agc, x, 4800, 0);
        for (int i = 0; i < 4800; i++) {
            peak = fabs((double)x[i]) > peak ? fabs((double)x[i]) : peak;
        }
    }
    CHECK(fabs(dsd_voice_agc_gain(&agc) - settled) < 1e-9 * settled, "a muted span moved the gain");
    CHECK(peak <= (double)DSD_VOICE_AGC_TARGET_PEAK * 1.0001, "muted output peak %.1f above the target", peak);

    /* A quiet signal above the floor boosts no further than DSD_VOICE_AGC_MAX_BOOST_DB. */
    dsd_voice_agc quiet;
    (void)dsd_voice_agc_init(&quiet, 48000, k_ref_gain);
    phase = 0.0;
    (void)agc_tone_peak(&quiet, 48000, -30.0, 20.0, &phase);
    const double boost = db(dsd_voice_agc_gain(&quiet) / k_ref_gain);
    CHECK(fabs(boost - DSD_VOICE_AGC_MAX_BOOST_DB) < 0.01, "quiet signal boosted %.3f dB", boost);

    dsd_voice_agc_reset(&quiet);
    CHECK(fabs(dsd_voice_agc_gain(&quiet) - k_ref_gain) < 1e-6 * k_ref_gain, "reset left gain %.1f",
          dsd_voice_agc_gain(&quiet));
    CHECK(dsd_voice_agc_init(NULL, 48000, k_ref_gain) == -1, "NULL init accepted");
    CHECK(dsd_voice_agc_init(&quiet, 0, k_ref_gain) == -1, "rate 0 accepted");
    CHECK(dsd_voice_agc_init(&quiet, 48000, 0.0) == -1, "gain 0 accepted");
    CHECK(dsd_voice_agc_init(&quiet, 48000, NAN) == -1, "NaN gain accepted");
}

/* Feed @p seconds of @p level_db tone (relative to the reference input) with the given playing flag. */
static void
agc_feed(dsd_voice_agc* agc, int fs, double level_db, double seconds, int playing, double* phase) {
    const double amp = (k_ref_peak / k_ref_gain) * pow(10.0, level_db / 20.0);
    const int total = (int)(seconds * (double)fs);
    float x[160];
    for (int n = 0; n < total;) {
        const int len = (total - n) < 160 ? (total - n) : 160;
        for (int i = 0; i < len; i++) {
            x[i] = (float)(amp * sin(*phase));
            *phase += 2.0 * M_PI * 1000.0 / (double)fs;
        }
        dsd_voice_agc_process(agc, x, (size_t)len, playing);
        n += len;
    }
}

static void
test_agc_rollback(void) {
    dsd_voice_agc agc;
    (void)dsd_voice_agc_init(&agc, 48000, k_ref_gain);
    double phase = 0.0;
    agc_feed(&agc, 48000, 0.0, 2.0, 1, &phase);
    const double settled = dsd_voice_agc_gain(&agc);
    /* A 100 ms squelch tail 20 dB up pulls the gain down 20 dB... */
    agc_feed(&agc, 48000, 20.0, 0.1, 1, &phase);
    CHECK(db(dsd_voice_agc_gain(&agc) / settled) < -19.0, "the tail did not pull the gain down");
    /* ... and the gate closing puts it back where it stood before the tail. */
    agc_feed(&agc, 48000, 0.0, 0.02, 0, &phase);
    CHECK(fabs(db(dsd_voice_agc_gain(&agc) / settled)) < 0.1, "after the close the gain is %.2f dB off",
          db(dsd_voice_agc_gain(&agc) / settled));

    /* A gate open for less than the rollback span goes back to the gain it opened on. */
    const double before = dsd_voice_agc_gain(&agc);
    agc_feed(&agc, 48000, 25.0, 0.05, 1, &phase);
    agc_feed(&agc, 48000, 0.0, 0.02, 0, &phase);
    CHECK(fabs(db(dsd_voice_agc_gain(&agc) / before)) < 1e-6, "a 50 ms opening kept %.2f dB",
          db(dsd_voice_agc_gain(&agc) / before));
}

/* After the hold, the gain rises at 3 dB/s while the input sits more than 20 dB under the envelope (a pause full of
   hiss), and at 20 dB/s while it stays within 20 dB (speech getting quieter). */
static void
test_agc_release_rates(void) {
    const int rates[] = {8000, 48000};
    for (size_t r = 0; r < 2; r++) {
        const int fs = rates[r];
        dsd_voice_agc agc;
        double phase = 0.0;
        (void)dsd_voice_agc_init(&agc, fs, k_ref_gain);
        agc_feed(&agc, fs, 0.0, 1.0, 1, &phase);
        double g0 = dsd_voice_agc_gain(&agc);
        /* Within the hold, nothing moves. */
        agc_feed(&agc, fs, -25.0, 0.45, 1, &phase);
        CHECK(fabs(db(dsd_voice_agc_gain(&agc) / g0)) < 1e-6, "%d Hz: the gain moved inside the hold", fs);
        agc_feed(&agc, fs, -25.0, 0.05, 1, &phase);
        g0 = dsd_voice_agc_gain(&agc);
        agc_feed(&agc, fs, -25.0, 1.0, 1, &phase);
        const double slow = db(dsd_voice_agc_gain(&agc) / g0);
        CHECK(fabs(slow - DSD_VOICE_AGC_RELEASE_SLOW_DB_PER_S) < 0.1, "%d Hz: rose %.2f dB in 1 s of hiss", fs, slow);

        (void)dsd_voice_agc_init(&agc, fs, k_ref_gain);
        agc_feed(&agc, fs, 0.0, 1.0, 1, &phase);
        agc_feed(&agc, fs, -15.0, 0.5, 1, &phase);
        g0 = dsd_voice_agc_gain(&agc);
        agc_feed(&agc, fs, -15.0, 0.25, 1, &phase);
        const double fast = db(dsd_voice_agc_gain(&agc) / g0);
        CHECK(fabs(fast - (DSD_VOICE_AGC_RELEASE_FAST_DB_PER_S / 4.0)) < 0.2, "%d Hz: rose %.2f dB in 250 ms", fs,
              fast);
    }
}

static void
test_agc_nonfinite(void) {
    dsd_voice_agc a;
    dsd_voice_agc b;
    (void)dsd_voice_agc_init(&a, 48000, k_ref_gain);
    b = a;
    float with_bad[2000];
    float clean[1998];
    for (int i = 0; i < 1998; i++) {
        clean[i] = (float)(0.3 * sin(2.0 * M_PI * 900.0 * (double)i / 48000.0));
    }
    for (int i = 0, j = 0; i < 2000; i++) {
        if (i == 500) {
            with_bad[i] = NAN;
        } else if (i == 1500) {
            with_bad[i] = -INFINITY;
        } else {
            with_bad[i] = clean[j++];
        }
    }
    dsd_voice_agc_process(&a, with_bad, 2000, 1);
    dsd_voice_agc_process(&b, clean, 1998, 1);
    int same = 1;
    for (int i = 0, j = 0; i < 2000; i++) {
        if (i == 500 || i == 1500) {
            CHECK(fabs((double)with_bad[i]) < 1e-30, "non-finite sample %d came out as %g", i, (double)with_bad[i]);
            continue;
        }
        same &= same_bits(&with_bad[i], &clean[j++], 1);
    }
    CHECK(same, "a non-finite sample changed the AGC state");
    CHECK(isfinite(dsd_voice_agc_gain(&a)), "gain not finite");
}

int
main(void) {
    test_golden_coefficients();
    test_fm_response();
    test_am_response();
    test_process_matches_design();
    test_stability();
    test_section_skip_and_passthrough();
    test_bandpass_reset_and_nonfinite();
    test_shaped_dcs_rejection();
    test_bandpass_partition_invariance();
    test_agc_convergence();
    test_agc_never_exceeds_target();
    test_agc_partition_invariance();
    test_agc_floor_freeze_and_gmax();
    test_agc_rollback();
    test_agc_release_rates();
    test_agc_nonfinite();
    if (g_failures) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("DSP_ANALOG_VOICE: OK\n");
    return 0;
}
