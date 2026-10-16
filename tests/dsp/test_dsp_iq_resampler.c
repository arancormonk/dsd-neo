// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The complex resampler a replay runs ahead of the half-band chain (issue #701): the ratio and its error per case, the
   filter's edges, passband ripple and stopband at the frequencies that really fold into the protected band, tone
   frequency and group delay, block-cut invariance, I/Q separation, clears, the output bound and the refusals. */

#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/rate_converter.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static const double kPi = 3.14159265358979323846;

/* The final DSP rate the replay cases protect: TETRA's 72 kHz, and 48 kHz. */
#define PROTECT_72K 32400
#define PROTECT_48K 21600

/* The outputs each spectral measurement windows. */
#define MEASURE_N   16384

static uint32_t g_rng = 0x2468ace1u;

static uint32_t
rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static float
rng_float(void) {
    return (float)((double)(rng_next() >> 8) / 8388608.0 - 1.0);
}

/* Bit-for-bit equality: the resampler promises the same bits however the input is cut, so no tolerance applies. */
static int
same_bits(const float* a, const float* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t x = 0U;
        uint32_t y = 0U;
        DSD_MEMCPY(&x, &a[i], sizeof(x));
        DSD_MEMCPY(&y, &b[i], sizeof(y));
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

static double
db(double ratio) {
    return 20.0 * log10(ratio > 1e-300 ? ratio : 1e-300);
}

/* The output rate L/M really runs at. */
static double
actual_out_hz(const dsd_iq_resampler* r) {
    return (double)r->in_hz * (double)r->L / (double)r->M;
}

/* A complex tone exp(j 2 pi f n / rate) of amplitude @p a, interleaved. */
static float*
make_tone(size_t n, double rate_hz, double f_hz, double a) {
    float* buf = (float*)malloc(n * 2 * sizeof(float));
    if (!buf) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        const double ph = 2.0 * kPi * f_hz * (double)i / rate_hz;
        buf[2 * i] = (float)(a * cos(ph));
        buf[2 * i + 1] = (float)(a * sin(ph));
    }
    return buf;
}

/* The amplitude of the complex tone at @p f_hz in @p n outputs at @p rate_hz, through a 4-term Blackman-Harris window
   (sidelobes below -92 dB) evaluated at the exact frequency, so there is no scalloping. */
static double
tone_amplitude(const float* iq, size_t n, double rate_hz, double f_hz) {
    double re = 0.0;
    double im = 0.0;
    double wsum = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double x = 2.0 * kPi * (double)i / (double)(n - 1);
        const double w = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2.0 * x) - 0.01168 * cos(3.0 * x);
        const double ph = -2.0 * kPi * f_hz * (double)i / rate_hz;
        const double c = cos(ph);
        const double s = sin(ph);
        const double yi = iq[2 * i];
        const double yq = iq[2 * i + 1];
        re += w * (yi * c - yq * s);
        im += w * (yi * s + yq * c);
        wsum += w;
    }
    return sqrt(re * re + im * im) / wsum;
}

/* Runs @p n inputs through @p r in one call; returns the outputs, or -1. */
static int
run_once(dsd_iq_resampler* r, const float* in, size_t n, float** out) {
    const size_t cap = dsd_iq_resampler_max_out(r, n);
    *out = (float*)malloc((cap + 1) * 2 * sizeof(float));
    if (!*out) {
        return -1;
    }
    return dsd_iq_resampler_process(r, in, n, *out, cap);
}

/* A tone at @p f_in through a fresh copy of the case; the steady-state outputs land in @p out (MEASURE_N of them). */
static int
tone_through(int in_hz, int out_hz, int protect_hz, double f_in, float* out) {
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    if (dsd_iq_resampler_configure(&r, in_hz, out_hz, protect_hz, 4096) != DSD_IQ_RESAMPLER_OK) {
        return 0;
    }
    const double R = actual_out_hz(&r);
    const size_t skip = (size_t)(2.0 * dsd_iq_resampler_group_delay_seconds(&r) * R) + 64;
    const size_t n_in = (size_t)((double)(skip + MEASURE_N) * (double)in_hz / R) + (size_t)r.taps_per_phase + 16;
    float* in = make_tone(n_in, (double)in_hz, f_in, 0.5);
    float* y = NULL;
    const int got = in ? run_once(&r, in, n_in, &y) : -1;
    int ok = y != NULL && got >= (int)(skip + MEASURE_N);
    if (ok) {
        DSD_MEMCPY(out, y + 2 * skip, (size_t)MEASURE_N * 2 * sizeof(float));
    }
    free(in);
    free(y);
    dsd_iq_resampler_free(&r);
    return ok;
}

