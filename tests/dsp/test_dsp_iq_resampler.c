// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The complex resampler a replay runs ahead of the half-band chain (issue #701): the ratio and its error per case, the
   filter's edges, passband ripple and stopband at the frequencies that really fold into the protected band, tone
   frequency and group delay, block-cut invariance, I/Q separation, clears, the output bound and the refusals. Then the
   rational entry point the live rate step uses (issue #806): every live ratio at its exact fractional input rate, the
   match with the scaled whole-Hz design bit for bit, the delay and tail it reports, swaps, a refused candidate next to
   a running resampler, block cuts and its refusals. */

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
    return dsd_iq_resampler_in_rate_hz(r) * (double)r->L / (double)r->M;
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

/* A tone at @p f_in through @p r from a clear start; the steady-state outputs land in @p out (MEASURE_N of them). */
static int
tone_through_design(dsd_iq_resampler* r, double f_in, float* out) {
    dsd_iq_resampler_clear(r);
    const double in_hz = dsd_iq_resampler_in_rate_hz(r);
    const double R = actual_out_hz(r);
    const size_t skip = (size_t)(2.0 * dsd_iq_resampler_group_delay_seconds(r) * R) + 64;
    const size_t n_in = (size_t)((double)(skip + MEASURE_N) * in_hz / R) + (size_t)r->taps_per_phase + 16;
    float* in = make_tone(n_in, in_hz, f_in, 0.5);
    float* y = NULL;
    const int got = in ? run_once(r, in, n_in, &y) : -1;
    int ok = y != NULL && got >= (int)(skip + MEASURE_N);
    if (ok) {
        DSD_MEMCPY(out, y + 2 * skip, (size_t)MEASURE_N * 2 * sizeof(float));
    }
    free(in);
    free(y);
    return ok;
}

/* A tone at @p f_in through a fresh copy of the case; the steady-state outputs land in @p out (MEASURE_N of them). */
static int
tone_through(int in_hz, int out_hz, int protect_hz, double f_in, float* out) {
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    if (dsd_iq_resampler_configure(&r, in_hz, out_hz, protect_hz, 4096) != DSD_IQ_RESAMPLER_OK) {
        return 0;
    }
    const int ok = tone_through_design(&r, f_in, out);
    dsd_iq_resampler_free(&r);
    return ok;
}

/* Runs @p n inputs through @p r in random cuts of 0 to @p max_chunk; returns the outputs written to @p out (room for
   @p cap), or (size_t)-1 when a call fails or the outputs would not fit. */
static size_t
run_cuts(dsd_iq_resampler* r, const float* in, size_t n, size_t max_chunk, float* out, size_t cap) {
    size_t pos = 0;
    size_t got = 0;
    while (pos < n) {
        size_t chunk = (size_t)(rng_next() % (uint32_t)(max_chunk + 1)); /* 0 included */
        if (chunk > n - pos) {
            chunk = n - pos;
        }
        const size_t bound = dsd_iq_resampler_max_out(r, chunk);
        if (got + bound > cap) {
            return (size_t)-1;
        }
        const int w = dsd_iq_resampler_process(r, in + 2 * pos, chunk, out + 2 * got, bound);
        if (w < 0 || (size_t)w > bound) {
            return (size_t)-1;
        }
        got += (size_t)w;
        pos += chunk;
    }
    return got;
}

/* The delay a slow tone at @p f_hz measures through @p r, fresh: each steady output m is the tone at time m / R - delay,
   so the phase residue gives the delay, in seconds. @p ok is set when enough outputs came out. */
static double
measure_delay_seconds(dsd_iq_resampler* r, double f_hz, int* ok) {
    const double in_hz = dsd_iq_resampler_in_rate_hz(r);
    const double R = actual_out_hz(r);
    const double gd = dsd_iq_resampler_group_delay_seconds(r);
    const size_t n_in = (size_t)((double)MEASURE_N * in_hz / R) + 64;
    float* in = make_tone(n_in, in_hz, f_hz, 0.5);
    float* out = NULL;
    const int got = in ? run_once(r, in, n_in, &out) : -1;
    const size_t skip = (size_t)(2.0 * gd * R) + 64;
    double re = 0.0;
    double im = 0.0;
    for (size_t m = skip; got > 0 && m < (size_t)got; m++) {
        const double ph = -2.0 * kPi * f_hz * (double)m / R;
        re += out[2 * m] * cos(ph) - out[2 * m + 1] * sin(ph);
        im += out[2 * m] * sin(ph) + out[2 * m + 1] * cos(ph);
    }
    *ok = got > (int)skip;
    free(in);
    free(out);
    return -atan2(im, re) / (2.0 * kPi * f_hz);
}

/* The prototype's response at @p f_hz (at the L-times rate), normalised so the passband is 1. The rotations are
   stepped by complex multiplies rather than a cosine per tap: the error stays far below the stopband. */
static double
prototype_gain(const dsd_iq_resampler* r, double f_hz) {
    const int L = r->L;
    const int T = r->taps_per_phase;
    const double step = -2.0 * kPi * f_hz / (dsd_iq_resampler_in_rate_hz(r) * (double)L);
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
                const size_t got = out ? run_cuts(&b, in, n, kMaxChunk[mc], out, (size_t)ref_n + 8) : (size_t)-1;
                CHECK(got == (size_t)ref_n && same_bits(out, ref, got * 2),
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
            for (size_t m = 0; m < (size_t)counts[0]; m++) {
                leak |= fabsf(outs[1][2U * m + 1U]) > 1e-30f || fabsf(outs[2][2U * m]) > 1e-30f;
                differ |= !same_bits(&outs[0][2U * m], &outs[1][2U * m], 1)
                          || !same_bits(&outs[0][2U * m + 1U], &outs[2][2U * m + 1U], 1);
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
            (void)run_once(&used, in + (size_t)2 * 17U, 1237, &junk);
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
        for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
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
        const double gd = dsd_iq_resampler_group_delay_seconds(&r);
        int ok = 0;
        const double measured = measure_delay_seconds(&r, 100.0, &ok);
        CHECK(ok && fabs(measured - gd) <= 1e-7, "%d->%d: measured delay %.9f s, reported %.9f s", k->in_hz, k->out_hz,
              measured, gd);
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

/* The live step's cases (issue #806): a device rate over 2^shift, the half-band chain's exact output, stepped to the
   DSP rate D, with the L/M and taps per phase measured for each in the #806 research. The device rate goes in as the
   numerator as is, not in lowest terms. */
typedef struct {
    const char* name;
    int64_t num;
    unsigned shift;
    int target_hz;
    int L;
    int M;
    int T;
} live_case;

static const live_case kLive[] = {
    {"R2 2.5M 4k", 2500000, 9, 4000, 512, 625, 52},   {"R2 2.5M 6k", 2500000, 8, 6000, 384, 625, 68},
    {"R2 2.5M 8k", 2500000, 8, 8000, 512, 625, 52},   {"R2 2.5M 12k", 2500000, 7, 12000, 384, 625, 68},
    {"R2 2.5M 16k", 2500000, 7, 16000, 512, 625, 52}, {"R2 2.5M 24k", 2500000, 6, 24000, 384, 625, 68},
    {"R2 2.5M 48k", 2500000, 5, 48000, 384, 625, 68}, {"R2 2.5M 72k", 2500000, 5, 72000, 576, 625, 44},
    {"R2 10M 4k", 10000000, 10, 4000, 256, 625, 100}, {"Mini 3M 4k", 3000000, 9, 4000, 256, 375, 60},
    {"Mini 3M 6k", 3000000, 8, 6000, 64, 125, 80},    {"Mini 3M 8k", 3000000, 8, 8000, 256, 375, 60},
    {"Mini 3M 12k", 3000000, 7, 12000, 64, 125, 80},  {"Mini 3M 16k", 3000000, 7, 16000, 256, 375, 60},
    {"Mini 3M 24k", 3000000, 6, 24000, 64, 125, 80},  {"Mini 3M 48k", 3000000, 5, 48000, 64, 125, 80},
    {"Mini 3M 72k", 3000000, 5, 72000, 96, 125, 52},  {"Soapy 2M 48k", 2000000, 5, 48000, 96, 125, 52},
    {"Soapy 2M 72k", 2000000, 4, 72000, 72, 125, 72},
};

#define N_LIVE (sizeof(kLive) / sizeof(kLive[0]))

/* The step's edges: the protected band 0.45 x D (whole Hz for any whole-kHz D), the stopband from D less that. */
static int
live_protect(const live_case* k) {
    return k->target_hz * 9 / 20;
}

static int
live_stop(const live_case* k) {
    return k->target_hz - live_protect(k);
}

static double
live_in_hz(const live_case* k) {
    return ldexp((double)k->num, -(int)k->shift);
}

static int
live_configure(dsd_iq_resampler* r, const live_case* k, size_t max_in) {
    return dsd_iq_resampler_configure_ratio(r, k->L, k->M, k->num, k->shift, live_protect(k), live_stop(k), max_in);
}

/* The whole-Hz design at the rates scaled by 2^shift: the device rate to D x 2^shift. */
static int
live_configure_scaled(dsd_iq_resampler* r, const live_case* k, size_t max_in) {
    return dsd_iq_resampler_configure(r, (int)k->num, k->target_hz << k->shift, live_protect(k) << k->shift, max_in);
}

static int
same_double(double a, double b) {
    uint64_t x = 0U;
    uint64_t y = 0U;
    DSD_MEMCPY(&x, &a, sizeof(x));
    DSD_MEMCPY(&y, &b, sizeof(y));
    return x == y;
}

/* Every field the same, the buffers by address. */
static int
same_resampler(const dsd_iq_resampler* a, const dsd_iq_resampler* b) {
    return a->configured == b->configured && a->in_hz == b->in_hz && a->out_hz == b->out_hz && a->L == b->L
           && a->M == b->M && a->taps_per_phase == b->taps_per_phase && a->in_rate_num == b->in_rate_num
           && a->in_rate_shift == b->in_rate_shift && a->keyed == b->keyed && a->protect_hz == b->protect_hz
           && a->stop_hz == b->stop_hz && same_double(a->passband_hz, b->passband_hz)
           && same_double(a->stopband_hz, b->stopband_hz) && a->max_in == b->max_in && a->phase == b->phase
           && a->bank == b->bank && a->work == b->work;
}

/* Whether @p r is released: nothing held, no design, no key. */
static int
is_released(const dsd_iq_resampler* r) {
    return !r->configured && r->bank == NULL && r->work == NULL && !r->keyed && r->L == 1 && r->M == 1;
}

/* Fills @p n complex samples of noise. */
static float*
make_noise(size_t n) {
    float* buf = (float*)malloc(n * 2 * sizeof(float));
    for (size_t i = 0; buf && i < n * 2; i++) {
        buf[i] = rng_float();
    }
    return buf;
}

/* The prototype's ripple to the passband edge and its worst gain from the stopband edge over the first 60 lobes. */
static void
prototype_ripple_and_stop(const dsd_iq_resampler* r, double* ripple_db, double* stop_db) {
    double gmin = 1e9;
    double gmax = 0.0;
    for (int i = 0; i <= 100; i++) {
        const double g = prototype_gain(r, r->passband_hz * (double)i / 100.0);
        gmin = fmin(gmin, g);
        gmax = fmax(gmax, g);
    }
    *ripple_db = fmax(db(gmax) - db(gmin), fmax(fabs(db(gmax)), fabs(db(gmin))));
    const double in_hz = dsd_iq_resampler_in_rate_hz(r);
    const double lobe = in_hz / (double)r->taps_per_phase;
    double worst = 0.0;
    for (int i = 0; i <= 600; i++) {
        const double f = r->stopband_hz + lobe * (double)i / 10.0;
        if (f > in_hz * (double)r->L / 2.0) {
            break;
        }
        worst = fmax(worst, prototype_gain(r, f));
    }
    *stop_db = db(worst);
}

/* Every live ratio: the L/M and taps measured, the exact rates and edges it reports, its own key, the scaled whole-Hz
   design bit for bit (taps and outputs), the delay and tail, and the prototype's ripple and stopband. */
static void
test_ratio_live_designs(void) {
    const size_t n = 4000;
    float* in = make_noise(n);
    if (!in) {
        CHECK(0, "alloc");
        return;
    }
    for (size_t c = 0; c < N_LIVE; c++) {
        const live_case* k = &kLive[c];
        dsd_iq_resampler r;
        dsd_iq_resampler w;
        dsd_iq_resampler_init(&r);
        dsd_iq_resampler_init(&w);
        const int st = live_configure(&r, k, 4096);
        const int st_scaled = live_configure_scaled(&w, k, 4096);
        CHECK(st == DSD_IQ_RESAMPLER_OK && st_scaled == DSD_IQ_RESAMPLER_OK, "%s: configure %d, scaled %d", k->name, st,
              st_scaled);
        if (st != DSD_IQ_RESAMPLER_OK || st_scaled != DSD_IQ_RESAMPLER_OK) {
            dsd_iq_resampler_free(&r);
            dsd_iq_resampler_free(&w);
            continue;
        }
        const double in_hz = live_in_hz(k);
        const int64_t unit = (int64_t)1 << k->shift;
        const int want_in = k->num % unit == 0 ? (int)(k->num / unit) : 0;
        CHECK(r.L == k->L && r.M == k->M && r.taps_per_phase == k->T, "%s: L/M %d/%d, %d taps, want %d/%d, %d", k->name,
              r.L, r.M, r.taps_per_phase, k->L, k->M, k->T);
        CHECK(same_double(dsd_iq_resampler_in_rate_hz(&r), in_hz), "%s: input rate %.6f, want %.6f", k->name,
              dsd_iq_resampler_in_rate_hz(&r), in_hz);
        CHECK(r.in_hz == want_in && r.out_hz == k->target_hz, "%s: in_hz %d out_hz %d, want %d and %d", k->name,
              r.in_hz, r.out_hz, want_in, k->target_hz);
        CHECK(fabs(actual_out_hz(&r) - (double)k->target_hz) <= 1e-9, "%s: output rate %.6f", k->name,
              actual_out_hz(&r));
        CHECK(fabs(dsd_iq_resampler_ratio_error_ppm(&r)) <= 1e-12, "%s: ratio error %.6f ppm", k->name,
              dsd_iq_resampler_ratio_error_ppm(&r));
        CHECK(fabs(r.passband_hz - (double)live_protect(k)) <= 1e-9
                  && fabs(r.stopband_hz - (double)live_stop(k)) <= 1e-9,
              "%s: edges %.3f/%.3f", k->name, r.passband_hz, r.stopband_hz);
        CHECK(dsd_iq_resampler_matches(&r, k->L, k->M, k->num, k->shift, live_protect(k), live_stop(k)),
              "%s: does not match its own key", k->name);

        const size_t bank_len = (size_t)r.L * (size_t)r.taps_per_phase;
        CHECK(w.L == r.L && w.M == r.M && w.taps_per_phase == r.taps_per_phase && same_bits(r.bank, w.bank, bank_len),
              "%s: taps differ from the scaled whole-Hz design", k->name);
        float* a = NULL;
        float* b = NULL;
        const int na = run_once(&r, in, n, &a);
        const int nb = run_once(&w, in, n, &b);
        CHECK(na > 0 && na == nb && same_bits(a, b, (size_t)na * 2), "%s: outputs %d vs scaled %d, or different bits",
              k->name, na, nb);
        free(a);
        free(b);

        const double T = (double)r.taps_per_phase;
        const double delay_in = ((double)r.L * T - 1.0) / (2.0 * (double)r.L);
        const double delay_s = dsd_iq_resampler_group_delay_seconds(&r);
        CHECK(fabs(dsd_iq_resampler_group_delay_inputs(&r) - delay_in) <= 1e-12, "%s: delay %.6f inputs, want %.6f",
              k->name, dsd_iq_resampler_group_delay_inputs(&r), delay_in);
        CHECK(fabs(delay_s - delay_in / in_hz) <= 1e-12, "%s: delay %.9f s, want %.9f", k->name, delay_s,
              delay_in / in_hz);
        CHECK(same_double(delay_s, ldexp(dsd_iq_resampler_group_delay_seconds(&w), (int)k->shift)),
              "%s: delay is not the scaled design's x 2^%u", k->name, k->shift);
        CHECK(dsd_iq_resampler_tail_inputs(&r) == (size_t)r.taps_per_phase - 1U, "%s: tail %zu", k->name,
              dsd_iq_resampler_tail_inputs(&r));

        double ripple = 0.0;
        double stop = 0.0;
        prototype_ripple_and_stop(&r, &ripple, &stop);
        CHECK(ripple <= DSD_IQ_RESAMPLER_RIPPLE_DB, "%s: prototype ripple %.4f dB", k->name, ripple);
        CHECK(stop <= -DSD_IQ_RESAMPLER_STOPBAND_DB, "%s: prototype stopband %.2f dB", k->name, stop);
        DSD_FPRINTF(stderr,
                    "%s: %.4f Hz -> %d Hz, L/M %d/%d, %d taps/phase, delay %.4f inputs = %.4f ms, tail %zu inputs, "
                    "ripple %.4f dB, stopband %.2f dB\n",
                    k->name, in_hz, k->target_hz, r.L, r.M, r.taps_per_phase, delay_in, delay_s * 1000.0,
                    dsd_iq_resampler_tail_inputs(&r), ripple, stop);
        dsd_iq_resampler_free(&r);
        dsd_iq_resampler_free(&w);
    }
    /* The headline bound: 2.5 MS/s stepped to 4 kHz delays by (512 x 52 - 1) / (2 x 512 x 4882.8125 Hz). */
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    CHECK(live_configure(&r, &kLive[0], 512) == DSD_IQ_RESAMPLER_OK, "R2 4k");
    CHECK(fabs(dsd_iq_resampler_group_delay_seconds(&r) - 26623.0 / 5e6) <= 1e-12, "R2 4k delay %.9f s",
          dsd_iq_resampler_group_delay_seconds(&r));
    dsd_iq_resampler_free(&r);
    free(in);
}

/* Streamed tones at each live ratio's fractional input rate: passband gain within the ripple, a tone leaving at the
   frequency it came in at (the output runs at D exactly), and input that folds straight into the protected band at D
   at least 60 dB down. */
static void
test_ratio_streamed(void) {
    float* y = (float*)malloc((size_t)MEASURE_N * 2 * sizeof(float));
    if (!y) {
        CHECK(0, "alloc");
        return;
    }
    for (size_t c = 0; c < N_LIVE; c++) {
        const live_case* k = &kLive[c];
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (live_configure(&r, k, 4096) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%s: configure", k->name);
            continue;
        }
        const double P = (double)live_protect(k);
        const double S = (double)live_stop(k);
        const double D = (double)k->target_hz;
        const double nyq = 0.49 * live_in_hz(k);
        double gmin = 1e9;
        double gmax = 0.0;
        static const double kPass[] = {0.0, 0.5, -0.5, 1.0, -1.0};
        for (size_t t = 0; t < sizeof(kPass) / sizeof(kPass[0]); t++) {
            const double f = kPass[t] * P;
            if (!tone_through_design(&r, f, y)) {
                CHECK(0, "%s: tone %.1f", k->name, f);
                continue;
            }
            const double g = tone_amplitude(y, MEASURE_N, D, f) / 0.5;
            gmin = fmin(gmin, g);
            gmax = fmax(gmax, g);
            if (t == 1) {
                const size_t q = MEASURE_N / 4;
                double ar = 0.0;
                double ai = 0.0;
                double br = 0.0;
                double bi = 0.0;
                for (size_t m = 0; m < q; m++) {
                    const size_t m3 = m + 3 * q;
                    const double pa = -2.0 * kPi * f * (double)m / D;
                    const double pb = -2.0 * kPi * f * (double)m3 / D;
                    ar += y[2 * m] * cos(pa) - y[2 * m + 1] * sin(pa);
                    ai += y[2 * m] * sin(pa) + y[2 * m + 1] * cos(pa);
                    br += y[2 * m3] * cos(pb) - y[2 * m3 + 1] * sin(pb);
                    bi += y[2 * m3] * sin(pb) + y[2 * m3 + 1] * cos(pb);
                }
                const double drift = atan2(bi * ar - br * ai, br * ar + bi * ai);
                const double f_err = drift * D / (2.0 * kPi * (double)(3 * q));
                CHECK(fabs(f_err) <= 1e-3, "%s: tone %.3f Hz left %.6f Hz off", k->name, f, f_err);
            }
        }
        CHECK(fabs(db(gmax)) <= DSD_IQ_RESAMPLER_RIPPLE_DB && fabs(db(gmin)) <= DSD_IQ_RESAMPLER_RIPPLE_DB,
              "%s: streamed gain %.4f..%.4f dB", k->name, db(gmin), db(gmax));
        /* Input from S up to the input's Nyquist folds straight into the protected band at D. */
        const double kFold[] = {S, -S, 0.5 * (S + nyq), -nyq};
        double worst = -400.0;
        int found = 0;
        for (size_t t = 0; t < sizeof(kFold) / sizeof(kFold[0]); t++) {
            const double f = kFold[t];
            const double at = fold(f, D);
            if (fabs(f) > nyq || fabs(f) < S || fabs(at) > P) {
                continue;
            }
            if (!tone_through_design(&r, f, y)) {
                CHECK(0, "%s: tone %.1f", k->name, f);
                continue;
            }
            const double lvl = db(tone_amplitude(y, MEASURE_N, D, at) / 0.5);
            CHECK(lvl <= -DSD_IQ_RESAMPLER_STOPBAND_DB, "%s: tone %.1f Hz folds to %.1f Hz at %.2f dB", k->name, f, at,
                  lvl);
            worst = fmax(worst, lvl);
            found++;
        }
        /* Stepping 78125 Hz to 72 kHz, the stopband starts past the input's Nyquist: nothing folds straight in. */
        CHECK(found > 0 || S >= nyq, "%s: no fold measured", k->name);
        DSD_FPRINTF(stderr, "%s: streamed passband %.4f..%.4f dB, worst direct fold %.2f dB (%d measured)\n", k->name,
                    db(gmin), db(gmax), worst, found);
        dsd_iq_resampler_free(&r);
    }
    free(y);
}

/* The delay a slow tone measures at fractional input rates matches the one reported in seconds, which would be off by
   2^shift if the resampler took the scaled rate. 40 Hz keeps the phase within +-pi up to 12.5 ms. */
static void
test_ratio_group_delay_measured(void) {
    static const size_t kCasesIdx[] = {0, 5, 12}; /* 4882.8125 Hz to 4 kHz (5.3 ms), 39062.5 to 24k, 23437.5 to 12k */
    for (size_t i = 0; i < sizeof(kCasesIdx) / sizeof(kCasesIdx[0]); i++) {
        const live_case* k = &kLive[kCasesIdx[i]];
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (live_configure(&r, k, 4096) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%s: configure", k->name);
            continue;
        }
        const double gd = dsd_iq_resampler_group_delay_seconds(&r);
        int ok = 0;
        const double measured = measure_delay_seconds(&r, 40.0, &ok);
        CHECK(ok && fabs(measured - gd) <= 1e-7, "%s: measured delay %.9f s, reported %.9f s", k->name, measured, gd);
        DSD_FPRINTF(stderr, "%s: measured delay %.6f ms, reported %.6f ms\n", k->name, measured * 1000.0, gd * 1000.0);
        dsd_iq_resampler_free(&r);
    }
}

/* The same rate written as different fractions makes the same design; 39062.5 Hz to 24 kHz matches the whole-Hz design
   at 2x and at 64x the rates bit for bit, and reports the delay at the real rate. */
static void
test_ratio_fraction_forms(void) {
    dsd_iq_resampler a;
    dsd_iq_resampler b;
    dsd_iq_resampler c;
    dsd_iq_resampler d;
    dsd_iq_resampler_init(&a);
    dsd_iq_resampler_init(&b);
    dsd_iq_resampler_init(&c);
    dsd_iq_resampler_init(&d);
    CHECK(dsd_iq_resampler_configure_ratio(&a, 384, 625, 78125, 1, 10800, 13200, 512) == DSD_IQ_RESAMPLER_OK, "a");
    CHECK(dsd_iq_resampler_configure_ratio(&b, 384, 625, 2500000, 6, 10800, 13200, 512) == DSD_IQ_RESAMPLER_OK, "b");
    CHECK(dsd_iq_resampler_configure(&c, 78125, 48000, 21600, 512) == DSD_IQ_RESAMPLER_OK, "c");
    CHECK(dsd_iq_resampler_configure(&d, 2500000, 1536000, 691200, 512) == DSD_IQ_RESAMPLER_OK, "d");
    if (a.configured && b.configured && c.configured && d.configured) {
        CHECK(a.in_rate_num == 78125 && a.in_rate_shift == 1U && b.in_rate_num == 78125 && b.in_rate_shift == 1U,
              "lowest terms: %lld/2^%u and %lld/2^%u", (long long)a.in_rate_num, a.in_rate_shift,
              (long long)b.in_rate_num, b.in_rate_shift);
        CHECK(a.in_hz == 0 && a.out_hz == 24000, "in_hz %d out_hz %d", a.in_hz, a.out_hz);
        CHECK(fabs(dsd_iq_resampler_in_rate_hz(&a) - 39062.5) <= 1e-12, "rate %.6f", dsd_iq_resampler_in_rate_hz(&a));
        const size_t len = (size_t)a.L * (size_t)a.taps_per_phase;
        CHECK(a.taps_per_phase == 68 && b.taps_per_phase == 68 && c.taps_per_phase == 68 && d.taps_per_phase == 68,
              "taps %d %d %d %d", a.taps_per_phase, b.taps_per_phase, c.taps_per_phase, d.taps_per_phase);
        CHECK(same_bits(a.bank, b.bank, len) && same_bits(a.bank, c.bank, len) && same_bits(a.bank, d.bank, len),
              "the banks differ");
        float* in = make_noise(7000);
        float* out[4] = {NULL, NULL, NULL, NULL};
        dsd_iq_resampler* rs[4] = {&a, &b, &c, &d};
        int counts[4] = {-1, -1, -1, -1};
        for (int i = 0; in && i < 4; i++) {
            counts[i] = run_once(rs[i], in, 7000, &out[i]);
        }
        for (int i = 1; i < 4; i++) {
            CHECK(counts[0] > 0 && counts[i] == counts[0] && same_bits(out[0], out[i], (size_t)counts[0] * 2),
                  "design %d: %d outputs vs %d, or different bits", i, counts[i], counts[0]);
        }
        for (int i = 0; i < 4; i++) {
            free(out[i]);
        }
        free(in);
        CHECK(same_double(dsd_iq_resampler_group_delay_seconds(&a), 2.0 * dsd_iq_resampler_group_delay_seconds(&c)),
              "delay %.9f s, want twice %.9f s", dsd_iq_resampler_group_delay_seconds(&a),
              dsd_iq_resampler_group_delay_seconds(&c));
        CHECK(dsd_iq_resampler_matches(&a, 384, 625, 2500000, 6, 10800, 13200)
                  && dsd_iq_resampler_matches(&b, 384, 625, 78125, 1, 10800, 13200),
              "equal fractions do not match");
        CHECK(!dsd_iq_resampler_matches(&a, 384, 625, 78125, 0, 10800, 13200), "78125 Hz matches 39062.5 Hz");
    }
    /* Whole rates written three ways. */
    dsd_iq_resampler_free(&a);
    dsd_iq_resampler_free(&b);
    CHECK(dsd_iq_resampler_configure_ratio(&a, 384, 625, 78125, 0, 21600, 26400, 512) == DSD_IQ_RESAMPLER_OK, "78125");
    CHECK(dsd_iq_resampler_configure_ratio(&b, 384, 625, 156250, 1, 21600, 26400, 512) == DSD_IQ_RESAMPLER_OK,
          "156250/2");
    CHECK(a.in_hz == 78125 && b.in_hz == 78125 && b.in_rate_num == 78125 && b.in_rate_shift == 0U,
          "whole rate in_hz %d/%d, %lld/2^%u", a.in_hz, b.in_hz, (long long)b.in_rate_num, b.in_rate_shift);
    CHECK(a.configured && c.configured && same_bits(a.bank, c.bank, (size_t)a.L * (size_t)a.taps_per_phase)
              && same_double(dsd_iq_resampler_group_delay_seconds(&a), dsd_iq_resampler_group_delay_seconds(&c)),
          "a whole-Hz ratio design differs from configure()'s");
    /* An output rate that is not whole is reported as 0. */
    CHECK(dsd_iq_resampler_configure_ratio(&b, 1, 3, 78125, 0, 10000, 16000, 512) == DSD_IQ_RESAMPLER_OK, "1/3");
    CHECK(b.in_hz == 78125 && b.out_hz == 0, "1/3: in_hz %d out_hz %d", b.in_hz, b.out_hz);
    dsd_iq_resampler_free(&a);
    dsd_iq_resampler_free(&b);
    dsd_iq_resampler_free(&c);
    dsd_iq_resampler_free(&d);
}

/* A design matches exactly its own L, M, input rate and edges; configure()'s designs and empty resamplers match
   nothing. */
static void
test_ratio_matches(void) {
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 0, 32400, 39600), "unconfigured matches");
    CHECK(!dsd_iq_resampler_matches(NULL, 576, 625, 78125, 0, 32400, 39600), "NULL matches");
    CHECK(dsd_iq_resampler_configure_ratio(&r, 576, 625, 78125, 0, 32400, 39600, 512) == DSD_IQ_RESAMPLER_OK, "ok");
    CHECK(dsd_iq_resampler_matches(&r, 576, 625, 78125, 0, 32400, 39600), "own key");
    CHECK(dsd_iq_resampler_matches(&r, 576, 625, 2500000, 5, 32400, 39600), "2.5 MS/s / 32");
    CHECK(!dsd_iq_resampler_matches(&r, 577, 625, 78125, 0, 32400, 39600), "L");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 626, 78125, 0, 32400, 39600), "M");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78126, 0, 32400, 39600), "rate");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 1, 32400, 39600), "shift");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 0, 32401, 39600), "protect");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 0, 32400, 39601), "stop");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 0, 0, 32400, 39600), "rate 0");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, -78125, 0, 32400, 39600), "negative rate");
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 64, 32400, 39600), "shift 64");
    CHECK(dsd_iq_resampler_configure(&r, 78125, 72000, 32400, 512) == DSD_IQ_RESAMPLER_OK, "configure");
    CHECK(r.L == 576 && r.M == 625 && !r.keyed, "configure() L/M %d/%d keyed %d", r.L, r.M, r.keyed);
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 0, 32400, 39600), "a configure() design matches");
    dsd_iq_resampler_free(&r);
    CHECK(!dsd_iq_resampler_matches(&r, 576, 625, 78125, 0, 32400, 39600), "freed matches");
}