/* The prototype's response at @p f_hz (at the L-times rate), normalised so the passband is 1. The rotations are
   stepped by complex multiplies rather than a cosine per tap: the error stays far below the stopband. */
static double
prototype_gain(const dsd_iq_resampler* r, double f_hz) {
    const int L = r->L;
    const int T = r->taps_per_phase;
    const double step = -2.0 * kPi * f_hz / ((double)r->in_hz * (double)L);
    const double w1r = cos(step);
    const double w1i = sin(step);
    const double wlr = cos(step * (double)L);
    const double wli = sin(step * (double)L);
    double br = 1.0;
    double bi = 0.0;
    double re = 0.0;
    double im = 0.0;
    for (int ph = 0; ph < L; ph++) {
        double rr = br;
        double ri = bi;
        for (int k = 0; k < T; k++) {
            const double h = r->bank[(size_t)ph * (size_t)T + (size_t)(T - 1 - k)];
            re += h * rr;
            im += h * ri;
            const double nr = rr * wlr - ri * wli;
            ri = rr * wli + ri * wlr;
            rr = nr;
        }
        const double nb = br * w1r - bi * w1i;
        bi = br * w1i + bi * w1r;
        br = nb;
    }
    return sqrt(re * re + im * im) / (double)L;
}

typedef struct {
    int in_hz;
    int out_hz;
    int protect_hz;
    int L;
    int M;
    double ppm;
} resample_case;

static const resample_case kCases[] = {
    {2048000, 1152000, PROTECT_72K, 9, 16, 0.0},     /* 2.048 MS/s to 1.152 MS/s, then /16 to 72 kHz */
    {48254, 72000, PROTECT_72K, 4063, 2723, 0.0102}, /* the reporter's capture to TETRA's rate */
    {48000, 72000, PROTECT_72K, 3, 2, 0.0},          {95999, 48000, PROTECT_48K, 1, 2, -10.4167},
    {8000, 72000, PROTECT_72K, 9, 1, 0.0}, /* the widest upsample the rate plan allows */
};

#define N_CASES (sizeof(kCases) / sizeof(kCases[0]))

/* The ratio, its error, the edges, the bank size and the group delay of each case. */
static void
test_design(void) {
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        int L = 0;
        int M = 0;
        (void)dsd_rate_converter_ratio(k->in_hz, k->out_hz, &L, &M);
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        const int st = dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 4096);
        CHECK(st == DSD_IQ_RESAMPLER_OK, "%d->%d: configure %d", k->in_hz, k->out_hz, st);
        if (st != DSD_IQ_RESAMPLER_OK) {
            continue;
        }
        CHECK(r.L == k->L && r.M == k->M, "%d->%d: L/M %d/%d, want %d/%d", k->in_hz, k->out_hz, r.L, r.M, k->L, k->M);
        CHECK(r.L == L && r.M == M, "%d->%d: L/M %d/%d differs from the rate converter's %d/%d", k->in_hz, k->out_hz,
              r.L, r.M, L, M);
        const double ppm = dsd_iq_resampler_ratio_error_ppm(&r);
        CHECK(fabs(ppm - k->ppm) <= 1e-3, "%d->%d: %.6f ppm, want %.4f", k->in_hz, k->out_hz, ppm, k->ppm);
        const double pe_want = fmin((double)k->protect_hz, 0.45 * (double)k->in_hz);
        const double s_want = fmin((double)k->in_hz - pe_want, (double)k->out_hz - (double)k->protect_hz);
        CHECK(fabs(r.passband_hz - pe_want) <= 1e-6, "%d->%d: passband %.3f, want %.3f", k->in_hz, k->out_hz,
              r.passband_hz, pe_want);
        CHECK(fabs(r.stopband_hz - s_want) <= 1e-6, "%d->%d: stopband %.3f, want %.3f", k->in_hz, k->out_hz,
              r.stopband_hz, s_want);
        CHECK(r.taps_per_phase % 4 == 0 && r.taps_per_phase >= 4
                  && r.taps_per_phase <= DSD_IQ_RESAMPLER_MAX_TAPS_PER_PHASE,
              "%d->%d: %d taps per phase", k->in_hz, k->out_hz, r.taps_per_phase);
        const double gd = dsd_iq_resampler_group_delay_seconds(&r);
        const double gd_want = ((double)r.L * (double)r.taps_per_phase - 1.0) / (2.0 * (double)r.L * (double)k->in_hz);
        CHECK(fabs(gd - gd_want) <= 1e-12, "%d->%d: group delay %.9f, want %.9f", k->in_hz, k->out_hz, gd, gd_want);
        DSD_FPRINTF(stderr,
                    "%d->%d: L/M %d/%d (%+.4f ppm), pass %.1f Hz, stop %.1f Hz, %d taps/phase, bank %d taps, group "
                    "delay %.4f ms\n",
                    k->in_hz, k->out_hz, r.L, r.M, ppm, r.passband_hz, r.stopband_hz, r.taps_per_phase,
                    r.L * r.taps_per_phase, gd * 1000.0);
        dsd_iq_resampler_free(&r);
    }
}