/* A swap exchanges everything, history and phase included, with the same buffers: a stream that swaps out and back in
   carries on bit for bit, and each side frees only what it holds. */
static void
test_ratio_swap(void) {
    const size_t n = 6000;
    const size_t cut = 2501;
    float* in = make_noise(n);
    dsd_iq_resampler a;
    dsd_iq_resampler b;
    dsd_iq_resampler ref;
    dsd_iq_resampler_init(&a);
    dsd_iq_resampler_init(&b);
    dsd_iq_resampler_init(&ref);
    const live_case* ka = &kLive[6];  /* 78125 Hz to 48 kHz */
    const live_case* kb = &kLive[16]; /* 93750 Hz to 72 kHz */
    if (!in || live_configure(&a, ka, 512) != DSD_IQ_RESAMPLER_OK || live_configure(&b, kb, 512) != DSD_IQ_RESAMPLER_OK
        || live_configure(&ref, ka, 512) != DSD_IQ_RESAMPLER_OK) {
        CHECK(0, "setup");
        free(in);
        dsd_iq_resampler_free(&a);
        dsd_iq_resampler_free(&b);
        dsd_iq_resampler_free(&ref);
        return;
    }
    float* want = NULL;
    const int want_n = run_once(&ref, in, n, &want);
    float* got = (float*)malloc(((size_t)want_n + 8) * 2 * sizeof(float));
    int head = -1;
    if (got) {
        head = dsd_iq_resampler_process(&a, in, cut, got, dsd_iq_resampler_max_out(&a, cut));
    }
    const float* a_bank = a.bank;
    const float* a_work = a.work;
    const float* b_bank = b.bank;
    const float* b_work = b.work;
    const int a_phase = a.phase;
    dsd_iq_resampler_swap(&a, &b);
    CHECK(a.bank == b_bank && a.work == b_work && b.bank == a_bank && b.work == a_work, "swap moved the buffers");
    CHECK(a.L == kb->L && a.M == kb->M && b.L == ka->L && b.M == ka->M && b.phase == a_phase,
          "swap: L/M %d/%d and %d/%d, phase %d", a.L, a.M, b.L, b.M, b.phase);
    CHECK(dsd_iq_resampler_matches(&b, ka->L, ka->M, ka->num, ka->shift, live_protect(ka), live_stop(ka))
              && dsd_iq_resampler_matches(&a, kb->L, kb->M, kb->num, kb->shift, live_protect(kb), live_stop(kb)),
          "swap: keys");
    /* Swapped back after the other side ran a little, the first stream carries on where it was. */
    float junk[2 * 64];
    (void)dsd_iq_resampler_process(&a, in, 16, junk, 64);
    dsd_iq_resampler_swap(&b, &a);
    int tail = -1;
    if (got && head >= 0) {
        tail = dsd_iq_resampler_process(&a, in + 2 * cut, n - cut, got + 2 * (size_t)head,
                                        dsd_iq_resampler_max_out(&a, n - cut));
    }
    CHECK(head >= 0 && tail >= 0 && head + tail == want_n && same_bits(got, want, (size_t)want_n * 2),
          "a swapped stream gives %d + %d outputs, want %d, or different bits", head, tail, want_n);
    /* Swapping with itself or with NULL changes nothing. */
    dsd_iq_resampler before;
    DSD_MEMCPY(&before, &a, sizeof(before));
    dsd_iq_resampler_swap(&a, &a);
    dsd_iq_resampler_swap(&a, NULL);
    dsd_iq_resampler_swap(NULL, &a);
    CHECK(same_resampler(&before, &a), "a self or NULL swap changed the resampler");
    free(got);
    free(want);
    free(in);
    dsd_iq_resampler_free(&a);
    dsd_iq_resampler_free(&b);
    dsd_iq_resampler_free(&ref);
}