/* The prototype itself: ripple from DC to the passband edge, and the stopband from its edge over the first 60 lobes,
   where a Kaiser window's sidelobes are highest. */
static void
test_prototype_response(void) {
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 4096) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%d->%d: configure", k->in_hz, k->out_hz);
            continue;
        }
        double gmin = 1e9;
        double gmax = 0.0;
        for (int i = 0; i <= 100; i++) {
            const double g = prototype_gain(&r, r.passband_hz * (double)i / 100.0);
            gmin = fmin(gmin, g);
            gmax = fmax(gmax, g);
        }
        const double ripple = db(gmax) - db(gmin);
        CHECK(ripple <= DSD_IQ_RESAMPLER_RIPPLE_DB, "%d->%d: prototype ripple %.4f dB", k->in_hz, k->out_hz, ripple);
        CHECK(fabs(db(gmax)) <= DSD_IQ_RESAMPLER_RIPPLE_DB && fabs(db(gmin)) <= DSD_IQ_RESAMPLER_RIPPLE_DB,
              "%d->%d: prototype gain %.4f..%.4f dB", k->in_hz, k->out_hz, db(gmin), db(gmax));
        const double lobe = (double)k->in_hz / (double)r.taps_per_phase;
        double worst = 0.0;
        for (int i = 0; i <= 600; i++) {
            const double f = r.stopband_hz + lobe * (double)i / 10.0;
            if (f > (double)k->in_hz * (double)r.L / 2.0) {
                break;
            }
            worst = fmax(worst, prototype_gain(&r, f));
        }
        CHECK(db(worst) <= -DSD_IQ_RESAMPLER_STOPBAND_DB, "%d->%d: prototype stopband %.2f dB", k->in_hz, k->out_hz,
              db(worst));
        DSD_FPRINTF(stderr, "%d->%d: prototype ripple %.4f dB p-p, stopband peak %.2f dB\n", k->in_hz, k->out_hz,
                    ripple, db(worst));
        dsd_iq_resampler_free(&r);
    }
}

/* Streamed tones across the passband: the gain at each stays within the ripple, and the tone leaves at the frequency
   it came in at, scaled only by the ratio error. */
static void
test_passband_and_frequency(void) {
    float* y = (float*)malloc((size_t)MEASURE_N * 2 * sizeof(float));
    if (!y) {
        CHECK(0, "alloc");
        return;
    }
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        const double pe = fmin((double)k->protect_hz, 0.45 * (double)k->in_hz);
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 4096) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%d->%d: configure", k->in_hz, k->out_hz);
            continue;
        }
        const double R = actual_out_hz(&r);
        dsd_iq_resampler_free(&r);
        double gmin = 1e9;
        double gmax = 0.0;
        for (int i = -8; i <= 8; i++) {
            const double f = pe * (double)i / 8.0;
            if (!tone_through(k->in_hz, k->out_hz, k->protect_hz, f, y)) {
                CHECK(0, "%d->%d: tone %.1f", k->in_hz, k->out_hz, f);
                continue;
            }
            const double g = tone_amplitude(y, MEASURE_N, R, f) / 0.5;
            gmin = fmin(gmin, g);
            gmax = fmax(gmax, g);
            if (i == 3) {
                /* The frequency: the phase drift against the expected tone, from the first quarter's mean to the
                   last quarter's, at the nominal output rate. */
                const double f_want = f * (double)k->out_hz / R;
                const size_t q = MEASURE_N / 4;
                double ar = 0.0;
                double ai = 0.0;
                double br = 0.0;
                double bi = 0.0;
                for (size_t n = 0; n < q; n++) {
                    const size_t m = n + 3 * q;
                    const double pa = -2.0 * kPi * f_want * (double)n / (double)k->out_hz;
                    const double pb = -2.0 * kPi * f_want * (double)m / (double)k->out_hz;
                    ar += y[2 * n] * cos(pa) - y[2 * n + 1] * sin(pa);
                    ai += y[2 * n] * sin(pa) + y[2 * n + 1] * cos(pa);
                    br += y[2 * m] * cos(pb) - y[2 * m + 1] * sin(pb);
                    bi += y[2 * m] * sin(pb) + y[2 * m + 1] * cos(pb);
                }
                const double drift = atan2(bi * ar - br * ai, br * ar + bi * ai);
                const double f_err = drift * (double)k->out_hz / (2.0 * kPi * (double)(3 * q));
                CHECK(fabs(f_err) <= 1e-3, "%d->%d: tone %.3f Hz left %.6f Hz off %.4f Hz", k->in_hz, k->out_hz, f,
                      f_err, f_want);
                CHECK(fabs(f_want - f) <= fabs(f) * DSD_IQ_RESAMPLER_MAX_PPM * 1e-6,
                      "%d->%d: tone %.3f Hz leaves at %.4f Hz", k->in_hz, k->out_hz, f, f_want);
            }
        }
        const double ripple = db(gmax) - db(gmin);
        CHECK(ripple <= DSD_IQ_RESAMPLER_RIPPLE_DB, "%d->%d: streamed ripple %.4f dB", k->in_hz, k->out_hz, ripple);
        CHECK(fabs(db(gmax)) <= DSD_IQ_RESAMPLER_RIPPLE_DB && fabs(db(gmin)) <= DSD_IQ_RESAMPLER_RIPPLE_DB,
              "%d->%d: streamed gain %.4f..%.4f dB", k->in_hz, k->out_hz, db(gmin), db(gmax));
        DSD_FPRINTF(stderr, "%d->%d: streamed passband %.4f..%.4f dB to %.1f Hz\n", k->in_hz, k->out_hz, db(gmin),
                    db(gmax), pe);
    }
    free(y);
}

/* Where an input tone at @p f_in lands at the output when it passes as image @p k (k x in_hz + f_in at the L-times
   rate), folded into [-R/2, R/2). */
static double
fold(double f, double R) {
    double x = fmod(f, R);
    if (x < -R / 2.0) {
        x += R;
    }
    if (x >= R / 2.0) {
        x -= R;
    }
    return x;
}

/* One input tone: every image or alias of it that folds into +-protect at the output (other than the tone itself)
   must be at least 60 dB down. Returns the worst level found, in dB, or 0 when none folds there. */
static double
worst_fold_db(const resample_case* k, double f_in, const float* y, double R, int L, int* found) {
    double worst = -400.0;
    const int kmax = L < 8 ? L : 8;
    /* A tone inside the protected band passes as itself; one outside has no output of its own to step around. */
    const int passes = fabs(f_in) <= (double)k->protect_hz;
    const double wanted = fold(f_in, R);
    const double bin = R / (double)MEASURE_N;
    for (int i = -kmax; i <= kmax; i++) {
        const double f_high = f_in + (double)i * (double)k->in_hz;
        if (fabs(f_high) > (double)k->in_hz * (double)L / 2.0) {
            continue;
        }
        if (i == 0 && passes) {
            continue; /* the tone itself */
        }
        const double at = fold(f_high, R);
        if (fabs(at) > (double)k->protect_hz || (passes && fabs(at - wanted) < 8.0 * bin)) {
            continue;
        }
        const double lvl = db(tone_amplitude(y, MEASURE_N, R, at) / 0.5);
        worst = fmax(worst, lvl);
        (*found)++;
    }
    return worst;
}

/* The frequencies that really fold into the protected band. Decimating 2.048 MS/s to 1.152 MS/s, no input frequency
   reaches 1.152 MHz - 32.4 kHz directly (the input's Nyquist is 1.024 MHz), so only the upsampled images do: input at
   256 kHz + d (image at 2.304 MHz + d) and at -896 kHz + d (image at 1.152 MHz + d) land on d. Upsampling, the first
   image of the passband edge (in_hz - Pe) lands inside +-protect. Decimating 95999 Hz to 48 kHz, input from 26.4 kHz up folds straight in. */