typedef struct {
    dsd_iq_resampler head;
    float* bank;
    float* work;
} resampler_snapshot;

static int
snapshot_take(resampler_snapshot* s, const dsd_iq_resampler* r) {
    const size_t bank_len = (size_t)r->L * (size_t)r->taps_per_phase;
    const size_t work_len = 2U * ((size_t)r->taps_per_phase - 1U + r->max_in);
    DSD_MEMCPY(&s->head, r, sizeof(*r));
    s->bank = (float*)malloc(bank_len * sizeof(float));
    s->work = (float*)malloc(work_len * sizeof(float));
    if (!s->bank || !s->work) {
        return 0;
    }
    DSD_MEMCPY(s->bank, r->bank, bank_len * sizeof(float));
    DSD_MEMCPY(s->work, r->work, work_len * sizeof(float));
    return 1;
}

static int
snapshot_same(const resampler_snapshot* s, const dsd_iq_resampler* r) {
    const size_t bank_len = (size_t)s->head.L * (size_t)s->head.taps_per_phase;
    const size_t work_len = 2U * ((size_t)s->head.taps_per_phase - 1U + s->head.max_in);
    return same_resampler(&s->head, r) && same_bits(s->bank, r->bank, bank_len)
           && same_bits(s->work, r->work, work_len);
}