static void
test_stopband_folds(void) {
    float* y = (float*)malloc((size_t)MEASURE_N * 2 * sizeof(float));
    if (!y) {
        CHECK(0, "alloc");
        return;
    }
    static const double kDecim[] = {256000.0 - 32400.0, 256000.0 - 12000.0,  256000.0,
                                    256000.0 + 20000.0, 256000.0 + 32400.0,  -896000.0 - 32400.0,
                                    -896000.0,          -896000.0 + 32400.0, 587520.0};
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 4096) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%d->%d: configure", k->in_hz, k->out_hz);
            continue;
        }
        const double R = actual_out_hz(&r);
        const double pe = r.passband_hz;
        const double r_stop = r.stopband_hz;
        const int L = r.L;
        dsd_iq_resampler_free(&r);
        double tones[16];
        size_t n_tones = 0;
        if (k->in_hz == 2048000) {
            for (size_t i = 0; i < sizeof(kDecim) / sizeof(kDecim[0]); i++) {
                tones[n_tones++] = kDecim[i];
            }
        } else {
            /* Passband tones, the edges first: -Pe's first image is in_hz - Pe, the stopband edge. */
            tones[n_tones++] = -pe;
            tones[n_tones++] = pe;
            tones[n_tones++] = -0.5 * pe;
            tones[n_tones++] = 0.25 * pe;
            tones[n_tones++] = 0.0;
            if (k->out_hz < k->in_hz) {
                /* Decimating, input from out_hz - protect up to the input's Nyquist folds straight in. */
                const double s_edge = r_stop;
                const double nyq = 0.49 * (double)k->in_hz;
                tones[n_tones++] = s_edge;
                tones[n_tones++] = -s_edge;
                tones[n_tones++] = fmin(s_edge + 0.5 * (double)k->protect_hz, nyq);
                tones[n_tones++] = -nyq;
            }
        }
        double worst = -400.0;
        int found = 0;
        for (size_t t = 0; t < n_tones; t++) {
            if (!tone_through(k->in_hz, k->out_hz, k->protect_hz, tones[t], y)) {
                CHECK(0, "%d->%d: tone %.1f", k->in_hz, k->out_hz, tones[t]);
                continue;
            }
            const double w = worst_fold_db(k, tones[t], y, R, L, &found);
            CHECK(w <= -DSD_IQ_RESAMPLER_STOPBAND_DB, "%d->%d: tone %.1f Hz folds into the protected band at %.2f dB",
                  k->in_hz, k->out_hz, tones[t], w);
            worst = fmax(worst, w);
        }
        CHECK(found > 0, "%d->%d: no fold measured", k->in_hz, k->out_hz);
        DSD_FPRINTF(stderr, "%d->%d: worst fold into +-%d Hz: %.2f dB (%d folds measured)\n", k->in_hz, k->out_hz,
                    k->protect_hz, worst, found);
    }
    free(y);
}

/* The first image of a 48254 Hz capture's passband edge: input at -Pe makes an image at in_hz - Pe, inside +-32.4 kHz
   at 72 kHz; and 587.52 kHz into the 1.152 MS/s case leaves at -564.48 kHz, outside +-32.4 kHz, for the half-band chain
   to remove. */
static void
test_named_folds(void) {
    float* y = (float*)malloc((size_t)MEASURE_N * 2 * sizeof(float));
    if (!y) {
        CHECK(0, "alloc");
        return;
    }
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    if (dsd_iq_resampler_configure(&r, 48254, 72000, PROTECT_72K, 4096) == DSD_IQ_RESAMPLER_OK) {
        const double pe = r.passband_hz;
        const double R = actual_out_hz(&r);
        const double image = fold(48254.0 - pe, R);
        CHECK(fabs(image) <= PROTECT_72K, "first image at %.1f Hz", image);
        if (tone_through(48254, 72000, PROTECT_72K, -pe, y)) {
            const double lvl = db(tone_amplitude(y, MEASURE_N, R, image) / 0.5);
            CHECK(lvl <= -DSD_IQ_RESAMPLER_STOPBAND_DB, "first image %.2f dB", lvl);
            DSD_FPRINTF(stderr, "48254->72000: input %.1f Hz, first image at %.1f Hz: %.2f dB\n", -pe, image, lvl);
        } else {
            CHECK(0, "tone");
        }
    } else {
        CHECK(0, "configure");
    }
    dsd_iq_resampler_free(&r);
    CHECK(fabs(fold(587520.0, 1152000.0) + 564480.0) <= 1e-6, "587.52 kHz folds to %.1f", fold(587520.0, 1152000.0));
    free(y);
}