static void
snapshot_free(resampler_snapshot* s) {
    free(s->bank);
    free(s->work);
}

/* Designing a candidate next to a running and a retained resampler: success or any refusal, the two are untouched, a
   refused candidate is left released, and the running stream carries on bit for bit. */
static void
test_ratio_candidate_isolation(void) {
    const size_t n = 5000;
    const size_t cut = 1500;
    float* in = make_noise(n);
    dsd_iq_resampler active;
    dsd_iq_resampler retained;
    dsd_iq_resampler ref;
    dsd_iq_resampler cand;
    dsd_iq_resampler_init(&active);
    dsd_iq_resampler_init(&retained);
    dsd_iq_resampler_init(&ref);
    dsd_iq_resampler_init(&cand);
    resampler_snapshot sa = {0};
    resampler_snapshot sr = {0};
    float* want = NULL;
    float* got = NULL;
    float junk[2 * 2048];
    int want_n = -1;
    int head = -1;
    int ok = in && live_configure(&active, &kLive[6], 512) == DSD_IQ_RESAMPLER_OK
             && live_configure(&retained, &kLive[16], 512) == DSD_IQ_RESAMPLER_OK
             && live_configure(&ref, &kLive[6], 512) == DSD_IQ_RESAMPLER_OK;
    if (ok) {
        want_n = run_once(&ref, in, n, &want);
        got = (float*)malloc(((size_t)want_n + 8) * 2 * sizeof(float));
        head = got ? dsd_iq_resampler_process(&active, in, cut, got, dsd_iq_resampler_max_out(&active, cut)) : -1;
        (void)dsd_iq_resampler_process(&retained, in, 1000, junk, 2048);
        ok = want_n > 0 && head >= 0 && snapshot_take(&sa, &active) && snapshot_take(&sr, &retained);
    }
    CHECK(ok, "setup");
    if (ok) {
        CHECK(live_configure(&cand, &kLive[7], 512) == DSD_IQ_RESAMPLER_OK, "candidate");
        CHECK(snapshot_same(&sa, &active) && snapshot_same(&sr, &retained), "a candidate design touched another");
        /* A candidate that held a design and is refused is released; so is a fresh one, for each refusal. */
        CHECK(dsd_iq_resampler_configure_ratio(&cand, 576, 625, 78125, 0, 32400, 32401, 512)
                  == DSD_IQ_RESAMPLER_ERR_TAPS,
              "taps");
        CHECK(is_released(&cand), "a refused candidate kept its design");
        CHECK(dsd_iq_resampler_configure_ratio(&cand, 576, 625, 78125, 0, 32400, 32400, 512)
                  == DSD_IQ_RESAMPLER_ERR_BAND,
              "band");
        CHECK(dsd_iq_resampler_configure_ratio(&cand, 768, 1250, 78125, 0, 32400, 39600, 512)
                  == DSD_IQ_RESAMPLER_ERR_ARGS,
              "args");
        CHECK(is_released(&cand), "a refused candidate is not released");
        CHECK(snapshot_same(&sa, &active) && snapshot_same(&sr, &retained), "a refused candidate touched another");
        const int tail = dsd_iq_resampler_process(&active, in + 2 * cut, n - cut, got + 2 * (size_t)head,
                                                  dsd_iq_resampler_max_out(&active, n - cut));
        CHECK(tail >= 0 && head + tail == want_n && same_bits(got, want, (size_t)want_n * 2),
              "the running stream changed: %d + %d outputs, want %d, or different bits", head, tail, want_n);
    }
    snapshot_free(&sa);
    snapshot_free(&sr);
    free(got);
    free(want);
    free(in);
    dsd_iq_resampler_free(&active);
    dsd_iq_resampler_free(&retained);
    dsd_iq_resampler_free(&ref);
    dsd_iq_resampler_free(&cand);
}