/* Random I/Q through one call, and through random cuts (with a per-pass size that splits calls too): the same bits. */
static void
test_chunking(void) {
    const size_t n = 30000;
    float* in = (float*)malloc(n * 2 * sizeof(float));
    if (!in) {
        CHECK(0, "alloc");
        return;
    }
    for (size_t i = 0; i < n * 2; i++) {
        in[i] = rng_float();
    }
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        dsd_iq_resampler a;
        dsd_iq_resampler_init(&a);
        if (dsd_iq_resampler_configure(&a, k->in_hz, k->out_hz, k->protect_hz, 8192) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%d->%d: configure", k->in_hz, k->out_hz);
            continue;
        }
        float* ref = NULL;
        const int ref_n = run_once(&a, in, n, &ref);
        CHECK(ref_n > 0, "%d->%d: one call %d", k->in_hz, k->out_hz, ref_n);
        static const size_t kMaxIn[] = {8192, 7, 1};
        static const size_t kMaxChunk[] = {1, 5, 333, 5000};
        for (size_t mi = 0; mi < sizeof(kMaxIn) / sizeof(kMaxIn[0]); mi++) {
            for (size_t mc = 0; mc < sizeof(kMaxChunk) / sizeof(kMaxChunk[0]); mc++) {
                dsd_iq_resampler b;
                dsd_iq_resampler_init(&b);
                if (dsd_iq_resampler_configure(&b, k->in_hz, k->out_hz, k->protect_hz, kMaxIn[mi])
                    != DSD_IQ_RESAMPLER_OK) {
                    CHECK(0, "configure");
                    continue;
                }
                float* out = (float*)malloc(((size_t)ref_n + 8) * 2 * sizeof(float));
                size_t pos = 0;
                size_t got = 0;
                int bad = out == NULL;
                while (!bad && pos < n) {
                    size_t chunk = (size_t)(rng_next() % (uint32_t)(kMaxChunk[mc] + 1)); /* 0 included */
                    if (chunk > n - pos) {
                        chunk = n - pos;
                    }
                    const size_t bound = dsd_iq_resampler_max_out(&b, chunk);
                    if (got + bound > (size_t)ref_n + 8) {
                        bad = 1;
                        break;
                    }
                    const int w = dsd_iq_resampler_process(&b, in + 2 * pos, chunk, out + 2 * got, bound);
                    if (w < 0 || (size_t)w > bound) {
                        bad = 1;
                        break;
                    }
                    got += (size_t)w;
                    pos += chunk;
                }
                CHECK(!bad && got == (size_t)ref_n && same_bits(out, ref, got * 2),
                      "%d->%d: max_in %zu, cuts up to %zu: %zu outputs, want %d, or different bits", k->in_hz,
                      k->out_hz, kMaxIn[mi], kMaxChunk[mc], got, ref_n);
                free(out);
                dsd_iq_resampler_free(&b);
            }
        }
        free(ref);
        dsd_iq_resampler_free(&a);
    }
    free(in);
}

/* I alone and Q alone: the other channel stays silent, and together they give each channel's own result. */
static void
test_no_crosstalk(void) {
    const size_t n = 6000;
    float* iq = (float*)malloc(n * 2 * sizeof(float));
    float* i_only = (float*)malloc(n * 2 * sizeof(float));
    float* q_only = (float*)malloc(n * 2 * sizeof(float));
    if (!iq || !i_only || !q_only) {
        CHECK(0, "alloc");
        free(iq);
        free(i_only);
        free(q_only);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        iq[2 * i] = rng_float();
        iq[2 * i + 1] = rng_float();
        i_only[2 * i] = iq[2 * i];
        i_only[2 * i + 1] = 0.0f;
        q_only[2 * i] = 0.0f;
        q_only[2 * i + 1] = iq[2 * i + 1];
    }
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        float* outs[3] = {NULL, NULL, NULL};
        const float* ins[3] = {iq, i_only, q_only};
        int counts[3] = {-1, -1, -1};
        for (int v = 0; v < 3; v++) {
            dsd_iq_resampler r;
            dsd_iq_resampler_init(&r);
            if (dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 1024) == DSD_IQ_RESAMPLER_OK) {
                counts[v] = run_once(&r, ins[v], n, &outs[v]);
            }
            dsd_iq_resampler_free(&r);
        }
        CHECK(counts[0] > 0 && counts[0] == counts[1] && counts[0] == counts[2], "%d->%d: counts", k->in_hz, k->out_hz);
        if (counts[0] > 0 && counts[0] == counts[1] && counts[0] == counts[2]) {
            int leak = 0;
            int differ = 0;
            for (int m = 0; m < counts[0]; m++) {
                leak |= fabsf(outs[1][2 * m + 1]) > 1e-30f || fabsf(outs[2][2 * m]) > 1e-30f;
                differ |= !same_bits(&outs[0][2 * m], &outs[1][2 * m], 1)
                          || !same_bits(&outs[0][2 * m + 1], &outs[2][2 * m + 1], 1);
            }
            CHECK(!leak, "%d->%d: one channel leaks into the other", k->in_hz, k->out_hz);
            CHECK(!differ, "%d->%d: a channel depends on the other", k->in_hz, k->out_hz);
        }
        for (int v = 0; v < 3; v++) {
            free(outs[v]);
        }
    }
    free(iq);
    free(i_only);
    free(q_only);
}

/* A clear after some input matches a fresh resampler, bit for bit. */
static void
test_clear(void) {
    const size_t n = 5000;
    float* in = (float*)malloc(n * 2 * sizeof(float));
    if (!in) {
        CHECK(0, "alloc");
        return;
    }
    for (size_t i = 0; i < n * 2; i++) {
        in[i] = rng_float();
    }
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        dsd_iq_resampler fresh;
        dsd_iq_resampler used;
        dsd_iq_resampler_init(&fresh);
        dsd_iq_resampler_init(&used);
        if (dsd_iq_resampler_configure(&fresh, k->in_hz, k->out_hz, k->protect_hz, 512) != DSD_IQ_RESAMPLER_OK
            || dsd_iq_resampler_configure(&used, k->in_hz, k->out_hz, k->protect_hz, 512) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%d->%d: configure", k->in_hz, k->out_hz);
        } else {
            float* junk = NULL;
            (void)run_once(&used, in + 2 * 17, 1237, &junk);
            free(junk);
            dsd_iq_resampler_clear(&used);
            CHECK(used.phase == 0, "%d->%d: phase %d after clear", k->in_hz, k->out_hz, used.phase);
            float* a = NULL;
            float* b = NULL;
            const int na = run_once(&fresh, in, n, &a);
            const int nb = run_once(&used, in, n, &b);
            CHECK(na > 0 && na == nb && same_bits(a, b, (size_t)na * 2), "%d->%d: cleared %d vs fresh %d", k->in_hz,
                  k->out_hz, nb, na);
            free(a);
            free(b);
        }
        dsd_iq_resampler_free(&fresh);
        dsd_iq_resampler_free(&used);
    }
    free(in);
}

/* The output bound: every call writes at most max_out, the bound is ceil(n L / M), one short is refused with nothing
   consumed, and arguments are checked. */
static void
test_max_out(void) {
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 256) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%d->%d: configure", k->in_hz, k->out_hz);
            continue;
        }
        for (size_t n = 0; n < 40; n++) {
            const size_t want = (n * (size_t)r.L + (size_t)r.M - 1) / (size_t)r.M;
            CHECK(dsd_iq_resampler_max_out(&r, n) == want, "%d->%d: max_out(%zu) %zu, want %zu", k->in_hz, k->out_hz, n,
                  dsd_iq_resampler_max_out(&r, n), want);
        }
        float in[2 * 64];
        float out[2 * 64 * 16];
        for (size_t i = 0; i < 2 * 64; i++) {
            in[i] = rng_float();
        }
        int max_seen = 0;
        for (int call = 0; call < 400; call++) {
            const size_t n = 1 + (size_t)(rng_next() % 63u);
            const size_t bound = dsd_iq_resampler_max_out(&r, n);
            if (bound > 0) {
                const int phase = r.phase;
                CHECK(dsd_iq_resampler_process(&r, in, n, out, bound - 1) == -1, "%d->%d: one short accepted", k->in_hz,
                      k->out_hz);
                CHECK(r.phase == phase, "%d->%d: a refused call moved the phase", k->in_hz, k->out_hz);
            }
            const int w = dsd_iq_resampler_process(&r, in, n, out, bound);
            CHECK(w >= 0 && (size_t)w <= bound, "%d->%d: %zu inputs wrote %d, bound %zu", k->in_hz, k->out_hz, n, w,
                  bound);
            if (w > max_seen) {
                max_seen = w;
            }
        }
        CHECK(max_seen > 0, "%d->%d: no outputs", k->in_hz, k->out_hz);
        CHECK(dsd_iq_resampler_process(&r, NULL, 4, out, 64) == -1, "NULL input");
        CHECK(dsd_iq_resampler_process(&r, in, 4, NULL, 64) == -1, "NULL output");
        CHECK(dsd_iq_resampler_process(&r, in, 0, out, 0) == 0, "no input");
        dsd_iq_resampler_free(&r);
        CHECK(dsd_iq_resampler_process(&r, in, 4, out, 64) == -1, "freed resampler");
        CHECK(dsd_iq_resampler_max_out(&r, 4) == 0, "freed max_out");
    }
}