/* Random I/Q through one call and through random cuts, at the live step's pass size and smaller: the same bits; and a
   clear after that matches the first run again. */
static void
test_ratio_chunking(void) {
    const size_t n = 30000;
    float* in = make_noise(n);
    if (!in) {
        CHECK(0, "alloc");
        return;
    }
    static const size_t kIdx[] = {5, 7, 9, 0}; /* 39062.5 to 24k, 78125 to 72k, Mini 4k (256/375), R2 4k (512/625) */
    static const size_t kMaxIn[] = {512, 7, 1};
    static const size_t kMaxChunk[] = {1, 5, 333, 5000};
    for (size_t c = 0; c < sizeof(kIdx) / sizeof(kIdx[0]); c++) {
        const live_case* k = &kLive[kIdx[c]];
        dsd_iq_resampler a;
        dsd_iq_resampler_init(&a);
        if (live_configure(&a, k, 8192) != DSD_IQ_RESAMPLER_OK) {
            CHECK(0, "%s: configure", k->name);
            continue;
        }
        float* ref = NULL;
        const int ref_n = run_once(&a, in, n, &ref);
        CHECK(ref_n > 0, "%s: one call %d", k->name, ref_n);
        for (size_t mi = 0; ref_n > 0 && mi < sizeof(kMaxIn) / sizeof(kMaxIn[0]); mi++) {
            for (size_t mc = 0; mc < sizeof(kMaxChunk) / sizeof(kMaxChunk[0]); mc++) {
                dsd_iq_resampler b;
                dsd_iq_resampler_init(&b);
                if (live_configure(&b, k, kMaxIn[mi]) != DSD_IQ_RESAMPLER_OK) {
                    CHECK(0, "%s: configure", k->name);
                    continue;
                }
                float* out = (float*)malloc(((size_t)ref_n + 8) * 2 * sizeof(float));
                const size_t got = out ? run_cuts(&b, in, n, kMaxChunk[mc], out, (size_t)ref_n + 8) : (size_t)-1;
                CHECK(got == (size_t)ref_n && same_bits(out, ref, got * 2),
                      "%s: max_in %zu, cuts up to %zu: %zu outputs, want %d, or different bits", k->name, kMaxIn[mi],
                      kMaxChunk[mc], got, ref_n);
                dsd_iq_resampler_clear(&b);
                float* again = NULL;
                const int again_n = run_once(&b, in, n, &again);
                CHECK(again_n == ref_n && same_bits(again, ref, (size_t)ref_n * 2), "%s: cleared run differs", k->name);
                free(again);
                free(out);
                dsd_iq_resampler_free(&b);
            }
        }
        free(ref);
        dsd_iq_resampler_free(&a);
    }
    free(in);
}

/* Each refusal returns its reason and leaves the resampler released; the band rules hold exactly at their edges. */
static void
test_ratio_refusals(void) {
    typedef struct {
        int64_t num;
        size_t max_in;
        const char* what;
        int L;
        int M;
        unsigned shift;
        int protect;
        int stop;
        int want;
    } refusal;

    /* Rate numerator, max_in, the case, L, M, shift, protected edge, stopband edge, the status wanted. */
    static const refusal kRefusals[] = {
        {78125, 64, "L 0", 0, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "M 0", 384, 0, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "L -384", -384, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "L 4097", 4097, 4096, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "M 4097", 1, 4097, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "not in lowest terms", 768, 1250, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {0, 64, "rate 0", 384, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {-78125, 64, "rate -78125", 384, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {(int64_t)INT32_MAX + 1, 64, "rate past INT32_MAX", 384, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "shift", 384, 625, DSD_IQ_RESAMPLER_MAX_RATE_SHIFT + 1, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "protect 0", 384, 625, 0, 0, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "stop 0", 384, 625, 0, 21600, 0, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 0, "max_in 0", 384, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_ERR_ARGS},
        {78125, 64, "no transition band", 384, 625, 0, 21600, 21600, DSD_IQ_RESAMPLER_ERR_BAND},
        {78125, 64, "stopband past the output less P", 384, 625, 0, 21600, 26401, DSD_IQ_RESAMPLER_ERR_BAND},
        {78125, 64, "protect past 0.45 of the input", 1, 1, 0, 35157, 42968, DSD_IQ_RESAMPLER_ERR_BAND},
        {48000, 64, "stopband past the input less P", 3, 2, 0, 21000, 27001, DSD_IQ_RESAMPLER_ERR_BAND},
        {78125, 64, "1 Hz of transition", 576, 625, 0, 32400, 32401, DSD_IQ_RESAMPLER_ERR_TAPS},
    };
    static const refusal kEdges[] = {
        {78125, 64, "stopband at the output less P", 384, 625, 0, 21600, 26400, DSD_IQ_RESAMPLER_OK},
        {78125, 64, "protect at 0.45 of the input, S + P at the input", 1, 1, 0, 35156, 42969, DSD_IQ_RESAMPLER_OK},
        {48000, 64, "stopband at the input less P", 3, 2, 0, 21000, 27000, DSD_IQ_RESAMPLER_OK},
        {72000, 64, "1/1", 1, 1, 0, 21600, 26400, DSD_IQ_RESAMPLER_OK},
    };
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    CHECK(dsd_iq_resampler_configure_ratio(NULL, 384, 625, 78125, 0, 21600, 26400, 64) == DSD_IQ_RESAMPLER_ERR_ARGS,
          "NULL");
    for (size_t i = 0; i < sizeof(kRefusals) / sizeof(kRefusals[0]); i++) {
        const refusal* f = &kRefusals[i];
        /* Each one on a resampler that held a design: the refusal drops it. */
        CHECK(dsd_iq_resampler_configure_ratio(&r, 384, 625, 78125, 0, 21600, 26400, 64) == DSD_IQ_RESAMPLER_OK,
              "ok before %s", f->what);
        const int st =
            dsd_iq_resampler_configure_ratio(&r, f->L, f->M, f->num, f->shift, f->protect, f->stop, f->max_in);
        CHECK(st == f->want, "%s: %d, want %d", f->what, st, f->want);
        CHECK(is_released(&r), "%s: not released", f->what);
    }
    for (size_t i = 0; i < sizeof(kEdges) / sizeof(kEdges[0]); i++) {
        const refusal* f = &kEdges[i];
        const int st =
            dsd_iq_resampler_configure_ratio(&r, f->L, f->M, f->num, f->shift, f->protect, f->stop, f->max_in);
        CHECK(st == f->want && r.configured, "%s: %d", f->what, st);
    }
    CHECK(r.L == 1 && r.M == 1 && r.in_hz == 72000 && r.out_hz == 72000, "1/1: L/M %d/%d, %d to %d Hz", r.L, r.M,
          r.in_hz, r.out_hz);
    dsd_iq_resampler_free(&r);
    CHECK(is_released(&r), "freed");
    CHECK(fabs(dsd_iq_resampler_in_rate_hz(&r)) <= 1e-12 && fabs(dsd_iq_resampler_group_delay_inputs(&r)) <= 1e-12
              && dsd_iq_resampler_tail_inputs(&r) == 0U && dsd_iq_resampler_tail_inputs(NULL) == 0U,
          "unconfigured rate, delay and tail");
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
    test_ratio_live_designs();
    test_ratio_streamed();
    test_ratio_group_delay_measured();
    test_ratio_fraction_forms();
    test_ratio_matches();
    test_ratio_swap();
    test_ratio_candidate_isolation();
    test_ratio_chunking();
    test_ratio_refusals();
    if (g_failures) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DSP_IQ_RESAMPLER: all passed\n");
    return 0;
}