/* The measured group delay: a slow tone's phase at the output against the input's, in seconds. */
static void
test_group_delay_measured(void) {
    for (size_t c = 0; c < N_CASES; c++) {
        const resample_case* k = &kCases[c];
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (dsd_iq_resampler_configure(&r, k->in_hz, k->out_hz, k->protect_hz, 4096) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "configure");
            continue;
        }
        const double R = actual_out_hz(&r);
        const double gd = dsd_iq_resampler_group_delay_seconds(&r);
        const double f = 100.0;
        const size_t n_in = (size_t)((double)MEASURE_N * (double)k->in_hz / R) + 64;
        float* in = make_tone(n_in, (double)k->in_hz, f, 0.5);
        float* out = NULL;
        const int got = in ? run_once(&r, in, n_in, &out) : -1;
        /* Each steady output m is the tone at time m / R - delay; the phase residue gives the delay. */
        const size_t skip = (size_t)(2.0 * gd * R) + 64;
        double re = 0.0;
        double im = 0.0;
        for (size_t m = skip; got > 0 && m < (size_t)got; m++) {
            const double ph = -2.0 * kPi * f * (double)m / R;
            re += out[2 * m] * cos(ph) - out[2 * m + 1] * sin(ph);
            im += out[2 * m] * sin(ph) + out[2 * m + 1] * cos(ph);
        }
        const double measured = -atan2(im, re) / (2.0 * kPi * f);
        CHECK(got > (int)skip && fabs(measured - gd) <= 1e-7, "%d->%d: measured delay %.9f s, reported %.9f s",
              k->in_hz, k->out_hz, measured, gd);
        free(in);
        free(out);
        dsd_iq_resampler_free(&r);
    }
}

/* Refusals leave the resampler unconfigured; a refused reconfigure drops the old design. */
static void
test_refusals(void) {
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    CHECK(dsd_iq_resampler_configure(NULL, 48000, 72000, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_ERR_ARGS, "NULL");
    CHECK(dsd_iq_resampler_configure(&r, 0, 72000, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_ERR_ARGS, "rate 0");
    CHECK(dsd_iq_resampler_configure(&r, 48000, -1, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_ERR_ARGS, "rate -1");
    CHECK(dsd_iq_resampler_configure(&r, 48000, 72000, 0, 64) == DSD_IQ_RESAMPLER_ERR_ARGS, "protect 0");
    CHECK(dsd_iq_resampler_configure(&r, 48000, 72000, PROTECT_72K, 0) == DSD_IQ_RESAMPLER_ERR_ARGS, "max_in 0");
    /* Below the rate converter's lowest input, and past its largest decimation. */
    CHECK(dsd_iq_resampler_configure(&r, 500, 72000, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_ERR_RATIO, "500 Hz");
    CHECK(dsd_iq_resampler_configure(&r, 2048000, 72000, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_ERR_RATIO, "/28");
    /* A protected band at half the output leaves no transition band. */
    CHECK(dsd_iq_resampler_configure(&r, 2048000, 1152000, 576000, 64) == DSD_IQ_RESAMPLER_ERR_BAND, "band");
    /* 2 kHz of transition at 2.048 MS/s needs thousands of taps per phase. */
    CHECK(dsd_iq_resampler_configure(&r, 2048000, 1152000, 575000, 64) == DSD_IQ_RESAMPLER_ERR_TAPS, "taps");
    CHECK(!r.configured && r.bank == NULL && r.work == NULL, "refusals leave nothing");
    CHECK(dsd_iq_resampler_configure(&r, 48000, 72000, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_OK, "ok");
    CHECK(r.configured, "configured");
    CHECK(dsd_iq_resampler_configure(&r, 2048000, 1152000, 576000, 64) == DSD_IQ_RESAMPLER_ERR_BAND, "band again");
    CHECK(!r.configured && r.bank == NULL && r.work == NULL, "a refused reconfigure drops the design");
    CHECK(fabs(dsd_iq_resampler_ratio_error_ppm(&r)) <= 1e-12, "ppm unconfigured");
    CHECK(fabs(dsd_iq_resampler_group_delay_seconds(&r)) <= 1e-12, "delay unconfigured");
    /* Equal rates run the filter at 1/1. */
    CHECK(dsd_iq_resampler_configure(&r, 72000, 72000, PROTECT_72K, 64) == DSD_IQ_RESAMPLER_OK, "equal rates");
    CHECK(r.L == 1 && r.M == 1, "equal rates L/M %d/%d", r.L, r.M);
    dsd_iq_resampler_free(&r);
    dsd_iq_resampler_free(NULL);
    dsd_iq_resampler_clear(NULL);
    CHECK(strlen(dsd_iq_resampler_status_text(DSD_IQ_RESAMPLER_ERR_TAPS)) > 0, "status text");
    CHECK(strlen(dsd_iq_resampler_status_text(12345)) > 0, "unknown status text");
}

int
main(void) {
    test_design();
    test_prototype_response();
    test_passband_and_frequency();
    test_stopband_folds();
    test_named_folds();
    test_chunking();
    test_no_crosstalk();
    test_clear();
    test_max_out();
    test_group_delay_measured();
    test_refusals();
    if (g_failures) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DSP_IQ_RESAMPLER: all passed\n");
    return 0;
}
