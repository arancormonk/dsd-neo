// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The speech level AGC and the peak limiter (<dsd-neo/dsp/speech_agc.h>). Every case runs a committed, deterministic
 * signal: the synthetic speech, vowels, tones and noise of test_support/speech_synth.h, or the local generators below,
 * measured with the P.56 meter of test_support/p56.h. Bounds come from the prototype's measured results (established
 * speech, startup and the gain law, noise and tone rejection, rollback, partitions, rates). Run with -v to print the
 * measured values. The cases on 60-120 s of hiss run on their own with --long (DSP_SPEECH_AGC_LONG).
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p56.h"
#include "speech_synth.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_verbose = 0;

#define SAY(...)                                                                                                       \
    do {                                                                                                               \
        if (g_verbose) {                                                                                               \
            DSD_FPRINTF(stdout, __VA_ARGS__);                                                                          \
        }                                                                                                              \
    } while (0)

#define CEILING_INT16 29204.0f

/* Every buffer here holds at least a few samples and at most a few minutes at 48 kHz. */
#define MAX_SAMPLES   ((size_t)1 << 26)

static float*
alloc_f(size_t n) {
    assert(n > 0 && n <= MAX_SAMPLES);
    float* p = (float*)calloc(n, sizeof(float));
    assert(p != NULL);
    return p;
}

static uint8_t*
alloc_u8(size_t n) {
    assert(n > 0 && n <= MAX_SAMPLES);
    uint8_t* p = (uint8_t*)calloc(n, 1U);
    assert(p != NULL);
    return p;
}

/* @p s seconds at @p rate, in samples: every duration used is at least 10 ms at 8 kHz. */
static size_t
secs(int rate, double s) {
    const size_t n = (size_t)llround(s * (double)rate);
    assert(n >= 80U && n <= MAX_SAMPLES);
    return n;
}

/* The sample at @p s seconds (0 allowed). */
static size_t
at_secs(int rate, double s) {
    return s > 0.0 ? secs(rate, s) : 0U;
}

static float
clampf(float x, float lo, float hi) {
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

static int
frame_len(int rate) {
    return (int)lround(0.020 * (double)rate);
}

/* ------------------------------------------------------------------------------------------- local generators */

typedef struct {
    uint64_t s;
} rng_t;

static uint64_t
rng_next(rng_t* r) {
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 2685821657736338717ULL;
}

static double
rng_uniform(rng_t* r) {
    return ((double)(rng_next(r) >> 11) + 0.5) / 9007199254740992.0;
}

static double
rng_gauss(rng_t* r) {
    const double u1 = rng_uniform(r);
    const double u2 = rng_uniform(r);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/*
 * A voiced sound: harmonics at 1/h up to 3.4 kHz on an integrated pitch phase, each pitch period drawn in model time
 * (f0 times 1 + jitter x gauss) when it starts, so one seed gives one waveform at every rate. RMS @p rms_dbfs over
 * the signal itself.
 */
typedef struct {
    double f0;
    double jitter;
    uint64_t seed;
} voiced_params;

static void
synth_voiced(float* out, size_t n, int rate, const voiced_params* vp, double rms_dbfs) {
    rng_t r = {vp->seed * 0x9E3779B97F4A7C15ULL + 1U};
    assert(n > 0 && n <= MAX_SAMPLES);
    double* y = (double*)calloc(n, sizeof(double));
    assert(y != NULL);
    double epoch = 0.0;
    double period = 1.0 / (vp->f0 * (1.0 + vp->jitter * rng_gauss(&r)));
    const int nh = (int)(3400.0 / vp->f0) + 1;
    double sumsq = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)rate;
        while (t >= epoch + period) {
            epoch += period;
            period = 1.0 / (vp->f0 * (1.0 + vp->jitter * rng_gauss(&r)));
        }
        const double ph = 2.0 * M_PI * (t - epoch) / period;
        const double fnow = 1.0 / period;
        double v = 0.0;
        for (int h = 1; h <= nh; h++) {
            if ((double)h * fnow >= 3400.0) {
                break;
            }
            v += sin((double)h * ph) / (double)h;
        }
        y[i] = v;
        sumsq += v * v;
    }
    const double scale = 32768.0 * pow(10.0, rms_dbfs / 20.0) / sqrt(sumsq / (double)n + 1e-300);
    for (size_t i = 0; i < n; i++) {
        out[i] = (float)(y[i] * scale);
    }
    free(y);
}

static void
voiced_defaults(voiced_params* vp, double f0, double jitter, uint64_t seed) {
    DSD_MEMSET(vp, 0, sizeof *vp);
    vp->f0 = f0;
    vp->jitter = jitter;
    vp->seed = seed;
}

static void
add_into(float* dst, const float* src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] += src[i];
    }
}

static void
add_noise(float* dst, size_t n, int rate, uint64_t seed, dsd_test_noise_kind kind, double rms_dbfs) {
    float* w = alloc_f(n);
    assert(dsd_test_synth_noise(w, n, rate, seed, kind, rms_dbfs) == 0);
    add_into(dst, w, n);
    free(w);
}

static void
speech_into(float* out, size_t n, int rate, uint64_t seed, double level_dbfs, double f0, double start_s) {
    dsd_test_speech_params p;
    dsd_test_speech_defaults(&p, seed);
    p.level_dbfs = level_dbfs;
    p.f0_hz = f0;
    p.start_s = start_s;
    assert(dsd_test_synth_speech(out, n, rate, &p) == 0);
}

/* Open-squelch FM hiss as the analog chain's AGC sees it: white noise through a one-pole de-emphasis of time constant
   @p tau_s (0: none; 750 us is DSD_NEO_DEEMPH=nfm), then the monitor's own FM voice band-pass, at RMS @p rms_dbfs over
   the @p n samples. The filters run 50 ms before the first sample. */
static void
synth_receiver_hiss(float* x, size_t n, int rate, uint64_t seed, double tau_s, double rms_dbfs) {
    const size_t lead = secs(rate, 0.05);
    float* w = alloc_f(n + lead);
    rng_t r = {seed * 0x9E3779B97F4A7C15ULL + 1U};
    for (size_t i = 0; i < n + lead; i++) {
        w[i] = (float)rng_gauss(&r);
    }
    if (tau_s > 0.0) {
        assert(dsd_test_deemphasis(w, n + lead, rate, tau_s) == 0);
    }
    dsd_voice_bandpass bp;
    assert(dsd_voice_bandpass_design(&bp, DSD_VOICE_BAND_FM, rate) == 0);
    dsd_voice_bandpass_process(&bp, w, n + lead);
    double sumsq = 0.0;
    for (size_t i = 0; i < n; i++) {
        sumsq += (double)w[lead + i] * (double)w[lead + i];
    }
    const double scale = 32768.0 * pow(10.0, rms_dbfs / 20.0) / sqrt(sumsq / (double)n);
    for (size_t i = 0; i < n; i++) {
        x[i] = (float)((double)w[lead + i] * scale);
    }
    free(w);
}

/* --------------------------------------------------------------------------------------------- frame tracing */

typedef struct {
    int committed; /* frames this frame committed: 0, 1 or the run of 6 */
    int speech;    /* the frame passed both cues (a run grew or was turned away) */
    int rejected;
    int steps;
    float g_before;
    float g;
    float s;
    float e;
    float floor_db;
    float applied_db; /* gain_db() after the frame's last sample */
    float ctrl_db;    /* the controller gain (G + E, ramped) the frame's last sample got */
    uint32_t restarts;
    float level;                   /* the frame's level as the AGC measures it when every sample learns */
    int eligible;                  /* at least 9 in 10 of the frame's samples learned */
    dsd_speech_agc_learned before; /* the learned state before and after the frame */
    dsd_speech_agc_learned after;
} frame_rec;

typedef struct {
    frame_rec* f;
    size_t count;
    size_t cap;
} trace_t;

static void
trace_init(trace_t* t, size_t frames) {
    t->f = (frame_rec*)calloc(frames + 1U, sizeof(frame_rec));
    assert(t->f != NULL);
    t->count = 0;
    t->cap = frames + 1U;
}

static void
trace_free(trace_t* t) {
    free(t->f);
    t->f = NULL;
}

typedef struct {
    int learn;
    int decodable;
    int tone;
} flags_t;

static const flags_t k_learn = {1, 1, 0};

/* A frame's level as the AGC measures it when every sample learns: the float sum of squares in sample order. */
static float
frame_level_db(const float* x, size_t n) {
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) {
        s += x[i] * x[i];
    }
    return 10.0f * log10f(s / (float)n / (32768.0f * 32768.0f) + 1e-30f);
}

/* Whether a frame of @p m samples with these flags is eligible: at least 9 in 10 of its samples learn (a learn array,
   one flag per sample, or @p fl), and the gain is not fixed. */
static int
frame_eligible(const dsd_speech_agc* agc, const uint8_t* learn, size_t m, flags_t fl) {
    size_t learning = fl.learn && fl.decodable && !fl.tone ? m : 0U;
    if (learn) {
        learning = 0U;
        for (size_t i = 0; i < m; i++) {
            learning += learn[i] != 0U;
        }
    }
    return !agc->fixed && learning * 10U >= (size_t)agc->frame_len * 9U;
}

/* Process @p n samples, which start on a frame boundary, one frame per call, tracing each frame. A learn array (one
   flag per sample) goes through process_runs; otherwise @p fl applies. */
static void
run_traced(dsd_speech_agc* agc, float* buf, size_t n, const uint8_t* learn, flags_t fl, trace_t* t) {
    const size_t F = (size_t)agc->frame_len;
    for (size_t p = 0; p < n; p += F) {
        const size_t m = p + F <= n ? F : n - p;
        const dsd_speech_agc_learned before = agc->learned;
        const float level = frame_level_db(buf + p, m);
        const int eligible = frame_eligible(agc, learn ? learn + p : NULL, m, fl);
        if (learn) {
            dsd_speech_agc_process_runs(agc, buf + p, m, learn + p);
        } else {
            dsd_speech_agc_process(agc, buf + p, m, fl.learn, fl.decodable, fl.tone);
        }
        if (m < F || t == NULL || t->count >= t->cap) {
            continue;
        }
        frame_rec* r = &t->f[t->count++];
        const dsd_speech_agc_learned* d = &agc->learned;
        r->committed = d->commits - before.commits;
        r->rejected = (int)(d->rejections - before.rejections);
        r->speech = r->rejected > 0 || d->run_len > before.run_len;
        r->steps = d->steps;
        r->g_before = before.gain_db;
        r->g = d->gain_db;
        r->s = d->level_db;
        r->e = d->atten_db;
        r->floor_db = d->noise_floor_db;
        r->applied_db = dsd_speech_agc_gain_db(agc);
        r->ctrl_db = 20.0f * log10f(agc->g_last);
        r->restarts = d->restarts;
        r->level = level;
        r->eligible = eligible;
        r->before = before;
        r->after = *d;
    }
}

static int
init_agc(dsd_speech_agc* agc, int rate, float g0) {
    return dsd_speech_agc_init(agc, rate, g0, DSD_SPEECH_AGC_TARGET_DBFS, DSD_SPEECH_AGC_CEILING_DBFS);
}

static dsd_speech_agc*
new_agc(int rate, float g0) {
    dsd_speech_agc* agc = (dsd_speech_agc*)calloc(1U, sizeof *agc);
    assert(agc != NULL);
    assert(init_agc(agc, rate, g0) == 0);
    return agc;
}

/* An instance as the analog chain runs it: the coloured-noise veto on. */
static dsd_speech_agc*
new_analog_agc(int rate, float g0) {
    dsd_speech_agc* agc = new_agc(rate, g0);
    dsd_speech_agc_set_noise_veto(agc, 1);
    return agc;
}

/* An instance as the digital voice chain runs it: the acquisition stage on. */
static dsd_speech_agc*
new_acq_agc(int rate, float g0) {
    dsd_speech_agc* agc = new_agc(rate, g0);
    dsd_speech_agc_set_acquisition(agc, 1);
    return agc;
}

/* The frame (0-based) at which the committed-frame count first reaches @p frames, or -1. */
static long
frame_of_committed(const trace_t* t, long from, int frames) {
    int sum = 0;
    for (size_t i = (size_t)from; i < t->count; i++) {
        sum += t->f[i].committed;
        if (sum >= frames) {
            return (long)i;
        }
    }
    return -1;
}

static long
first_commit(const trace_t* t, long from) {
    return frame_of_committed(t, from, 1);
}

static int
count_commits(const trace_t* t, long from, long to) {
    int sum = 0;
    for (long i = from; i < to && i < (long)t->count; i++) {
        sum += t->f[i].committed;
    }
    return sum;
}

/* Frames in [from, to) that committed, alone or as part of a run of six. */
static int
count_committed_frames(const trace_t* t, long from, long to) {
    int c = 0;
    for (long i = from; i < to && i < (long)t->count; i++) {
        int in_batch = 0;
        for (long j = i; j < (long)t->count && j < i + DSD_SPEECH_AGC_RUN_FRAMES; j++) {
            in_batch |= t->f[j].committed == DSD_SPEECH_AGC_RUN_FRAMES && j - i < DSD_SPEECH_AGC_RUN_FRAMES;
        }
        c += t->f[i].committed == 1 || in_batch;
    }
    return c;
}

static int
same_bits(const void* a, const void* b, size_t n) {
    return memcmp(a, b, n) == 0;
}

static float
max_abs(const float* x, size_t n) {
    float m = 0.0f;
    for (size_t i = 0; i < n; i++) {
        const float a = fabsf(x[i]);
        m = a > m ? a : m;
    }
    return m;
}

static double
rms_dbfs(const float* x, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        s += (double)x[i] * (double)x[i];
    }
    return 10.0 * log10(s / (double)(n > 0 ? n : 1U) / (32768.0 * 32768.0) + 1e-30);
}

static double
p56(const float* x, size_t n, int rate) {
    return dsd_test_p56_level_dbfs(x, n, rate, NULL);
}

static int
cmp_float(const void* a, const void* b) {
    const float x = *(const float*)a;
    const float y = *(const float*)b;
    return (x > y) - (x < y);
}

static float
percentile(float* v, size_t n, double q) {
    qsort(v, n, sizeof(float), cmp_float);
    size_t k = (size_t)floor(q * (double)(n - 1U) + 0.5);
    return v[k < n ? k : n - 1U];
}

/* Established speech: output P.56 after the first 100 committed frames (2 s), and the p5..p95 swing of the applied
   gain over the same part. Returns 0 when the signal never got there or less than 1 s is left. */
typedef struct {
    double err_db;
    double swing_db;
    double gain_db;
    long frame;
} established_t;

static int
established(const trace_t* t, const float* out, size_t n, int rate, established_t* e) {
    const long k = frame_of_committed(t, 0, 100);
    const size_t F = (size_t)frame_len(rate);
    if (k < 0 || (size_t)(k + 1) * F + secs(rate, 1.0) > n) {
        return 0;
    }
    const size_t from = (size_t)(k + 1) * F;
    e->err_db = p56(out + from, n - from, rate) - (double)DSD_SPEECH_AGC_TARGET_DBFS;
    const size_t m = t->count - (size_t)(k + 1);
    float* g = alloc_f(m);
    for (size_t i = 0; i < m; i++) {
        g[i] = t->f[(size_t)k + 1U + i].applied_db;
    }
    const float p95 = percentile(g, m, 0.95);
    const float p5 = percentile(g, m, 0.05);
    e->swing_db = (double)(p95 - p5);
    e->gain_db = (double)percentile(g, m, 0.5);
    e->frame = k;
    free(g);
    return 1;
}

/* ----------------------------------------------------------------------------------------------------- tests */

static void
test_init_and_sizes(void) {
    dsd_speech_agc* agc = (dsd_speech_agc*)calloc(1U, sizeof *agc);
    assert(agc != NULL);
    assert(dsd_speech_agc_init(NULL, 8000, 0.0f, -24.0f, -1.0f) == -1);
    assert(init_agc(agc, DSD_SPEECH_AGC_MIN_RATE_HZ - 1, 0.0f) == -1);
    assert(init_agc(agc, DSD_SPEECH_AGC_MAX_RATE_HZ + 1, 0.0f) == -1);
    assert(init_agc(agc, 48000, NAN) == -1);
    assert(dsd_speech_agc_init(agc, 48000, 0.0f, INFINITY, -1.0f) == -1);
    assert(dsd_speech_agc_init(agc, 48000, 0.0f, -24.0f, 0.5f) == -1);
    assert(dsd_speech_agc_init(agc, 48000, 0.0f, -24.0f, NAN) == -1);
    assert(dsd_speech_agc_init(agc, 48000, 0.0f, -24.0f, -91.0f) == -1); /* under one int16 unit */
    assert(init_agc(agc, 48000, DSD_SPEECH_AGC_START_GAIN_MAX_DB + 0.5f) == -1);
    assert(init_agc(agc, 48000, -DSD_SPEECH_AGC_START_GAIN_MAX_DB - 0.5f) == -1);
    assert(init_agc(agc, 48000, INFINITY) == -1);
    assert(agc->frame_len == 0); /* refused inits leave the state alone */
    /* An uninitialised (zeroed) instance leaves samples alone. */
    float x[4] = {1000.0f, -2000.0f, 3000.0f, 4000.0f};
    dsd_speech_agc_process(agc, x, 4, 1, 1, 0);
    assert(x[0] == 1000.0f && x[3] == 4000.0f);
    /* Every supported rate fits the compile-time history. */
    for (int rate = DSD_SPEECH_AGC_MIN_RATE_HZ; rate <= DSD_SPEECH_AGC_MAX_RATE_HZ; rate += rate < 24000 ? 1 : 7) {
        assert(init_agc(agc, rate, 0.0f) == 0);
        assert(agc->frame_len == frame_len(rate));
        assert(agc->decim == (int)lroundf((float)rate / 8000.0f));
    }
    assert(init_agc(agc, DSD_SPEECH_AGC_MAX_RATE_HZ, 0.0f) == 0);
    assert(init_agc(agc, 8000, 0.0f) == 0);
    assert(agc->ceiling == 29204.0f);
    assert(agc->frame_len == 160 && agc->decim == 1 && agc->lag_lo == 20 && agc->lag_hi == 120);
    assert(init_agc(agc, 22050, 0.0f) == 0);
    assert(agc->frame_len == 441 && agc->decim == 3 && agc->lag_lo == 18 && agc->lag_hi == 110);
    assert(init_agc(agc, 48000, 0.0f) == 0);
    assert(agc->frame_len == 960 && agc->decim == 6 && agc->lag_lo == 20 && agc->lag_hi == 120);
    SAY("state: %zu bytes\n", sizeof(dsd_speech_agc));
    free(agc);
}

/* One talker of the synthetic set at @p rate: 20 s from reset, frame by frame. */
typedef struct {
    int rate;
    float g0;
    double f0;
    double level;
    uint64_t seed;
} talker_t;

static const talker_t k_talkers[] = {
    {8000, 0.0f, 95.0, -34.0, 1},      {8000, 0.0f, 105.0, -27.0, 2},     {8000, 0.0f, 115.0, -20.0, 3},
    {8000, 0.0f, 125.0, -37.0, 4},     {8000, 0.0f, 140.0, -30.0, 5},     {8000, 0.0f, 155.0, -23.0, 6},
    {8000, 0.0f, 170.0, -17.0, 7},     {8000, 0.0f, 185.0, -33.0, 8},     {8000, 0.0f, 200.0, -26.0, 9},
    {8000, 0.0f, 215.0, -19.0, 10},    {8000, 0.0f, 230.0, -36.0, 11},    {8000, 0.0f, 245.0, -29.0, 12},
    {8000, 0.0f, 260.0, -22.0, 13},    {8000, 0.0f, 280.0, -31.0, 14},    {8000, 0.0f, 110.0, -24.0, 15},
    {8000, 0.0f, 210.0, -35.0, 16},    {22050, 0.0f, 130.0, -28.0, 17},   {22050, 0.0f, 220.0, -21.0, 18},
    {48000, 8.537f, 120.0, -32.0, 19}, {48000, 8.537f, 200.0, -38.0, 20},
};

typedef struct {
    double first_err;
    double first_bound;
    long first_frame;
    long first_speech;
    established_t est;
    int law_bad;
    int held_bad;
    int steps;
    int first_batch;
} talker_result;

/* The gain law on every frame: a committing frame steps G by clamp(want - G, -dn, +up), want = clamp(T - S, G0 - 12,
   G0 + 30), up = dn = 2 dB for the first 20 steps, then +0.12 / -0.20; every other frame leaves G alone. */
static void
check_gain_law(const dsd_speech_agc* agc, const trace_t* t, talker_result* r) {
    for (size_t i = 0; i < t->count; i++) {
        const frame_rec* f = &t->f[i];
        const int prev_steps = i > 0 ? t->f[i - 1].steps : 0;
        if (f->committed == 0) {
            r->held_bad += fabsf(f->g - f->g_before) > 0.0f || f->steps != prev_steps;
            continue;
        }
        r->steps++;
        float want = agc->target_db - f->s;
        const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
        const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
        want = clampf(want, lo, hi);
        const float up = prev_steps < 20 ? 2.0f : 0.12f;
        const float dn = prev_steps < 20 ? 2.0f : 0.20f;
        const float dg = clampf(want - f->g_before, -dn, up);
        r->law_bad += fabsf((f->g - f->g_before) - dg) > 2e-4f || f->steps != prev_steps + 1;
    }
}

static void
run_talker(const talker_t* tk, talker_result* r) {
    DSD_MEMSET(r, 0, sizeof *r);
    const size_t n = secs(tk->rate, 20.0);
    float* in = alloc_f(n);
    float* out = alloc_f(n);
    speech_into(in, n, tk->rate, tk->seed, tk->level, tk->f0, 0.0);
    DSD_MEMCPY(out, in, n * sizeof(float));
    dsd_speech_agc* agc = new_agc(tk->rate, tk->g0);
    trace_t t;
    trace_init(&t, n / (size_t)agc->frame_len);
    run_traced(agc, out, n, NULL, k_learn, &t);
    assert(established(&t, out, n, tk->rate, &r->est));
    check_gain_law(agc, &t, r);
    /* Startup: the first commit is the run of six on the sixth consecutive speech frame, and moves the gain no further
       than toward the established gain. */
    r->first_frame = first_commit(&t, 0);
    assert(r->first_frame >= 5);
    r->first_batch = t.f[r->first_frame].committed;
    r->first_speech = -1;
    for (long i = 0; i <= r->first_frame; i++) {
        if (t.f[i].speech) {
            r->first_speech = i;
            break;
        }
    }
    for (long i = r->first_frame - 5; i <= r->first_frame; i++) {
        assert(t.f[i].speech);
    }
    /* As the prototype measured it: the gain at the committing frame's last sample, before its ramp. */
    r->first_err = fabs((double)t.f[r->first_frame].ctrl_db - r->est.gain_db);
    r->first_bound = fabs((double)tk->g0 - r->est.gain_db);
    assert(max_abs(out, n) <= CEILING_INT16);
    trace_free(&t);
    free(agc);
    free(in);
    free(out);
}

/* Established speech on the synthetic talker set (pitches 95-280 Hz, levels -38 to -17 dBFS, 8, 22.05 and 48 kHz):
   every talker within 3 dB of the target, the set's spread around its median within the corpus bounds (95% within
   2 dB, 90% within 1.5 dB), the swing within 9 dB (95% within 6.5 dB); the gain law on every frame; and the startup. */
static void
test_established_talkers(void) {
    const size_t count = sizeof k_talkers / sizeof k_talkers[0];
    talker_result r[sizeof k_talkers / sizeof k_talkers[0]];
    float err[sizeof k_talkers / sizeof k_talkers[0]];
    for (size_t i = 0; i < count; i++) {
        run_talker(&k_talkers[i], &r[i]);
        err[i] = (float)r[i].est.err_db;
    }
    const float median = percentile(err, count, 0.5);
    int within2 = 0;
    int within15 = 0;
    int swing65 = 0;
    for (size_t i = 0; i < count; i++) {
        SAY("talker %2zu (%5d Hz, f0 %3.0f, %+5.1f dBFS): established %+6.2f dB, swing %4.2f dB, gain %+6.2f dB; "
            "first commit frame %ld (first speech %ld, batch %d), |err| %.2f <= %.2f; %d steps, law %d/%d bad\n",
            i, k_talkers[i].rate, k_talkers[i].f0, k_talkers[i].level, r[i].est.err_db, r[i].est.swing_db,
            r[i].est.gain_db, r[i].first_frame, r[i].first_speech, r[i].first_batch, r[i].first_err, r[i].first_bound,
            r[i].steps, r[i].law_bad, r[i].held_bad);
        const double spread = fabs(r[i].est.err_db - (double)median);
        assert(fabs(r[i].est.err_db) <= 3.0);
        assert(r[i].est.swing_db <= 9.0);
        within2 += spread <= 2.0;
        within15 += spread <= 1.5;
        swing65 += r[i].est.swing_db <= 6.5;
        assert(r[i].law_bad == 0 && r[i].held_bad == 0 && r[i].steps > 100);
        assert(r[i].first_batch == DSD_SPEECH_AGC_RUN_FRAMES);
        assert(r[i].first_frame - r[i].first_speech >= DSD_SPEECH_AGC_RUN_FRAMES - 1);
        assert(r[i].first_err <= r[i].first_bound + 1e-3);
    }
    SAY("talkers: median %+.2f dB; spread within 2.0 dB %d/%zu, within 1.5 dB %d; swing <= 6.5 dB %d\n", (double)median,
        within2, count, within15, swing65);
    assert(within2 * 100 >= 95 * (int)count);
    assert(within15 * 100 >= 90 * (int)count);
    assert(swing65 * 100 >= 95 * (int)count);
}

/* The first commit's ramp: the gain holds through the sixth speech frame's last sample, moves from the next sample,
   and reaches the new gain one frame later. */
static void
test_first_commit_ramp(void) {
    const int rate = 8000;
    const size_t n = secs(rate, 4.0);
    float* in = alloc_f(n);
    speech_into(in, n, rate, 21, -38.0, 150.0, 0.0);
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    const size_t F = (size_t)agc->frame_len;
    size_t p = 0;
    while (p + F <= n && agc->learned.commits == 0) {
        dsd_speech_agc_process(agc, in + p, F, 1, 1, 0);
        p += F;
    }
    assert(agc->learned.commits == DSD_SPEECH_AGC_RUN_FRAMES);
    const float before = agc->g_last;
    assert(fabsf(log10f(before)) < 1e-6f); /* the start gain through the committing frame */
    const float want = agc->learned.gain_db + agc->learned.atten_db;
    assert(fabsf(want - 2.0f) < 1e-5f); /* one start-up step up */
    float prev = before;
    for (size_t i = 0; i < F; i++) {
        dsd_speech_agc_process(agc, in + p + i, 1, 1, 1, 0);
        assert(agc->g_last > prev); /* moves from the first sample on */
        prev = agc->g_last;
    }
    assert(fabsf(20.0f * log10f(agc->g_last) - want) < 1e-4f);
    SAY("first commit at sample %zu (frame %zu), ramp %.4f -> %.4f dB over %zu samples\n", p, p / F,
        20.0 * log10((double)before), 20.0 * log10((double)agc->g_last), F);
    free(agc);
    free(in);
}

/* Run @p in (learning throughout) from reset, with the coloured-noise veto when @p veto and the acquisition stage when
   @p acq; returns the frame trace. */
static void
run_fresh_mode(int rate, float g0, int veto, int acq, float* buf, size_t n, trace_t* t) {
    dsd_speech_agc* agc = veto ? new_analog_agc(rate, g0) : new_agc(rate, g0);
    dsd_speech_agc_set_acquisition(agc, acq);
    trace_init(t, n / (size_t)agc->frame_len);
    run_traced(agc, buf, n, NULL, k_learn, t);
    free(agc);
}

static void
run_fresh_veto(int rate, float g0, int veto, float* buf, size_t n, trace_t* t) {
    run_fresh_mode(rate, g0, veto, 0, buf, n, t);
}

static void
run_fresh(int rate, float g0, float* buf, size_t n, trace_t* t) {
    run_fresh_veto(rate, g0, 0, buf, n, t);
}

/* A reception that starts inside quiet speech, or after exact zeros, commits within half a second of speech. */
static void
test_start_inside_speech_and_after_zeros(void) {
    static const int rates[] = {8000, 48000};
    /* The first time after 3 s at which the talker has been above -48 dBFS for 100 ms (8 kHz, 20 ms frames). */
    double inside_s = -1.0;
    {
        const size_t n = secs(8000, 8.0);
        float* x = alloc_f(n);
        speech_into(x, n, 8000, 31, -42.0, 120.0, 0.0);
        int loud = 0;
        for (size_t f = 150; f * 160U + 160U <= n && inside_s < 0.0; f++) {
            loud = rms_dbfs(x + f * 160U, 160U) > -48.0 ? loud + 1 : 0;
            inside_s = loud >= 5 ? (double)(f + 1U) * 0.02 : -1.0;
        }
        free(x);
        assert(inside_s > 3.0);
    }
    for (size_t ri = 0; ri < 2; ri++) {
        const int rate = rates[ri];
        const size_t n = secs(rate, 6.0);
        float* x = alloc_f(n);
        trace_t t;
        /* Inside: start where the talker is 100 ms into a loud stretch of a syllable. */
        speech_into(x, n, rate, 31, -42.0, 120.0, inside_s);
        assert(rms_dbfs(x, (size_t)frame_len(rate)) > -50.0);
        run_fresh(rate, 0.0f, x, n, &t);
        const long k1 = first_commit(&t, 0);
        SAY("%d Hz: start inside quiet speech: first commit at frame %ld (%.2f s)\n", rate, k1,
            0.02 * (double)(k1 + 1));
        assert(k1 >= DSD_SPEECH_AGC_RUN_FRAMES - 1 && k1 < 25);
        trace_free(&t);
        /* After 2 s of exact zeros. */
        const size_t z = secs(rate, 2.0);
        DSD_MEMSET(x, 0, n * sizeof(float));
        speech_into(x + z, n - z, rate, 32, -42.0, 120.0, 0.0);
        run_fresh(rate, 0.0f, x, n, &t);
        const long k2 = first_commit(&t, 0);
        SAY("%d Hz: after 2 s of zeros: first commit at %.2f s into the speech\n", rate, 0.02 * (double)(k2 + 1) - 2.0);
        assert(k2 >= 100 + DSD_SPEECH_AGC_RUN_FRAMES - 1 && k2 < 100 + 25);
        trace_free(&t);
        free(x);
    }
}

/* Natural vowels of 300 ms to 1 s are speech, alone at the start of a reception and in the middle of speech; a
   perfectly steady vowel is a tone within 12 frames of steady sound. The same with the coloured-noise veto when
   @p veto: voiced sound keeps its pitch peaks through the veto's pre-emphasis. */
static void
test_vowels(int veto) {
    static const int rates[] = {8000, 48000};
    static const double durs[] = {0.3, 0.5, 1.0};
    for (size_t ri = 0; ri < 2; ri++) {
        const int rate = rates[ri];
        for (size_t di = 0; di < 3; di++) {
            for (int mid = 0; mid <= 1; mid++) {
                const size_t lead = mid ? secs(rate, 4.0) : 0U;
                const size_t n = lead + secs(rate, durs[di] + 0.5);
                float* x = alloc_f(n);
                if (mid) {
                    speech_into(x, lead, rate, 40 + di, -30.0, 120.0, 0.0);
                }
                dsd_test_vowel_params v;
                dsd_test_vowel_defaults(&v, 41 + di);
                v.duration_s = durs[di];
                v.level_dbfs = -36.0;
                assert(dsd_test_synth_vowel(x + lead, n - lead, rate, &v) == 0);
                trace_t t;
                run_fresh_veto(rate, 0.0f, veto, x, n, &t);
                const long from = (long)(lead / (size_t)frame_len(rate));
                const int c = count_commits(&t, from, (long)t.count);
                const int frames = (int)lround(durs[di] / 0.02);
                SAY("%d Hz%s natural vowel %.1f s %s: %d of %d frames committed\n", rate, veto ? " veto" : "", durs[di],
                    mid ? "mid" : "alone", c, frames);
                assert(c >= frames / 2);
                trace_free(&t);
                free(x);
            }
        }
        /* Steady vowel, 1 s, alone and mid-speech. */
        for (int mid = 0; mid <= 1; mid++) {
            const size_t lead = mid ? secs(rate, 4.0) : 0U;
            const size_t n = lead + secs(rate, 1.5);
            float* x = alloc_f(n);
            if (mid) {
                speech_into(x, lead, rate, 45, -30.0, 120.0, 0.0);
            }
            dsd_test_vowel_params v;
            dsd_test_vowel_defaults(&v, 46);
            v.duration_s = 1.0;
            v.level_dbfs = -36.0;
            v.steady = 1;
            assert(dsd_test_synth_vowel(x + lead, n - lead, rate, &v) == 0);
            trace_t t;
            run_fresh_veto(rate, 0.0f, veto, x, n, &t);
            const long from = (long)(lead / (size_t)frame_len(rate));
            int c = 0;
            int rej = 0;
            long first = -1;
            long last = -1;
            for (long i = from; i < (long)t.count; i++) {
                if (t.f[i].speech && first < 0) {
                    first = i;
                }
                if (t.f[i].committed > 0) {
                    last = i;
                }
                c += t.f[i].committed;
                rej += t.f[i].rejected;
            }
            SAY("%d Hz%s steady vowel 1 s %s: %d frames committed, %d rejections, last commit %ld frames in\n", rate,
                veto ? " veto" : "", mid ? "mid" : "alone", c, rej, last - first);
            /* The run may include the vowel's onset frame, so the 12 steady frames end one frame later. */
            assert(rej >= 1 && c <= DSD_SPEECH_AGC_STATIONARY_FRAMES);
            assert(last - first <= DSD_SPEECH_AGC_STATIONARY_FRAMES - 1);
            trace_free(&t);
            free(x);
        }
    }
}

typedef enum { NS_TONE, NS_HUM, NS_BUZZ, NS_HETERODYNE, NS_DTMF } nonspeech_kind;

static void
synth_nonspeech(float* x, size_t n, int rate, nonspeech_kind k) {
    switch (k) {
        case NS_TONE: assert(dsd_test_synth_tone(x, n, rate, 1000.0, -30.0) == 0); break;
        case NS_HUM: assert(dsd_test_synth_mains(x, n, rate, DSD_TEST_MAINS_HUM, 60.0, -30.0) == 0); break;
        case NS_BUZZ: assert(dsd_test_synth_mains(x, n, rate, DSD_TEST_MAINS_BUZZ, 50.0, -30.0) == 0); break;
        case NS_HETERODYNE: assert(dsd_test_synth_heterodyne(x, n, rate, 1200.0, 2.0, -30.0) == 0); break;
        case NS_DTMF: assert(dsd_test_synth_dtmf(x, n, rate, '5', -30.0) == 0); break;
    }
}

/* Steady sounds commit at most until the 12-frame tone test turns them away (within 12 frames of steady sound; the
   run may also hold its onset frame), opening a reception or mid-call, at 8, 22.05 and 48 kHz; a tone opening a
   reception moves the gain by 6 dB, or 7 with its onset frame. */
static void
test_steady_sounds_rejected(void) {
    static const int rates[] = {8000, 22050, 48000};
    static const char* names[] = {"tone 1 kHz", "hum 60 Hz", "buzz 50 Hz", "heterodyne 1.2 kHz +2 Hz/s", "DTMF 5"};
    for (size_t ri = 0; ri < 3; ri++) {
        const int rate = rates[ri];
        for (int k = NS_TONE; k <= NS_DTMF; k++) {
            for (int mid = 0; mid <= 1; mid++) {
                const size_t lead = mid ? secs(rate, 5.0) : secs(rate, 0.5);
                const size_t n = lead + secs(rate, 3.0);
                float* x = alloc_f(n);
                if (mid) {
                    speech_into(x, lead, rate, 50 + (uint64_t)k, -30.0, 140.0, 0.0);
                }
                synth_nonspeech(x + lead, n - lead, rate, (nonspeech_kind)k);
                trace_t t;
                run_fresh(rate, 0.0f, x, n, &t);
                const long from = (long)(lead / (size_t)frame_len(rate));
                long first = -1;
                long last = -1;
                int steps = 0;
                for (long i = from; i < (long)t.count; i++) {
                    first = (first < 0 && t.f[i].speech) ? i : first;
                    last = t.f[i].committed > 0 ? i : last;
                    steps += t.f[i].committed > 0;
                }
                const float dg = t.f[t.count - 1].g - t.f[from - 1].g;
                SAY("%5d Hz %-27s %s: %2d committing frames, the last %ld frames into it, G %+5.2f dB\n", rate,
                    names[k], mid ? "mid  " : "start", steps, last >= 0 ? last - first : -1L, (double)dg);
                /* A speech run may carry into the sound mid-call; either way nothing commits after its 12th frame. */
                assert(steps <= DSD_SPEECH_AGC_STATIONARY_FRAMES);
                assert(last < 0 || last - first <= DSD_SPEECH_AGC_STATIONARY_FRAMES - 1);
                /* At most the run of six and six single commits, one start-up step (2 dB) each. */
                assert(fabsf(dg) <= 14.0f);
                trace_free(&t);
                free(x);
            }
        }
    }
}

/* Voiced sound with a 1% jitter near the lag range's edges is speech (70 Hz: 14.3 ms; 380-420 Hz around 2.5 ms), and
   so is 120 Hz; a correlation still rising at 15 ms (60 Hz) and rumble are not, at 8, 22.05 and 48 kHz. The same with
   the coloured-noise veto when @p veto. */
static void
test_pitch_edges_and_rumble(int veto) {
    static const int rates[] = {8000, 22050, 48000};
    static const double accept[] = {70.0, 380.0, 400.0, 420.0, 120.0};
    const size_t n_accept = sizeof accept / sizeof accept[0];
    for (size_t ri = 0; ri < 3; ri++) {
        const int rate = rates[ri];
        const size_t lead = secs(rate, 0.5);
        const size_t n = lead + secs(rate, 2.0);
        const long from = (long)(lead / (size_t)frame_len(rate)) + 3;
        for (size_t ai = 0; ai < n_accept + 2; ai++) {
            float* x = alloc_f(n);
            const char* name;
            if (ai < n_accept) {
                voiced_params vp;
                voiced_defaults(&vp, accept[ai], 0.01, 60 + ai);
                synth_voiced(x + lead, n - lead, rate, &vp, -30.0);
                name = "voiced";
            } else if (ai == n_accept) {
                voiced_params vp;
                voiced_defaults(&vp, 60.0, 0.01, 65);
                synth_voiced(x + lead, n - lead, rate, &vp, -30.0);
                name = "voiced 60 Hz";
            } else {
                assert(dsd_test_synth_noise(x + lead, n - lead, rate, 66, DSD_TEST_NOISE_RUMBLE, -30.0) == 0);
                name = "rumble";
            }
            trace_t t;
            run_fresh_veto(rate, 0.0f, veto, x, n, &t);
            const int c = count_committed_frames(&t, from, (long)t.count);
            const int frames = (int)t.count - (int)from;
            SAY("%5d Hz%s %-12s %3.0f Hz: %3d of %3d frames committed\n", rate, veto ? " veto" : "", name,
                ai < n_accept ? accept[ai] : 0.0, c, frames);
            if (ai < n_accept) {
                assert(c * 10 >= frames * 9);
            } else {
                assert(c == 0);
            }
            trace_free(&t);
            free(x);
        }
    }
}

/* Continuous voiced sound quieter than the hiss before it, with no gap (the prototype's glide vowel): the pitch glides
   130 +- 30 Hz (1.5 cycles over 8 s) with a 1% jitter drawn every 1/120 s, 25 harmonics at 1/h through a resonance
   at 700 Hz, the level wobbling +-1.5 dB at 1.3 Hz, at -35.5 dBFS, over a -60 dBFS residual hiss. Model time, so one
   seed gives one waveform at every rate. */
static void
synth_glide(float* x, size_t n, int rate, uint64_t seed) {
    rng_t r = {seed * 0x9E3779B97F4A7C15ULL + 1U};
    double w[26];
    for (int h = 1; h <= 25; h++) {
        const double q = (double)h * 130.0 / 700.0;
        const double d = (double)h * 130.0 * 90.0 / (700.0 * 700.0);
        w[h] = (1.0 / (double)h) / sqrt(((1.0 - q * q) * (1.0 - q * q)) + d * d);
    }
    double ph = 0.0;
    double jit = 1.0;
    long block = -1;
    double sumsq = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)rate;
        if ((long)floor(t * 120.0) != block) {
            block = (long)floor(t * 120.0);
            jit = 1.0 + 0.01 * rng_gauss(&r);
        }
        ph += 2.0 * M_PI * (130.0 + 30.0 * sin(2.0 * M_PI * t * 1.5 / 8.0)) * jit / (double)rate;
        double v = 0.0;
        for (int h = 1; h <= 25; h++) {
            v += w[h] * sin((double)h * ph);
        }
        x[i] = (float)v;
        sumsq += v * v;
    }
    const double scale = 32768.0 * pow(10.0, -35.5 / 20.0) / sqrt(sumsq / (double)n);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)rate;
        x[i] = (float)((double)x[i] * scale * pow(10.0, 1.5 * sin(2.0 * M_PI * 1.3 * t) / 20.0));
    }
    add_noise(x, n, rate, seed + 1000U, DSD_TEST_NOISE_VOICE_BAND, -60.0);
}

static dsd_speech_agc*
clone_agc(const dsd_speech_agc* agc) {
    dsd_speech_agc* c = (dsd_speech_agc*)malloc(sizeof *c);
    assert(c != NULL);
    DSD_MEMCPY(c, agc, sizeof *c);
    return c;
}

/* Continue @p agc (on a frame boundary) with the glide vowel scaled by @p scale; returns the first commit, in frames
   into it, and the floor restarts of its first frame. */
static long
glide_first_commit(dsd_speech_agc* agc, int rate, float scale, uint32_t* restarts_first) {
    const size_t n = secs(rate, 3.0);
    float* x = alloc_f(n);
    synth_glide(x, n, rate, 71);
    for (size_t i = 0; i < n; i++) {
        x[i] *= scale;
    }
    const uint32_t before = agc->learned.restarts;
    trace_t t;
    trace_init(&t, n / (size_t)agc->frame_len);
    run_traced(agc, x, n, NULL, k_learn, &t);
    const long k = first_commit(&t, 0);
    if (restarts_first) {
        *restarts_first = t.f[0].restarts - before;
    }
    trace_free(&t);
    free(x);
    return k;
}

/* The @p n samples of hiss @p x on @p agc, from a frame boundary; snapshots at 60 s into @p at60 when asked. Returns
   the frames committed after the first 2 s. */
static int
run_hiss_on(dsd_speech_agc* agc, int rate, float* x, size_t n, dsd_speech_agc** at60) {
    trace_t t;
    trace_init(&t, n / (size_t)agc->frame_len);
    const size_t half = secs(rate, 60.0) < n ? secs(rate, 60.0) : n;
    run_traced(agc, x, half, NULL, k_learn, &t);
    if (at60) {
        *at60 = clone_agc(agc);
    }
    run_traced(agc, x + half, n - half, NULL, k_learn, &t);
    const int c = count_commits(&t, 100, (long)t.count);
    trace_free(&t);
    return c;
}

/* Hiss of @p kind at @p db for @p total seconds on @p agc (run_hiss_on()). */
static int
run_hiss(dsd_speech_agc* agc, int rate, dsd_test_noise_kind kind, double db, double total, dsd_speech_agc** at60) {
    const size_t n = secs(rate, total);
    float* x = alloc_f(n);
    assert(dsd_test_synth_noise(x, n, rate, 72, kind, db) == 0);
    const int c = run_hiss_on(agc, rate, x, n, at60);
    free(x);
    return c;
}

/* After 60-120 s of open-squelch hiss, continuous speech quieter than the hiss commits within 0.7 s, though it never
   drops 10 dB under the floor: the floor goes stale and starts over. Speech 15 dB under the hiss drops the floor at
   once (a frame 10 dB under it restarts it), so it commits within the run rule's own delay, where a floor that only
   followed the level down would wait for the stale-floor rule. The reverse too (speech, long hiss, speech). */
static void
test_floor_recovery_after_long_hiss(void) {
    const int rate = 48000;
    const float g0 = 8.537f;

    static const struct {
        dsd_test_noise_kind kind;
        double db;
        double total;
        const char* name;
    } cases[] = {
        {DSD_TEST_NOISE_VOICE_BAND, -30.0, 120.0, "white"},
        {DSD_TEST_NOISE_VOICE_BAND, -40.0, 120.0, "white"},
        {DSD_TEST_NOISE_DEEMPH, -30.0, 60.0, "de-emphasised"},
        {DSD_TEST_NOISE_DEEMPH, -40.0, 60.0, "de-emphasised"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_speech_agc* agc = new_agc(rate, g0);
        dsd_speech_agc* at60 = NULL;
        const int c = run_hiss(agc, rate, cases[i].kind, cases[i].db, cases[i].total, &at60);
        assert(c == 0);
        dsd_speech_agc* drop = clone_agc(at60);
        const long k60 = glide_first_commit(at60, rate, 1.0f, NULL);
        const long kend = glide_first_commit(agc, rate, 1.0f, NULL);
        /* -15 dB re the hiss: the glide sits at -35.5 dBFS against -30, so 9.5 dB more. */
        uint32_t restarted = 0;
        const float scale = (float)pow(10.0, (cases[i].db - 15.0 + 35.5) / 20.0);
        const long kdrop = glide_first_commit(drop, rate, scale, &restarted);
        SAY("%s hiss %+.0f dBFS: no commits on the hiss; quieter speech after 60 s commits at %.2f s, after %.0f s at "
            "%.2f s; speech 15 dB under it at %.2f s (floor restarts on its first frame: %u)\n",
            cases[i].name, cases[i].db, 0.02 * (double)(k60 + 1), cases[i].total, 0.02 * (double)(kend + 1),
            0.02 * (double)(kdrop + 1), restarted);
        assert(k60 >= 0 && k60 < 35);
        assert(kend >= 0 && kend < 35);
        assert(restarted == 1U && kdrop >= DSD_SPEECH_AGC_RUN_FRAMES - 1 && kdrop <= DSD_SPEECH_AGC_RUN_FRAMES + 2);
        free(drop);
        free(at60);
        free(agc);
    }
    /* Speech, then 60 s of hiss, then quieter speech. */
    static const dsd_test_noise_kind kinds[] = {DSD_TEST_NOISE_VOICE_BAND, DSD_TEST_NOISE_DEEMPH};
    for (size_t i = 0; i < 2; i++) {
        dsd_speech_agc* agc = new_agc(rate, g0);
        const size_t n = secs(rate, 12.0);
        float* x = alloc_f(n);
        speech_into(x, n, rate, 73, -29.0, 130.0, 0.0);
        run_traced(agc, x, n, NULL, k_learn, NULL);
        free(x);
        const int c = run_hiss(agc, rate, kinds[i], -30.0, 60.0, NULL);
        const long k = glide_first_commit(agc, rate, 1.0f, NULL);
        SAY("speech, then %s hiss 60 s: %d commits on the hiss, quieter speech after it commits at %.2f s\n",
            i == 0 ? "white" : "de-emphasised", c, 0.02 * (double)(k + 1));
        assert(c == 0);
        assert(k >= 0 && k < 35);
        free(agc);
    }
}

/* The same recovery on an analog instance (the coloured-noise veto on) after 60 s of hiss through 750 us de-emphasis
   and the monitor's band-pass, as hot as off-air receiver noise (-13 dBFS) and quieter (-30 dBFS): nothing commits on
   the hiss, which plays at -30 dBFS or less at its end; quieter speech after it commits within 0.7 s. Speech 15 dB
   under the -30 dBFS hiss restarts the floor at once (the floor, rising 1 dB/s, is still under -13 dBFS hiss after
   60 s). The reverse too (speech, the hiss, speech). */
static void
test_veto_floor_recovery_after_long_hiss(void) {
    const int rate = 48000;
    const float g0 = 8.537f;
    const size_t n = secs(rate, 60.0);
    const size_t tail = secs(rate, 5.0);
    float* x = alloc_f(n);
    static const double levels[] = {-13.0, -30.0};
    for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
        dsd_speech_agc* agc = new_analog_agc(rate, g0);
        synth_receiver_hiss(x, n, rate, 74, 750e-6, levels[i]);
        const int c = run_hiss_on(agc, rate, x, n, NULL);
        const double out = rms_dbfs(x + n - tail, tail);
        dsd_speech_agc* drop = clone_agc(agc);
        const long k = glide_first_commit(agc, rate, 1.0f, NULL);
        uint32_t restarted = 0;
        const float scale = (float)pow(10.0, (levels[i] - 15.0 + 35.5) / 20.0);
        const long kdrop = glide_first_commit(drop, rate, scale, &restarted);
        SAY("750 us hiss %+.0f dBFS, veto: %d commits on it, its last 5 s at %.2f dBFS; "
            "quieter speech after it commits at %.2f s; "
            "speech 15 dB under it at %.2f s (floor restarts on its first frame: %u)\n",
            levels[i], c, out, 0.02 * (double)(k + 1), 0.02 * (double)(kdrop + 1), restarted);
        assert(c == 0 && out <= -30.0);
        assert(k >= 0 && k < 35);
        if (levels[i] <= -30.0) {
            assert(restarted == 1U && kdrop >= DSD_SPEECH_AGC_RUN_FRAMES - 1 && kdrop <= DSD_SPEECH_AGC_RUN_FRAMES + 2);
        }
        free(drop);
        free(agc);
    }
    dsd_speech_agc* agc = new_analog_agc(rate, g0);
    const size_t ns = secs(rate, 12.0);
    float* sp = alloc_f(ns);
    speech_into(sp, ns, rate, 73, -29.0, 130.0, 0.0);
    run_traced(agc, sp, ns, NULL, k_learn, NULL);
    free(sp);
    synth_receiver_hiss(x, n, rate, 75, 750e-6, -30.0);
    const int c = run_hiss_on(agc, rate, x, n, NULL);
    const long k = glide_first_commit(agc, rate, 1.0f, NULL);
    SAY("speech, then 750 us hiss 60 s, veto: %d commits on the hiss, quieter speech after it commits at %.2f s\n", c,
        0.02 * (double)(k + 1));
    assert(c == 0);
    assert(k >= 0 && k < 35);
    free(agc);
    free(x);
}

/* Per-sample run: @p gains gets each sample's applied gain (controller, attenuator and limiter). */
static void
run_gains(dsd_speech_agc* agc, float* buf, size_t n, flags_t fl, float* gains) {
    for (size_t i = 0; i < n; i++) {
        dsd_speech_agc_process(agc, buf + i, 1, fl.learn, fl.decodable, fl.tone);
        gains[i] = agc->g_last * agc->g_lim;
    }
}

/* G.169-shaped noise after speech (continuous, and 1 s on/off) teaches nothing; speech in noise at SNR 20/15/10
   lands within 2 dB of the clean case; the gain holds across pauses. */
static void
test_noise_after_speech_and_snr(void) {
    const int rate = 8000;

    static const struct {
        dsd_test_noise_kind kind;
        double db;
    } noises[] = {
        {DSD_TEST_NOISE_VOICE_BAND, -45.0},
        {DSD_TEST_NOISE_WHITE, -50.0},
        {DSD_TEST_NOISE_WHITE, -40.0},
        {DSD_TEST_NOISE_VOICE_BAND, -30.0},
    };

    for (size_t i = 0; i < sizeof noises / sizeof noises[0]; i++) {
        dsd_speech_agc* agc = new_agc(rate, 0.0f);
        const size_t ns = secs(rate, 6.0);
        const size_t nn = secs(rate, 20.0);
        float* x = alloc_f(ns + nn);
        speech_into(x, ns, rate, 80 + i, -30.0, 125.0, 0.0);
        assert(dsd_test_synth_noise(x + ns, secs(rate, 10.0), rate, 81 + i, noises[i].kind, noises[i].db) == 0);
        for (int b = 0; b < 5; b++) { /* 1 s on, 1 s off */
            const size_t at = ns + secs(rate, 10.0 + 2.0 * b);
            assert(dsd_test_synth_noise(x + at, secs(rate, 1.0), rate, 90 + (uint64_t)b, noises[i].kind, noises[i].db)
                   == 0);
        }
        trace_t t;
        trace_init(&t, (ns + nn) / (size_t)agc->frame_len);
        run_traced(agc, x, ns + nn, NULL, k_learn, &t);
        const long from = (long)(ns / (size_t)agc->frame_len);
        const int c = count_commits(&t, from + 1, (long)t.count);
        const float dg = t.f[t.count - 1].g - t.f[from].g;
        SAY("noise %d at %+.0f dBFS after speech: %d commits over 10 s continuous + 10 s on/off, G moved %+.3f dB\n",
            (int)noises[i].kind, noises[i].db, c, (double)dg);
        assert(c == 0 && fabsf(dg) <= 1.0f);
        trace_free(&t);
        free(x);
        free(agc);
    }
    /* Speech in continuous voice-band noise. */
    static const double snrs[] = {1000.0, 20.0, 15.0, 10.0};
    double clean_err = 0.0;
    for (size_t i = 0; i < 4; i++) {
        const size_t n = secs(rate, 20.0);
        float* clean = alloc_f(n);
        float* x = alloc_f(n);
        float* g = alloc_f(n);
        speech_into(clean, n, rate, 85, -30.0, 125.0, 0.0);
        DSD_MEMCPY(x, clean, n * sizeof(float));
        const double sp = p56(clean, n, rate);
        if (snrs[i] < 999.0) {
            add_noise(x, n, rate, 86, DSD_TEST_NOISE_VOICE_BAND, sp - snrs[i]);
        }
        dsd_speech_agc* agc = new_agc(rate, 0.0f);
        run_gains(agc, x, n, k_learn, g);
        /* Established: from 2 s of committed speech on, the clean speech times the applied gain. */
        dsd_speech_agc* probe = new_agc(rate, 0.0f);
        float* y = alloc_f(n);
        DSD_MEMCPY(y, x, n * sizeof(float));
        trace_t t;
        trace_init(&t, n / (size_t)probe->frame_len);
        run_traced(probe, y, n, NULL, k_learn, &t);
        const long k = frame_of_committed(&t, 0, 100);
        assert(k >= 0);
        const size_t from = (size_t)(k + 1) * (size_t)probe->frame_len;
        for (size_t j = 0; j < n; j++) {
            y[j] = clean[j] * g[j];
        }
        const double err = p56(y + from, n - from, rate) - (double)DSD_SPEECH_AGC_TARGET_DBFS;
        if (i == 0) {
            clean_err = err;
            /* Pause hold: across every pause of at least 0.5 s the gain moves by at most 0.5 dB. */
            double worst = 0.0;
            int pauses = 0;
            size_t j = 0;
            while (j < n) {
                if (clean[j] != 0.0f) {
                    j++;
                    continue;
                }
                size_t e = j;
                while (e < n && clean[e] == 0.0f) {
                    e++;
                }
                if (j > 0 && e < n && e - j >= secs(rate, 0.5)) {
                    const double d = fabs(20.0 * log10((double)g[e] / (double)g[j - 1]));
                    worst = d > worst ? d : worst;
                    pauses++;
                }
                j = e;
            }
            SAY("pause hold: the gain moves at most %.3f dB across %d pauses >= 0.5 s\n", worst, pauses);
            assert(pauses >= 3 && worst <= 0.5);
        }
        SAY("speech in noise, SNR %4.0f dB: established %+.2f dB\n", snrs[i] > 999.0 ? INFINITY : snrs[i], err);
        assert(fabs(err) <= 3.0 && fabs(err - clean_err) <= 2.0);
        trace_free(&t);
        free(probe);
        free(agc);
        free(y);
        free(g);
        free(x);
        free(clean);
    }
}

/* Learn a talker for @p s seconds from reset, frame-aligned. */
static void
learn_talker(dsd_speech_agc* agc, int rate, double s, uint64_t seed, double level, double f0) {
    const size_t n = secs(rate, s);
    float* x = alloc_f(n);
    speech_into(x, n, rate, seed, level, f0, 0.0);
    run_traced(agc, x, n, NULL, k_learn, NULL);
    free(x);
}

/* Strong hiss after a transmission eases down to -30 dBFS or less without touching S or G; when the carrier quiets
   the hiss, the attenuator lets go within one frame. */
static void
test_attenuator_hiss_and_quieting(void) {
    const int rate = 48000;
    const size_t F = (size_t)frame_len(rate);
    dsd_speech_agc* agc = new_agc(rate, 8.537f);
    learn_talker(agc, rate, 5.0, 100, -30.0, 125.0);
    const dsd_speech_agc_learned before = agc->learned;
    const size_t nh = secs(rate, 10.0);
    float* hiss = alloc_f(nh);
    assert(dsd_test_synth_noise(hiss, nh, rate, 101, DSD_TEST_NOISE_VOICE_BAND, -10.0 - (double)before.gain_db) == 0);
    trace_t th;
    trace_init(&th, nh / F);
    run_traced(agc, hiss, nh, NULL, k_learn, &th);
    const double tail = rms_dbfs(hiss + nh - secs(rate, 2.0), secs(rate, 2.0));
    SAY("hiss that would play at -10 dBFS: E %.1f dB, last 2 s at %.1f dBFS; G %+.3f -> %+.3f, S %.3f -> %.3f\n",
        (double)agc->learned.atten_db, tail, (double)before.gain_db, (double)agc->learned.gain_db,
        (double)before.level_db, (double)agc->learned.level_db);
    assert(tail <= -30.0);
    assert(count_commits(&th, 0, (long)th.count) == 0);
    assert(same_bits(&agc->learned.gain_db, &before.gain_db, sizeof(float)));
    assert(same_bits(&agc->learned.level_db, &before.level_db, sizeof(float)));
    const size_t nq = secs(rate, 1.0);
    float* quiet = alloc_f(nq);
    assert(dsd_test_synth_noise(quiet, nq, rate, 102, DSD_TEST_NOISE_VOICE_BAND, -60.0) == 0);
    trace_t tq;
    trace_init(&tq, nq / F);
    run_traced(agc, quiet, nq, NULL, k_learn, &tq);
    SAY("quieting: E after the first quiet frame %.2f dB; applied gain after the second %+.3f dB (G %+.3f)\n",
        (double)tq.f[0].e, (double)tq.f[1].applied_db, (double)agc->learned.gain_db);
    assert(tq.f[0].e == 0.0f);
    assert(fabsf(tq.f[1].applied_db - agc->learned.gain_db) < 0.01f);
    trace_free(&tq);
    trace_free(&th);
    free(quiet);
    free(hiss);
    free(agc);
}

/* A quiet talker's gain over moderate hiss eases E down; speech louder than the hiss releases it on its first frame
   that passes both cues, within that frame's ramp. */
static void
test_attenuator_release_on_speech(void) {
    const int rate = 48000;
    const size_t F = (size_t)frame_len(rate);
    dsd_speech_agc* agc = new_agc(rate, 8.537f);
    learn_talker(agc, rate, 6.0, 103, -38.0, 180.0);
    const float g = agc->learned.gain_db;
    const size_t n = secs(rate, 9.0);
    const size_t at = secs(rate, 6.0);
    float* x = alloc_f(n);
    speech_into(x + at, n - at, rate, 104, -30.0, 180.0, 0.0);
    add_noise(x, n, rate, 105, DSD_TEST_NOISE_VOICE_BAND, -44.0);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    const long from = (long)(at / F);
    long first = -1;
    for (long i = from; i < (long)t.count; i++) {
        if (t.f[i].speech) {
            first = i;
            break;
        }
    }
    SAY("speech over hiss: G %+.2f, E before the speech %.2f dB, first speech frame %ld in: E %.2f, applied after the "
        "next frame %+.3f dB (G %+.3f)\n",
        (double)g, (double)t.f[from - 1].e, first - from, (double)t.f[first].e, (double)t.f[first + 1].applied_db,
        (double)t.f[first + 1].g);
    assert(t.f[from - 1].e < -1.0f);
    assert(first >= from && t.f[first].e == 0.0f);
    assert(fabsf(t.f[first + 1].applied_db - (t.f[first + 1].g + t.f[first + 1].e)) < 0.01f);
    trace_free(&t);
    free(x);
    free(agc);
}

/* What one hiss run did: frames taken for speech, and the output's RMS over its last 5 s. */
typedef struct {
    int speech;
    double tail_dbfs;
} hiss_run;

/* A copy of @p x into @p y through a fresh 48 kHz FM narrowband instance (the veto on when @p veto), learning
   throughout as on an open squelch. */
static hiss_run
run_open_squelch(const float* x, float* y, size_t n, int veto) {
    const int rate = 48000;
    dsd_speech_agc* agc = veto ? new_analog_agc(rate, 8.537f) : new_agc(rate, 8.537f);
    DSD_MEMCPY(y, x, n * sizeof(float));
    trace_t t;
    trace_init(&t, n / (size_t)agc->frame_len);
    run_traced(agc, y, n, NULL, k_learn, &t);
    hiss_run r = {0, rms_dbfs(y + n - secs(rate, 5.0), secs(rate, 5.0))};
    for (size_t i = 0; i < t.count; i++) {
        r.speech += t.f[i].speech;
    }
    trace_free(&t);
    free(agc);
    return r;
}

/* The coloured-noise veto (analog instances). 10 s of open-squelch FM hiss, as hot as off-air receiver noise (-13
   dBFS) and quieter (-30 dBFS), white noise through the receiver's de-emphasis and the monitor's band-pass. After
   750 us de-emphasis the hiss is coloured enough that a few percent of its frames pass the pitch cue on their own,
   which keeps the attenuator from easing it down (-10 dBFS RMS); with the veto no frame is taken for speech and it
   eases to -30 dBFS or less. After 75 us, 50 us or no de-emphasis the veto finds nothing to turn away: the output is
   the same bits as without it. The veto is off after init, and a reset keeps it. */
static void
test_noise_veto_hiss(void) {
    const int rate = 48000;
    const size_t n = secs(rate, 10.0);
    static const double taus[] = {750e-6, 75e-6, 50e-6, 0.0};
    static const double levels[] = {-13.0, -30.0};
    float* x = alloc_f(n);
    float* off = alloc_f(n);
    float* on = alloc_f(n);
    for (size_t ti = 0; ti < sizeof taus / sizeof taus[0]; ti++) {
        for (size_t li = 0; li < sizeof levels / sizeof levels[0]; li++) {
            synth_receiver_hiss(x, n, rate, 170 + ti, taus[ti], levels[li]);
            const hiss_run a = run_open_squelch(x, off, n, 0);
            const hiss_run b = run_open_squelch(x, on, n, 1);
            SAY("hiss, de-emphasis %3.0f us, %+.0f dBFS: without the veto %3d speech frames, last 5 s at %6.2f dBFS; "
                "with it %d, %6.2f dBFS\n",
                taus[ti] * 1e6, levels[li], a.speech, a.tail_dbfs, b.speech, b.tail_dbfs);
            assert(b.speech == 0 && b.tail_dbfs <= -30.0);
            if (ti == 0) {
                assert(a.speech > 0);
                if (li == 0) {
                    assert(a.tail_dbfs > -20.0);
                }
            } else {
                assert(same_bits(on, off, n * sizeof(float)));
            }
        }
    }
    dsd_speech_agc* agc = new_agc(rate, 8.537f);
    assert(agc->noise_veto == 0);
    dsd_speech_agc_set_noise_veto(agc, 7);
    dsd_speech_agc_reset(agc);
    assert(agc->noise_veto == 1);
    dsd_speech_agc_set_noise_veto(agc, 0);
    assert(agc->noise_veto == 0);
    dsd_speech_agc_set_noise_veto(NULL, 1);
    free(agc);
    free(on);
    free(off);
    free(x);
}

/* Tone frames (auto mode): at most the start gain from the first tone sample, no learning, S and G untouched, and the
   speech after plays as if the tone had never been there. Fixed mode leaves tones alone. */
static void
test_tone_cap(void) {
    const int rate = 8000;
    const size_t nt = secs(rate, 2.0);
    const size_t n2 = secs(rate, 3.0);
    float* tone = alloc_f(nt);
    float* tone_out = alloc_f(nt);
    float* s2 = alloc_f(n2);
    float* s2_ctrl = alloc_f(n2);
    assert(dsd_test_synth_dtmf(tone, nt, rate, '5', -10.0) == 0);
    speech_into(s2, n2, rate, 111, -40.0, 150.0, 0.0);
    DSD_MEMCPY(s2_ctrl, s2, n2 * sizeof(float));
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    dsd_speech_agc* ctrl = new_agc(rate, 0.0f);
    learn_talker(agc, rate, 8.0, 110, -40.0, 150.0);
    learn_talker(ctrl, rate, 8.0, 110, -40.0, 150.0);
    const dsd_speech_agc_learned before = agc->learned;
    assert(before.gain_db > 10.0f);
    DSD_MEMCPY(tone_out, tone, nt * sizeof(float));
    const flags_t tone_flags = {1, 1, 1};
    run_traced(agc, tone_out, nt, NULL, tone_flags, NULL);
    float worst = 0.0f;
    for (size_t i = 0; i < nt; i++) {
        if (fabsf(tone[i]) > 1.0f) {
            const float r = fabsf(tone_out[i] / tone[i]);
            worst = r > worst ? r : worst;
        }
    }
    assert(worst <= 1.0f + 1e-6f); /* at most 0 dB, the start gain, from the first sample */
    assert(fabsf(tone_out[1] / tone[1] - 1.0f) < 1e-6f);
    assert(same_bits(&agc->learned.gain_db, &before.gain_db, sizeof(float)));
    assert(same_bits(&agc->learned.level_db, &before.level_db, sizeof(float)));
    assert(agc->learned.commits == before.commits);
    float* zeros = alloc_f(nt);
    const flags_t closed = {0, 1, 0};
    run_traced(ctrl, zeros, nt, NULL, closed, NULL);
    run_traced(agc, s2, n2, NULL, k_learn, NULL);
    run_traced(ctrl, s2_ctrl, n2, NULL, k_learn, NULL);
    const size_t m = secs(rate, 0.32);
    const double d320 = p56(s2, m, rate) - p56(s2_ctrl, m, rate);
    const double dall = p56(s2, n2, rate) - p56(s2_ctrl, n2, rate);
    SAY("tone cap: tone at most %+.4f dB, G %+.2f / S %.2f unchanged; speech after vs control %+.3f dB (320 ms), "
        "%+.3f dB (3 s)\n",
        20.0 * log10((double)worst), (double)before.gain_db, (double)before.level_db, d320, dall);
    assert(fabs(d320) <= 0.1 && fabs(dall) <= 0.1);
    /* Fixed mode: the tone gets the fixed gain (+1.9 dB keeps its peaks under the ceiling). */
    dsd_speech_agc_set_fixed(agc, 1.25f);
    DSD_MEMCPY(tone_out, tone, nt * sizeof(float));
    run_traced(agc, tone_out, secs(rate, 0.1), NULL, tone_flags, NULL);
    for (size_t i = 0; i < secs(rate, 0.1); i++) {
        assert(fabsf(tone_out[i] - 1.25f * tone[i]) <= 1e-6f * fabsf(tone[i]));
    }
    free(zeros);
    free(ctrl);
    free(agc);
    free(s2_ctrl);
    free(s2);
    free(tone_out);
    free(tone);
}

/* Undecodable frames never learn but the gain and the limiter still apply; when decodability returns, the limiter and
   the pitch history start over, so clear speech after 3 s of full-scale garbage plays as if the garbage had been
   silence. */
static void
test_decodability_return(void) {
    const int rate = 8000;
    const size_t ng = secs(rate, 3.0);
    const size_t n2 = secs(rate, 2.0);
    float* garbage = alloc_f(ng);
    float* zeros = alloc_f(ng);
    float* s2 = alloc_f(n2);
    float* s2_ctrl = alloc_f(n2);
    assert(dsd_test_synth_noise(garbage, ng, rate, 121, DSD_TEST_NOISE_WHITE, -3.0) == 0);
    speech_into(s2, n2, rate, 122, -40.0, 150.0, 0.0);
    DSD_MEMCPY(s2_ctrl, s2, n2 * sizeof(float));
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    dsd_speech_agc* ctrl = new_agc(rate, 0.0f);
    learn_talker(agc, rate, 8.0, 120, -40.0, 150.0);
    learn_talker(ctrl, rate, 8.0, 120, -40.0, 150.0);
    const int commits = agc->learned.commits;
    const flags_t undecodable = {1, 0, 0};
    run_traced(agc, garbage, ng, NULL, undecodable, NULL);
    const float lim_db = 20.0f * log10f(agc->g_lim);
    assert(max_abs(garbage, ng) <= CEILING_INT16);
    assert(agc->learned.commits == commits);
    assert(lim_db < -10.0f);
    const flags_t closed = {0, 1, 0};
    run_traced(ctrl, zeros, ng, NULL, closed, NULL);
    run_traced(agc, s2, n2, NULL, k_learn, NULL);
    run_traced(ctrl, s2_ctrl, n2, NULL, k_learn, NULL);
    const size_t m = secs(rate, 0.15); /* the first 100 ms of speech (it starts at 50 ms) */
    const double d100 = p56(s2, m, rate) - p56(s2_ctrl, m, rate);
    SAY("decodability return: limiter at %.1f dB during the garbage; speech after vs control %+.3f dB (100 ms), "
        "outputs %s\n",
        (double)lim_db, d100, memcmp(s2, s2_ctrl, n2 * sizeof(float)) == 0 ? "identical" : "differ");
    assert(fabs(d100) <= 0.5);
    assert(memcmp(s2, s2_ctrl, n2 * sizeof(float)) == 0);
    free(ctrl);
    free(agc);
    free(s2_ctrl);
    free(s2);
    free(zeros);
    free(garbage);
}

/* Feed @p x through process_runs with @p learn in chunks of 1..1500 samples (seeded). */
static void
runs_random(dsd_speech_agc* agc, float* x, const uint8_t* learn, size_t n, uint64_t seed) {
    rng_t r = {seed};
    size_t p = 0;
    while (p < n) {
        size_t m = 1U + (size_t)(rng_next(&r) % 1500U);
        m = p + m > n ? n - p : m;
        dsd_speech_agc_process_runs(agc, x + p, m, learn + p);
        p += m;
    }
}

/* Like run_traced() with per-call flags, but @p fl_after from sample @p at on (split inside its frame). */
static void
run_traced_switch(dsd_speech_agc* agc, float* buf, size_t n, size_t at, flags_t fl_before, flags_t fl_after,
                  trace_t* t) {
    const size_t F = (size_t)agc->frame_len;
    for (size_t p = 0; p < n; p += F) {
        const size_t m = p + F <= n ? F : n - p;
        const dsd_speech_agc_learned before = agc->learned;
        const size_t cut = at <= p ? 0 : (at >= p + m ? m : at - p);
        if (cut > 0) {
            dsd_speech_agc_process(agc, buf + p, cut, fl_before.learn, fl_before.decodable, fl_before.tone);
        }
        if (cut < m) {
            dsd_speech_agc_process(agc, buf + p + cut, m - cut, fl_after.learn, fl_after.decodable, fl_after.tone);
        }
        if (m == F && t != NULL && t->count < t->cap) {
            frame_rec* r = &t->f[t->count++];
            r->committed = agc->learned.commits - before.commits;
        }
    }
}

/* A rollback after a contaminated tail is complete: a reception whose oldest checkpoint holds an unfinished run of
   1-5 frames, then 12 frames of loud non-speech, then a close, continues bit for bit like a control instance that
   never heard the tail and never closed. The control hears the same history, then closed silence where the tail and
   the gap were, through process() alone: 12 whole closed frames clear its run, pitch history, partial frame and ramp
   by themselves, so it runs none of the gate code under test. Just after the close the learned state is also exactly
   the oldest checkpoint's, with the since-commit clock advanced by the frames since and the run dropped, even when
   every live field was overwritten first. The next reception commits on a fresh run of six. With the acquisition stage
   on (@p acq) the same holds for its state, which the checkpoints carry with the rest. */
static void
test_rollback_matches_control(int acq) {
    const int rate = 48000;
    const float g0 = 8.537f;
    const size_t F = (size_t)frame_len(rate);
    static const size_t gaps[] = {1, 100, 480, 2880, 48000}; /* the last two: 3 frames and 1 s */
    for (int k = 1; k <= 5; k++) {
        for (size_t gi = 0; gi < sizeof gaps / sizeof gaps[0]; gi++) {
            const size_t gap = gaps[gi];
            const size_t n_pre = 50 * F;
            const size_t n_v = (size_t)k * F;
            const size_t n_tail = 12 * F;
            const size_t n_s2 = 30 * F;
            const size_t c0 = n_pre + n_v + n_tail;
            const size_t s2 = c0 + gap;
            const size_t n = s2 + n_s2;
            float* x = alloc_f(n);
            uint8_t* learn = alloc_u8(n);
            /* 20 frames of voice that commit (so the since-commit clock runs), then noise, then the pending run. */
            voiced_params vp;
            voiced_defaults(&vp, 120.0, 0.015, 141);
            synth_voiced(x, 20 * F, rate, &vp, -30.0);
            assert(dsd_test_synth_noise(x + 20 * F, n_pre - 20 * F, rate, 140, DSD_TEST_NOISE_VOICE_BAND, -45.0) == 0);
            float* v = alloc_f(n_v + F);
            synth_voiced(v, n_v + F, rate, &vp, -30.0);
            DSD_MEMCPY(x + n_pre, v + F, n_v * sizeof(float)); /* past the onset frame: every frame is speech */
            free(v);
            assert(dsd_test_synth_noise(x + n_pre + n_v, n_tail, rate, 142, DSD_TEST_NOISE_VOICE_BAND, -12.0) == 0);
            synth_voiced(x + s2, 20 * F, rate, &vp, -30.0);
            assert(dsd_test_synth_noise(x + s2 + 20 * F, 10 * F, rate, 143, DSD_TEST_NOISE_VOICE_BAND, -45.0) == 0);
            DSD_MEMSET(learn, 1, c0);
            DSD_MEMSET(learn + s2, 1, n_s2);

            /* A: the whole stream, the close and the open found by process_runs, random partitions. */
            dsd_speech_agc* a = acq ? new_acq_agc(rate, g0) : new_agc(rate, g0);
            float* ya = alloc_f(n);
            DSD_MEMCPY(ya, x, n * sizeof(float));
            runs_random(a, ya, learn, c0, 144 + (uint64_t)k);
            const int oldest = (a->cp_head - a->cp_count + DSD_SPEECH_AGC_CHECKPOINTS) % DSD_SPEECH_AGC_CHECKPOINTS;
            const dsd_speech_agc_checkpoint snap = a->checkpoints[oldest];
            assert(snap.learned.run_len == k);
            assert(snap.frames == (uint64_t)((n_pre + n_v) / F));
            assert(snap.learned.commits > 0 && snap.learned.since_commit < 100);
            assert(!acq || (snap.learned.loudest_known && snap.learned.peaks == DSD_SPEECH_AGC_PEAKS));
            dsd_speech_agc_learned expected = snap.learned;
            expected.since_commit += (int)(a->frames - snap.frames);
            expected.since_commit = expected.since_commit > (1 << 20) ? (1 << 20) : expected.since_commit;
            expected.run_len = 0;
            assert(!same_bits(&a->learned, &expected, sizeof expected)); /* the tail taught A something */
            /* As if the tail had moved every learned field: whatever the restore leaves alone keeps this pattern. */
            DSD_MEMSET(&a->learned, 0x5A, sizeof a->learned);
            /* The close, then one closed sample. */
            dsd_speech_agc_process_runs(a, ya + c0, 1, learn + c0);
            assert(same_bits(&a->learned, &expected, sizeof expected));

            /* C: the uncontaminated timeline, through process() alone. */
            dsd_speech_agc* c = acq ? new_acq_agc(rate, g0) : new_agc(rate, g0);
            float* yc = alloc_f(n);
            DSD_MEMCPY(yc, x, (n_pre + n_v) * sizeof(float));
            DSD_MEMCPY(yc + s2, x + s2, n_s2 * sizeof(float));
            run_traced(c, yc, n_pre + n_v, NULL, k_learn, NULL);
            const flags_t closed = {0, 1, 0};
            run_traced(c, yc + n_pre + n_v, n_tail, NULL, closed, NULL);
            assert(same_bits(&c->learned, &expected, sizeof expected));

            runs_random(a, ya + c0 + 1, learn + c0 + 1, n - c0 - 1, 145 + (uint64_t)k);
            trace_t t;
            trace_init(&t, (n - c0) / F);
            run_traced_switch(c, yc + c0, n - c0, gap, closed, k_learn, &t);
            assert(same_bits(ya + c0, yc + c0, (n - c0) * sizeof(float)));
            assert(same_bits(&a->learned, &c->learned, sizeof a->learned));
            const long first = first_commit(&t, 0);
            assert(first >= 0 && t.f[first].committed == DSD_SPEECH_AGC_RUN_FRAMES);
            const size_t end = (size_t)(first + 1) * F;
            SAY("rollback%s, pending run %d, gap %5zu: first commit %.2f frames into the next reception, = control\n",
                acq ? " (acquisition)" : "", k, gap, (double)(end - gap) / (double)F);
            assert(end >= gap + 6 * F - F / 10);
            trace_free(&t);
            free(yc);
            free(c);
            free(ya);
            free(a);
            free(learn);
            free(x);
        }
    }
}

/* The since-commit clock keeps running across a rollback: after a close it counts every frame since the last commit,
   so the attenuator starts exactly 1 s (50 frames) after the last committed frame, as it would without the close. */
static void
test_rollback_keeps_the_commit_clock(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    const size_t ns = secs(rate, 4.0);
    const size_t nh = secs(rate, 3.0);
    const size_t at_close = ns + 30 * F; /* 30 frames into the hiss */
    const size_t n = ns + nh;
    float* x = alloc_f(n);
    uint8_t* learn = alloc_u8(n);
    speech_into(x, ns, rate, 130, -30.0, 140.0, 0.0);
    assert(dsd_test_synth_noise(x + ns, nh, rate, 131, DSD_TEST_NOISE_VOICE_BAND, -20.0) == 0);
    DSD_MEMSET(learn, 1, n);
    DSD_MEMSET(learn + at_close, 0, F); /* closed for one frame, then open again */
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, at_close, learn, k_learn, &t);
    long last = -1;
    for (long i = 0; i < (long)t.count; i++) {
        last = t.f[i].committed > 0 ? i : last;
    }
    assert(last >= 0 && last < (long)(ns / F));
    run_traced(agc, x + at_close, F, learn + at_close, k_learn, &t);
    /* The close came first in that frame; the clock counts every finished frame since the last commit. */
    assert(agc->learned.since_commit == (int)t.count - 1 - (int)last);
    run_traced(agc, x + at_close + F, n - at_close - F, learn + at_close + F, k_learn, &t);
    long onset = -1;
    for (long i = last + 1; i < (long)t.count && onset < 0; i++) {
        onset = t.f[i].e < 0.0f ? i : -1;
    }
    SAY("rollback clock: last commit at frame %ld, close at frame %zu, attenuator starts at frame %ld\n", last,
        at_close / F, onset);
    assert(onset == last + 50);
    trace_free(&t);
    free(agc);
    free(learn);
    free(x);
}

/* Fixed to auto during a reception: the switch is the new opening checkpoint, so a close 3 frames later keeps the
   gain the switch set (the fixed gain clamped into range) instead of the auto gain from before it. */
static void
test_fixed_to_auto_then_close(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    learn_talker(agc, rate, 4.0, 220, -20.0, 140.0); /* auto settles near 0 dB */
    const float g_auto = agc->learned.gain_db;
    dsd_speech_agc_set_fixed(agc, 4.0f); /* +12.04 dB */
    learn_talker(agc, rate, 1.0, 221, -20.0, 140.0);
    dsd_speech_agc_set_fixed(agc, 0.0f);
    const float g_switch = agc->learned.gain_db;
    assert(fabsf(g_switch - 12.0412f) < 1e-3f && fabsf(g_auto - g_switch) > 6.0f);
    const size_t n = 4 * F;
    float* x = alloc_f(n);
    uint8_t* learn = alloc_u8(n);
    speech_into(x, n, rate, 222, -20.0, 140.0, 0.0);
    DSD_MEMSET(learn, 1, 3 * F);
    dsd_speech_agc_process_runs(agc, x, n, learn); /* 3 frames, then the close */
    SAY("fixed -> auto: auto %+.2f dB, switch %+.2f dB, after a close 3 frames later %+.2f dB\n", (double)g_auto,
        (double)g_switch, (double)agc->learned.gain_db);
    assert(agc->learned.commits > 0);
    assert(same_bits(&agc->learned.gain_db, &g_switch, sizeof(float)) && agc->learned.atten_db == 0.0f);
    free(learn);
    free(x);
    free(agc);
}

/* A reset is a new reception: it starts at the start gain whatever fixed gain the reception before it played, even
   when automatic gain takes over after it; within one reception the switch starts from the fixed gain. */
static void
test_reset_wins_over_the_fixed_gain(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(F);
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    for (int round = 0; round < 2; round++) {
        dsd_speech_agc_set_fixed(agc, 50.0f);
        for (size_t i = 0; i < F; i++) {
            x[i] = 100.0f;
        }
        dsd_speech_agc_process(agc, x, F, 0, 1, 0);
        assert(fabsf(x[0] - 5000.0f) < 1e-3f);
        dsd_speech_agc_reset(agc);
        if (round == 1) {
            /* The fixed setting stays through the reset, then auto: still the start gain. */
            dsd_speech_agc_set_fixed(agc, 50.0f);
            dsd_speech_agc_reset(agc);
        }
        dsd_speech_agc_set_fixed(agc, 0.0f);
        for (size_t i = 0; i < F; i++) {
            x[i] = 100.0f;
        }
        dsd_speech_agc_process(agc, x, F, 0, 1, 0);
        SAY("reset then auto: first sample %.3f (round %d)\n", (double)x[0], round);
        assert(fabsf(x[0] - 100.0f) < 1e-3f && fabsf(agc->learned.gain_db) < 1e-6f);
    }
    dsd_speech_agc_set_fixed(agc, 2.0f);
    for (size_t i = 0; i < F; i++) {
        x[i] = 100.0f;
    }
    dsd_speech_agc_process(agc, x, F, 0, 1, 0);
    dsd_speech_agc_set_fixed(agc, 0.0f);
    assert(fabsf(agc->learned.gain_db - 6.0206f) < 1e-3f);
    free(agc);
    free(x);
}

/* The start gain is bounded (init refuses, the setter ignores, anything past +-60 dB), and at the bounds the output
   stays finite and under the ceiling for the largest input the AGC takes. */
static void
test_start_gain_range(void) {
    dsd_speech_agc* agc = new_agc(8000, 0.0f);
    dsd_speech_agc_set_start_gain_db(agc, DSD_SPEECH_AGC_START_GAIN_MAX_DB + 1.0f);
    dsd_speech_agc_set_start_gain_db(agc, NAN);
    dsd_speech_agc_reset(agc);
    assert(fabsf(agc->start_gain_db) < 1e-6f);
    static const float bounds[] = {DSD_SPEECH_AGC_START_GAIN_MAX_DB, -DSD_SPEECH_AGC_START_GAIN_MAX_DB};
    for (size_t b = 0; b < 2; b++) {
        dsd_speech_agc_set_start_gain_db(agc, bounds[b]);
        dsd_speech_agc_reset(agc);
        assert(fabsf(agc->start_gain_db - bounds[b]) < 1e-6f);
        const size_t n = secs(8000, 2.0);
        float* x = alloc_f(n);
        speech_into(x, n, 8000, 230, -8.0, 150.0, 0.0);
        for (size_t i = 0; i < n; i += 997) {
            x[i] = (i & 1U) ? 9.9e14f : -9.9e14f; /* just under the input guard */
        }
        static const int tones[] = {0, 1};
        for (size_t k = 0; k < 2; k++) {
            dsd_speech_agc_process(agc, x, n / 2, 1, 1, tones[k]);
            dsd_speech_agc_process(agc, x + n / 2, n - n / 2, 1, 1, tones[k]);
        }
        for (size_t i = 0; i < n; i++) {
            assert(isfinite(x[i]) && fabsf(x[i]) <= CEILING_INT16);
        }
        assert(isfinite(dsd_speech_agc_gain_db(agc)));
        free(x);
    }
    free(agc);
}

/* The stale-floor counters saturate: a steady tone at the floor's level probes on every frame, for as long as it
   lasts, and the counters stop at 2^30 instead of overflowing (the state is set just short of the cap). */
static void
test_stale_counters_saturate(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    const size_t n = 12 * F;
    float* x = alloc_f(n);
    assert(dsd_test_synth_tone(x, n, rate, 1000.0, -30.0) == 0);
    dsd_speech_agc_learned* d = &agc->learned;
    d->noise_floor_db = -31.0f; /* the tone 1 dB over the floor: no cue 1, a stale-floor probe */
    d->floor_frames = 200;
    d->stale_frames = (1 << 30) - 3;
    d->stale_moves = 1 << 30;
    d->stale_lag_min_ms = 3.0f; /* the tone's first peak is 3 ms: no span, so no restart */
    d->stale_lag_max_ms = 3.0f;
    d->stale_lag_last_ms = 2.9f; /* the first frame counts as a move */
    const uint32_t restarts = d->restarts;
    dsd_speech_agc_process(agc, x, n, 1, 1, 0);
    SAY("stale counters after 12 probing frames: %d frames, %d moves, %u restarts\n", d->stale_frames, d->stale_moves,
        d->restarts - restarts);
    assert(d->stale_frames == 1 << 30 && d->stale_moves == 1 << 30);
    assert(d->restarts == restarts && d->commits == 0);
    free(x);
    free(agc);
}

/* Per-sample flags for the partition test: learn, decodable, tone. */
typedef struct {
    float* x;
    uint8_t* learn;
    uint8_t* dec;
    uint8_t* tone;
    size_t n;
} stream_t;

static void
stream_make(stream_t* s, int rate) {
    s->n = secs(rate, 12.0);
    s->x = alloc_f(s->n);
    s->learn = alloc_u8(s->n);
    s->dec = alloc_u8(s->n);
    s->tone = alloc_u8(s->n);
    speech_into(s->x, secs(rate, 4.0), rate, 150, -33.0, 140.0, 0.0);
    assert(dsd_test_synth_dtmf(s->x + secs(rate, 4.0), secs(rate, 0.5), rate, '7', -20.0) == 0);
    speech_into(s->x + secs(rate, 4.5), secs(rate, 4.0), rate, 151, -28.0, 210.0, 0.0);
    assert(dsd_test_synth_tone(s->x + secs(rate, 8.5), secs(rate, 1.0), rate, 900.0, -30.0) == 0);
    speech_into(s->x + secs(rate, 9.5), secs(rate, 2.5), rate, 152, -31.0, 120.0, 0.0);
    add_noise(s->x, s->n, rate, 153, DSD_TEST_NOISE_VOICE_BAND, -55.0);
    DSD_MEMSET(s->learn, 1, s->n);
    DSD_MEMSET(s->dec, 1, s->n);
    DSD_MEMSET(s->learn + secs(rate, 2.0), 0, secs(rate, 0.3));  /* a close and an open */
    DSD_MEMSET(s->learn + secs(rate, 7.0), 0, secs(rate, 0.01)); /* a short one */
    DSD_MEMSET(s->dec + secs(rate, 6.0), 0, secs(rate, 0.5));
    DSD_MEMSET(s->tone + secs(rate, 4.0), 1, secs(rate, 0.5));
    s->x[secs(rate, 5.0)] = NAN;
    s->x[secs(rate, 5.0) + 7] = INFINITY;
}

static void
stream_free(stream_t* s) {
    free(s->x);
    free(s->learn);
    free(s->dec);
    free(s->tone);
}

/* Drive process() over @p s with calls of at most @p chunk samples (0: random 1..1500), split where a flag changes. */
static void
drive_flags(dsd_speech_agc* agc, const stream_t* s, float* y, size_t chunk) {
    rng_t r = {chunk + 7U};
    size_t p = 0;
    while (p < s->n) {
        size_t m = chunk > 0 ? chunk : 1U + (size_t)(rng_next(&r) % 1500U);
        m = p + m > s->n ? s->n - p : m;
        size_t q = p + 1U;
        while (q < p + m && s->learn[q] == s->learn[p] && s->dec[q] == s->dec[p] && s->tone[q] == s->tone[p]) {
            q++;
        }
        dsd_speech_agc_process(agc, y + p, q - p, s->learn[p], s->dec[p], s->tone[p]);
        p = q;
    }
}

static void
drive_runs(dsd_speech_agc* agc, const stream_t* s, float* y, size_t chunk) {
    rng_t r = {chunk + 9U};
    size_t p = 0;
    while (p < s->n) {
        size_t m = chunk > 0 ? chunk : 1U + (size_t)(rng_next(&r) % 1500U);
        m = p + m > s->n ? s->n - p : m;
        dsd_speech_agc_process_runs(agc, y + p, m, s->learn + p);
        p += m;
    }
}

/* Drive a fresh instance (start gain @p g0, the veto on when @p veto, the acquisition stage when @p acq) over @p s,
   with per-call flags or, when @p runs, per-sample learn runs; @p y gets the output. */
static dsd_speech_agc*
drive_fresh(const stream_t* s, int rate, float g0, int veto, int acq, int runs, float* y, size_t chunk) {
    dsd_speech_agc* agc = veto ? new_analog_agc(rate, g0) : new_agc(rate, g0);
    dsd_speech_agc_set_acquisition(agc, acq);
    DSD_MEMCPY(y, s->x, s->n * sizeof(float));
    if (runs) {
        drive_runs(agc, s, y, chunk);
    } else {
        drive_flags(agc, s, y, chunk);
    }
    return agc;
}

/* The output is bit-identical for any partition (one sample, random, a frame, a frame +-1), at 8, 22.05 and 48 kHz,
   for per-call flags (learn, decodable, tone) and for per-sample learn runs with their closes and opens, and so with
   the coloured-noise veto on the same stream under 750 us de-emphasised hiss, where it turns frames away. The same with
   the acquisition stage on (@p acq). */
static void
test_partition_identity(int acq) {
    static const int rates[] = {8000, 22050, 48000};
    for (size_t ri = 0; ri < 3; ri++) {
        const int rate = rates[ri];
        const size_t F = (size_t)frame_len(rate);
        stream_t s;
        stream_make(&s, rate);
        /* The veto's stream: the same with 750 us hiss under it, in its own buffer so every mode keeps its input. */
        float* plain = s.x;
        float* hissy = alloc_f(s.n);
        synth_receiver_hiss(hissy, s.n, rate, 154, 750e-6, -40.0);
        add_into(hissy, plain, s.n);
        const size_t chunks[] = {1, 0, F, F - 1, F + 1};
        for (int mode = 0; mode < 3; mode++) {
            const int veto = mode == 2;
            const int runs = mode != 0;
            const float g0 = runs ? 8.537f : 0.0f;
            s.x = veto ? hissy : plain;
            float* yref = alloc_f(s.n);
            dsd_speech_agc* ref = drive_fresh(&s, rate, g0, veto, acq, runs, yref, s.n);
            assert(ref->learned.commits > 100 && ref->learned.rejections >= 1);
            if (veto) {
                float* yoff = alloc_f(s.n);
                free(drive_fresh(&s, rate, g0, 0, acq, runs, yoff, s.n));
                assert(!same_bits(yoff, yref, s.n * sizeof(float)));
                free(yoff);
            }
            for (size_t ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
                float* y = alloc_f(s.n);
                dsd_speech_agc* agc = drive_fresh(&s, rate, g0, veto, acq, runs, y, chunks[ci]);
                assert(same_bits(y, yref, s.n * sizeof(float)));
                assert(same_bits(&agc->learned, &ref->learned, sizeof agc->learned));
                free(y);
                free(agc);
            }
            SAY("%5d Hz %s%s: identical for every partition (%d commits, %u rejections, %u restarts, %d bursts)\n",
                rate, veto ? "veto, runs " : (runs ? "learn runs " : "frame flags"), acq ? " (acquisition)" : "",
                ref->learned.commits, ref->learned.rejections, ref->learned.restarts, ref->learned.bursts);
            free(yref);
            free(ref);
        }
        s.x = plain;
        free(hissy);
        stream_free(&s);
    }
}

/* The cross-rate material: one analytic waveform sampled at each rate. Segment starts in seconds. */
typedef struct {
    double start;
    double dur;
    int kind; /* 0 speech, 1 natural vowel, 2 steady vowel, 3 tone */
    double arg;
    uint64_t seed;
} segment_t;

static const segment_t k_xrate[] = {
    {0.0, 10.0, 0, 0.0, 160},  {10.3, 1.0, 1, 0.0, 161}, {11.6, 1.0, 2, 0.0, 162},  {12.9, 5.0, 0, 0.0, 163},
    {17.9, 3.0, 3, 1000.0, 0}, {20.9, 5.0, 0, 0.0, 164}, {25.9, 3.0, 3, 1025.6, 0}, {28.9, 5.0, 0, 0.0, 165},
};
#define XRATE_TOTAL_S 33.9

static void
xrate_make(float* x, size_t n, int rate) {
    DSD_MEMSET(x, 0, n * sizeof(float));
    for (size_t i = 0; i < sizeof k_xrate / sizeof k_xrate[0]; i++) {
        const segment_t* g = &k_xrate[i];
        const size_t at = at_secs(rate, g->start);
        const size_t m = secs(rate, g->dur);
        assert(at + m <= n);
        if (g->kind == 0) {
            speech_into(x + at, m, rate, g->seed, -30.0, 130.0, 0.0);
        } else if (g->kind == 3) {
            assert(dsd_test_synth_tone(x + at, m, rate, g->arg, -30.0) == 0);
        } else {
            dsd_test_vowel_params v;
            dsd_test_vowel_defaults(&v, g->seed);
            v.duration_s = g->dur;
            v.steady = g->kind == 2;
            assert(dsd_test_synth_vowel(x + at, m, rate, &v) == 0);
        }
    }
}

/* One waveform at 8, 22.05 and 48 kHz: speech levels within 0.3 dB, the applied gain within 0.25 dB on 95% of frames
   and 2 dB on all (over 1 dB on at most 2%), the first commit and every steady-tone rejection in the same frame, and
   the same commit decision on 99% of frames. The same with the acquisition stage on (@p acq). */
static void
test_cross_rate(int acq) {
    static const int rates[] = {48000, 22050, 8000};
    trace_t t[3];
    double level[3][8];
    for (size_t ri = 0; ri < 3; ri++) {
        const int rate = rates[ri];
        const size_t n = secs(rate, XRATE_TOTAL_S);
        float* x = alloc_f(n);
        xrate_make(x, n, rate);
        run_fresh_mode(rate, 0.0f, 0, acq, x, n, &t[ri]);
        for (size_t i = 0; i < sizeof k_xrate / sizeof k_xrate[0]; i++) {
            level[ri][i] =
                k_xrate[i].kind == 0 ? p56(x + at_secs(rate, k_xrate[i].start), secs(rate, k_xrate[i].dur), rate) : 0.0;
        }
        free(x);
    }
    for (size_t ri = 1; ri < 3; ri++) {
        assert(t[ri].count == t[0].count);
        const size_t nf = t[0].count;
        float* d = alloc_f(nf);
        int over1 = 0;
        int agree = 0;
        int rejections = 0;
        float dmax = 0.0f;
        for (size_t i = 0; i < nf; i++) {
            d[i] = fabsf(t[ri].f[i].applied_db - t[0].f[i].applied_db);
            dmax = d[i] > dmax ? d[i] : dmax;
            over1 += d[i] > 1.0f;
            agree += (t[ri].f[i].committed > 0) == (t[0].f[i].committed > 0);
            assert(t[ri].f[i].rejected == t[0].f[i].rejected);
            rejections += t[ri].f[i].rejected;
        }
        /* The steady vowel and the two tones each end in a rejection, so the frame-for-frame check has something to
           compare. */
        assert(rejections >= 3);
        const float p95 = percentile(d, nf, 0.95);
        double worst_level = 0.0;
        for (size_t i = 0; i < sizeof k_xrate / sizeof k_xrate[0]; i++) {
            const double dl = fabs(level[ri][i] - level[0][i]);
            worst_level = dl > worst_level ? dl : worst_level;
        }
        SAY("%5d Hz vs 48 kHz%s: gain p95 %.3f max %.3f dB, %d of %zu frames over 1 dB; commits agree on %.2f%%; first "
            "commit frame %ld vs %ld; speech levels within %.3f dB; %d rejections in the same frames\n",
            rates[ri], acq ? " (acquisition)" : "", (double)p95, (double)dmax, over1, nf, 100.0 * agree / (double)nf,
            first_commit(&t[ri], 0), first_commit(&t[0], 0), worst_level, rejections);
        assert(p95 <= 0.25f && dmax <= 2.0f && over1 * 50 <= (int)nf);
        assert(agree * 100 >= 99 * (int)nf);
        assert(first_commit(&t[ri], 0) == first_commit(&t[0], 0));
        assert(worst_level <= 0.3);
        free(d);
    }
    for (size_t ri = 0; ri < 3; ri++) {
        trace_free(&t[ri]);
    }
}

/* No sample ever leaves the limiter above the ceiling, auto or fixed, however hot the input. */
static void
test_ceiling(void) {
    static const int rates[] = {8000, 48000};
    for (size_t ri = 0; ri < 2; ri++) {
        const int rate = rates[ri];
        const size_t n = secs(rate, 8.0);
        float* x = alloc_f(n);
        speech_into(x, n, rate, 170, -8.0, 160.0, 0.0); /* peaks well past full scale */
        assert(max_abs(x, n) > 40000.0f);
        static const float fixed[] = {0.0f, 1.0f, 30.0f, 1e9f};
        for (size_t fi = 0; fi < 4; fi++) {
            dsd_speech_agc* agc = new_agc(rate, 6.0f);
            dsd_speech_agc_set_fixed(agc, fixed[fi]);
            float* y = alloc_f(n);
            DSD_MEMCPY(y, x, n * sizeof(float));
            run_traced(agc, y, n, NULL, k_learn, NULL);
            const float m = max_abs(y, n);
            SAY("%5d Hz hot speech, fixed %g: peak %.1f\n", rate, (double)fixed[fi], (double)m);
            assert(m <= CEILING_INT16 && m > 20000.0f);
            free(y);
            free(agc);
        }
        free(x);
    }
    /* A full-scale square at 0 dBFS ceiling stays within int16; at -1 dBFS limited samples sit at the ceiling. */
    dsd_speech_agc agc;
    assert(dsd_speech_agc_init(&agc, 8000, 0.0f, -24.0f, 0.0f) == 0);
    assert(agc.ceiling == 32768.0f);
    float sq[320];
    for (size_t i = 0; i < 320; i++) {
        sq[i] = (i & 8U) ? 32767.0f : -32768.0f;
    }
    dsd_speech_agc_set_fixed(&agc, 4.0f);
    dsd_speech_agc_process(&agc, sq, 320, 1, 1, 0);
    for (size_t i = 0; i < 320; i++) {
        assert(sq[i] <= 32767.0f && sq[i] >= -32768.0f);
    }
}

/* A reset is a fresh start: after other audio, a reset instance plays a signal exactly as a new one does, with the
   acquisition stage on too (@p acq). */
static void
test_reset_is_fresh(int acq) {
    const int rate = 22050;
    const size_t n = secs(rate, 5.0);
    float* x = alloc_f(n);
    float* y1 = alloc_f(n);
    float* y2 = alloc_f(n);
    speech_into(x, n, rate, 180, -34.0, 150.0, 0.0);
    DSD_MEMCPY(y1, x, n * sizeof(float));
    DSD_MEMCPY(y2, x, n * sizeof(float));
    dsd_speech_agc* fresh = acq ? new_acq_agc(rate, 2.0f) : new_agc(rate, 2.0f);
    dsd_speech_agc* used = acq ? new_acq_agc(rate, 2.0f) : new_agc(rate, 2.0f);
    float* junk = alloc_f(n);
    speech_into(junk, n, rate, 181, -20.0, 220.0, 0.0);
    add_noise(junk, n, rate, 182, DSD_TEST_NOISE_VOICE_BAND, -25.0);
    dsd_speech_agc_process(used, junk, n / 3 + 17, 1, 1, 0); /* stop mid-frame */
    dsd_speech_agc_process(used, junk, 100, 1, 0, 1);
    dsd_speech_agc_reset(used);
    assert(used->learned.commits == 0 && used->cp_count == 1 && used->frame_pos == 0);
    assert(fabsf(used->learned.gain_db - 2.0f) < 1e-6f);
    dsd_speech_agc_process(fresh, y1, n, 1, 1, 0);
    dsd_speech_agc_process(used, y2, n, 1, 1, 0);
    assert(same_bits(y1, y2, n * sizeof(float)));
    assert(same_bits(&fresh->learned, &used->learned, sizeof fresh->learned));
    free(junk);
    free(used);
    free(fresh);
    free(y2);
    free(y1);
    free(x);
}

/* Closed audio (nothing learns) of one level for @p n samples, one sample per call; @p gains gets each sample's
   controller gain, linear. */
static void
run_closed_gains(dsd_speech_agc* agc, size_t n, float* gains) {
    for (size_t i = 0; i < n; i++) {
        float v = 100.0f;
        dsd_speech_agc_process(agc, &v, 1, 0, 1, 0);
        gains[i] = agc->g_last;
    }
}

/* dsd_speech_agc_open_at() before a commit: the gain follows it at once (ramp 0) or over one frame, linearly (ramp 1),
   clamped into the controller's range; a tone frame still plays at no more than the start gain; non-finite values are
   ignored. It is a new opening checkpoint. Once a frame has committed it is ignored; a reset returns to the start
   gain. */
static void
test_open_at(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t F = (size_t)frame_len(rate);
    float* g = alloc_f(F);
    dsd_speech_agc* agc = new_agc(rate, g0);
    dsd_speech_agc_open_at(NULL, 10.0f, 0);
    /* At once. */
    dsd_speech_agc_open_at(agc, 12.0f, 0);
    assert(fabsf(agc->learned.gain_db - 12.0f) < 1e-6f);
    assert(fabsf(dsd_speech_agc_gain_db(agc) - 12.0f) < 1e-4f);
    run_closed_gains(agc, F, g);
    assert(fabsf(20.0f * log10f(g[0]) - 12.0f) < 1e-4f && fabsf(20.0f * log10f(g[F - 1]) - 12.0f) < 1e-4f);
    /* Over one frame, linearly, from a frame boundary. */
    const float from = agc->g_last;
    dsd_speech_agc_open_at(agc, 20.0f, 1);
    assert(fabsf(agc->learned.gain_db - 20.0f) < 1e-6f);
    run_closed_gains(agc, F, g);
    const float step = (g[F - 1] - from) / (float)F;
    for (size_t i = 0; i < F; i++) {
        assert(fabsf(g[i] - (from + step * (float)(i + 1U))) <= 1e-4f * g[F - 1]);
    }
    assert(fabsf(20.0f * log10f(g[F - 1]) - 20.0f) < 1e-4f && fabsf(dsd_speech_agc_gain_db(agc) - 20.0f) < 1e-4f);
    /* Out of range, then not finite. */
    dsd_speech_agc_open_at(agc, 100.0f, 0);
    assert(fabsf(agc->learned.gain_db - (g0 + DSD_SPEECH_AGC_MAX_BOOST_DB)) < 1e-6f);
    dsd_speech_agc_open_at(agc, -100.0f, 0);
    assert(fabsf(agc->learned.gain_db - (g0 - DSD_SPEECH_AGC_MAX_CUT_DB)) < 1e-6f);
    static const float bad[] = {NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < 3; i++) {
        dsd_speech_agc_open_at(agc, bad[i], 0);
        assert(fabsf(agc->learned.gain_db - (g0 - DSD_SPEECH_AGC_MAX_CUT_DB)) < 1e-6f);
    }
    /* The tone cap stays at the start gain. */
    dsd_speech_agc_open_at(agc, g0 + 20.0f, 0);
    float* tone = alloc_f(2 * F);
    float* tone_out = alloc_f(2 * F);
    assert(dsd_test_synth_tone(tone, 2 * F, rate, 1000.0, -30.0) == 0);
    DSD_MEMCPY(tone_out, tone, 2 * F * sizeof(float));
    dsd_speech_agc_process(agc, tone_out, 2 * F, 1, 1, 1);
    for (size_t i = 0; i < 2 * F; i++) {
        assert(fabsf(tone_out[i]) <= fabsf(tone[i]) * powf(10.0f, g0 / 20.0f) * (1.0f + 1e-6f));
    }
    assert(fabsf(agc->abs_floor_db - (-20.0f - g0 - 60.0f)) < 1e-4f && fabsf(agc->start_gain_db - g0) < 1e-6f);
    /* A new opening checkpoint: a close 3 frames after it restores the opened gain, not the start gain. */
    dsd_speech_agc_reset(agc);
    float* x = alloc_f(6 * F);
    assert(dsd_test_synth_noise(x, 6 * F, rate, 240, DSD_TEST_NOISE_VOICE_BAND, -50.0) == 0);
    dsd_speech_agc_process(agc, x, 3 * F, 1, 1, 0);
    dsd_speech_agc_open_at(agc, 16.0f, 0);
    dsd_speech_agc_process(agc, x + 3 * F, 3 * F, 1, 1, 0);
    dsd_speech_agc_gate_closed(agc);
    assert(fabsf(agc->learned.gain_db - 16.0f) < 1e-6f && fabsf(dsd_speech_agc_gain_db(agc) - 16.0f) < 1e-4f);
    /* After a commit it is ignored; a reset returns to the start gain. */
    learn_talker(agc, rate, 2.0, 241, -34.0, 140.0);
    assert(agc->learned.commits > 0);
    const dsd_speech_agc_learned learned = agc->learned;
    const float g_cur = agc->g_cur;
    dsd_speech_agc_open_at(agc, g0 + 25.0f, 0);
    assert(same_bits(&agc->learned, &learned, sizeof learned) && same_bits(&agc->g_cur, &g_cur, sizeof g_cur));
    dsd_speech_agc_reset(agc);
    assert(fabsf(agc->learned.gain_db - g0) < 1e-6f && fabsf(dsd_speech_agc_gain_db(agc) - g0) < 1e-4f);
    SAY("open_at: at once, over one frame, clamped, tone cap, checkpoint, ignored after a commit\n");
    free(x);
    free(tone_out);
    free(tone);
    free(agc);
    free(g);
}

/* Whether the controller gains @p g of @p n samples are a ramp from @p from to @p to over @p n samples as the AGC steps
   one, bit for bit: (to - from) / n added each sample, the last sample at @p to exactly. */
static int
is_ramp(const float* g, size_t n, float from, float to) {
    const float step = (to - from) / (float)n;
    float cur = from;
    for (size_t i = 0; i < n; i++) {
        cur = i + 1U < n ? cur + step : to;
        if (!same_bits(&g[i], &cur, sizeof cur)) {
            return 0;
        }
    }
    return 1;
}

/* @p n samples of @p x, one per call, with learn flag @p learn; @p gains gets each sample's controller gain, linear. */
static void
run_ctrl_gains(dsd_speech_agc* agc, const float* x, size_t n, int learn, float* gains) {
    for (size_t i = 0; i < n; i++) {
        float v = x[i];
        dsd_speech_agc_process(agc, &v, 1, learn, 1, 0);
        gains[i] = agc->g_last;
    }
}

/* dsd_speech_agc_open_at(..., 1) partway through an analysis frame: the ramp runs its own frame's worth of samples
   across the frame boundary, at the step the call set, and ends exactly at the gain, linear in the gain (half way at
   half its length); the boundary does not restart it. Called at a frame boundary it ends with that frame, as before.
   At a boundary that changes the target (the loud-frame guard cutting a loud frame) the pending ramp gives way: a new
   one-frame ramp from where it stood reaches the new target one frame later. */
static void
test_open_at_mid_frame(void) {
    enum { F = 160, N = 2 * F }; /* one analysis frame at 8 kHz, and two */

    const int rate = 8000;
    const float g0 = 4.0f;
    const float open_db = 20.0f;
    const float from = powf(10.0f, g0 / 20.0f);
    const float to = powf(10.0f, open_db / 20.0f);
    float g[N];
    float closed[N];
    float loud[F];
    assert(frame_len(rate) == F);
    for (size_t i = 0; i < N; i++) {
        closed[i] = 100.0f;
    }
    /* Half a frame in, then two frames. */
    dsd_speech_agc* mid = new_agc(rate, g0);
    run_ctrl_gains(mid, closed, F / 2, 0, g);
    assert(same_bits(&g[F / 2 - 1], &from, sizeof from));
    dsd_speech_agc_open_at(mid, open_db, 1);
    run_ctrl_gains(mid, closed, N, 0, g);
    const float half = 0.5f * (from + to);
    SAY("open_at half a frame in: %+.2f dB at the frame boundary (%.4f, half way %.4f), %+.4f dB one frame after the "
        "call\n",
        (double)(20.0f * log10f(g[F / 2 - 1])), (double)g[F / 2 - 1], (double)half, (double)(20.0f * log10f(g[F - 1])));
    assert(is_ramp(g, F, from, to));
    assert(fabsf(g[F / 2 - 1] - half) <= 1e-5f * to && fabsf(20.0f * log10f(g[F / 2 - 1]) - 12.0f) > 3.0f);
    for (size_t i = F; i < N; i++) {
        assert(same_bits(&g[i], &to, sizeof to));
    }
    free(mid);
    /* At a frame boundary. */
    dsd_speech_agc* edge = new_agc(rate, g0);
    run_ctrl_gains(edge, closed, F, 0, g);
    dsd_speech_agc_open_at(edge, open_db, 1);
    run_ctrl_gains(edge, closed, N, 0, g);
    assert(is_ramp(g, F, from, to));
    for (size_t i = F; i < N; i++) {
        assert(same_bits(&g[i], &to, sizeof to));
    }
    free(edge);
    /* The guard changes the target at the boundary half way through the ramp. */
    assert(dsd_test_synth_noise(loud, F, rate, 350, DSD_TEST_NOISE_VOICE_BAND, -10.0) == 0);
    dsd_speech_agc* guarded = new_acq_agc(rate, g0);
    run_ctrl_gains(guarded, loud, F / 2, 1, g);
    dsd_speech_agc_open_at(guarded, open_db, 1);
    run_ctrl_gains(guarded, loud + F / 2, F / 2, 1, g);
    const float at = g[F / 2 - 1];
    const float cut = guarded->learned.gain_db;
    const float cut_to = powf(10.0f, (guarded->learned.gain_db + guarded->learned.atten_db) / 20.0f);
    run_ctrl_gains(guarded, closed, N, 0, g);
    SAY("open_at, then the guard at the boundary: cut to %+.2f dB from %+.2f dB, reached one frame later\n",
        (double)cut, (double)(20.0f * log10f(at)));
    assert(cut < g0 && at > from && at < to);
    assert(is_ramp(g, F, at, cut_to));
    for (size_t i = F; i < N; i++) {
        assert(same_bits(&g[i], &cut_to, sizeof cut_to));
    }
    free(guarded);
}

/* The frame of @p x (@p frames analysis frames, learning samples, one frame per call) at whose end the AGC first
   commits, or -1. */
static int
first_commit_frame(int rate, float g0, const float* x, int frames) {
    const size_t F = (size_t)frame_len(rate);
    dsd_speech_agc* agc = new_agc(rate, g0);
    float* y = alloc_f(F);
    int at = -1;
    for (int k = 0; k < frames && at < 0; k++) {
        DSD_MEMCPY(y, x + (size_t)k * F, F * sizeof(float));
        dsd_speech_agc_process(agc, y, F, 1, 1, 0);
        at = agc->learned.commits > 0 ? k : -1;
    }
    free(y);
    free(agc);
    return at;
}

/* dsd_speech_agc_open_at(..., 1) half a frame into learning samples. Every eligible frame passes the boundary with the
   gain marked as touched, whether or not the target moved: a frame that passes both cues (a 125 Hz sine) resets the
   attenuator, and unvoiced noise is eligible without being speech. A boundary whose target did not move keeps the
   pending ramp: it reaches the gain exactly one frame's worth of samples after the call, linearly. A boundary that
   moves it (the guard cutting a loud sine frame) starts a new one-frame ramp from where the pending one stood. With
   the acquisition stage off, the analog law's first step at the first commit moves the target: a ramp pending at
   that boundary restarts there too. */
static void
test_open_at_mid_frame_learning(void) {
    enum { F = 160, N = 2 * F, FRAMES = 50 };

    const int rate = 8000;
    const float g0 = 4.0f;
    const float open_db = 20.0f;
    const float from = powf(10.0f, g0 / 20.0f);
    const float to = powf(10.0f, open_db / 20.0f);
    float g[N];
    float sine[N];
    float noise[N];
    float loud[N];
    assert(frame_len(rate) == F);
    assert(dsd_test_synth_tone(sine, N, rate, 125.0, -40.0) == 0);
    assert(dsd_test_synth_noise(noise, N, rate, 352, DSD_TEST_NOISE_VOICE_BAND, -55.0) == 0);
    assert(dsd_test_synth_tone(loud, N, rate, 125.0, -10.0) == 0);
    const float* material[] = {sine, noise};
    const char* names[] = {"a 125 Hz sine", "unvoiced noise"};
    for (size_t m = 0; m < 2; m++) {
        dsd_speech_agc* agc = new_acq_agc(rate, g0);
        run_ctrl_gains(agc, material[m], F / 2, 1, g);
        dsd_speech_agc_open_at(agc, open_db, 1);
        run_ctrl_gains(agc, material[m] + F / 2, F, 1, g);
        SAY("open_at half a frame into %s: %+.4f dB one frame after the call, speech run %d\n", names[m],
            (double)(20.0f * log10f(g[F - 1])), agc->learned.run_len);
        assert(agc->learned.commits == 0 && agc->learned.run_len == (m == 0 ? 1 : 0));
        assert(same_bits(&agc->learned.gain_db, &open_db, sizeof open_db) && fabsf(agc->learned.atten_db) <= 0.0f);
        assert(is_ramp(g, F, from, to));
        assert(fabsf(g[F / 2 - 1] - 0.5f * (from + to)) <= 1e-5f * to);
        free(agc);
    }
    /* The guard cuts the loud sine frame at the boundary half way through the ramp. */
    dsd_speech_agc* guarded = new_acq_agc(rate, g0);
    run_ctrl_gains(guarded, loud, F / 2, 1, g);
    dsd_speech_agc_open_at(guarded, open_db, 1);
    run_ctrl_gains(guarded, loud + F / 2, F / 2, 1, g);
    const float at = g[F / 2 - 1];
    const float cut_to = powf(10.0f, (guarded->learned.gain_db + guarded->learned.atten_db) / 20.0f);
    run_ctrl_gains(guarded, loud + F, F, 1, g);
    SAY("open_at, then the guard on a loud sine: cut to %+.2f dB from %+.2f dB, reached one frame later\n",
        (double)guarded->learned.gain_db, (double)(20.0f * log10f(at)));
    assert(guarded->learned.gain_db < g0 && at > from && at < to);
    assert(is_ramp(g, F, at, cut_to));
    free(guarded);
    /* The stage off: the call half a frame before the first commit, whose step moves the target. */
    float* voiced = alloc_f((size_t)FRAMES * F);
    assert(dsd_test_synth_tone(voiced, (size_t)FRAMES * F, rate, 125.0, -30.0) == 0);
    const int k = first_commit_frame(rate, g0, voiced, FRAMES);
    assert(k > 0);
    dsd_speech_agc* off = new_agc(rate, g0);
    dsd_speech_agc_process(off, voiced, (size_t)k * F + F / 2, 1, 1, 0);
    assert(off->learned.commits == 0);
    const float off_from = off->g_last;
    dsd_speech_agc_open_at(off, open_db, 1);
    run_ctrl_gains(off, voiced + (size_t)k * F + F / 2, F / 2, 1, g);
    const float off_at = g[F / 2 - 1];
    const float off_step = off->learned.gain_db;
    const float off_to = powf(10.0f, (off->learned.gain_db + off->learned.atten_db) / 20.0f);
    assert(off->learned.commits > 0 && off->learned.steps == 1 && fabsf(off_step - open_db) > 1.0f);
    run_ctrl_gains(off, voiced + (size_t)(k + 1) * F, F, 1, g);
    SAY("open_at with the stage off, half a frame before the first commit (frame %d): the step takes it to %+.2f dB "
        "from %+.2f dB, reached one frame later\n",
        k, (double)off_step, (double)(20.0f * log10f(off_at)));
    assert(off_at > off_from && off_at < to);
    assert(is_ramp(g, F, off_at, off_to));
    free(off);
    free(voiced);
}

/* dsd_speech_agc_settled_gain_db(): 0 (the output untouched) until 20 frames have committed, then 1 with the settled
   aim clamp(T - S) of that moment, within the controller's range; 0 again after a reset. */
static void
test_settled_gain_db(void) {
    const int rate = 8000;
    static const float g0s[] = {4.0f, -20.0f}; /* the second clamps the aim at its top, -20 + 30 dB */
    for (size_t gi = 0; gi < 2; gi++) {
        const float g0 = g0s[gi];
        const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
        const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
        const size_t F = (size_t)frame_len(rate);
        const size_t n = secs(rate, 4.0);
        float* x = alloc_f(n);
        speech_into(x, n, rate, 245, -36.0, 130.0, 0.0);
        dsd_speech_agc* agc = new_agc(rate, g0);
        const float sentinel = -999.0f;
        float got = sentinel;
        assert(dsd_speech_agc_settled_gain_db(NULL, &got) == 0 && dsd_speech_agc_settled_gain_db(agc, NULL) == 0);
        int before = 0;
        int after = 0;
        float last = 0.0f;
        for (size_t p = 0; p + F <= n; p += F) {
            dsd_speech_agc_process(agc, x + p, F, 1, 1, 0);
            got = sentinel;
            const int r = dsd_speech_agc_settled_gain_db(agc, &got);
            if (agc->learned.commits < 20) {
                assert(r == 0 && same_bits(&got, &sentinel, sizeof got));
                before++;
                continue;
            }
            const float want = clampf(agc->target_db - agc->learned.level_db, lo, hi);
            assert(r == 1 && same_bits(&got, &want, sizeof got));
            assert(dsd_speech_agc_settled_gain_db(agc, NULL) == 0);
            after++;
            last = got;
        }
        SAY("settled gain (start %+.0f dB): 0 for %d frames, then 1 for %d, last %+.2f dB\n", (double)g0, before, after,
            (double)last);
        assert(before > 0 && after > 50);
        if (gi == 1) {
            assert(fabsf(last - hi) < 1e-6f);
        }
        dsd_speech_agc_reset(agc);
        got = sentinel;
        assert(dsd_speech_agc_settled_gain_db(agc, &got) == 0 && same_bits(&got, &sentinel, sizeof got));
        free(agc);
        free(x);
    }
}

/* ------------------------------------------------------------------------------------------- acquisition stage */

#define ACQ_WEIGHT_C         24.0f
#define ACQ_CAP_DBFS         (-12.0f)
#define ACQ_BURST_FLOOR_DBFS (-17.0f)
#define ACQ_PEAK_ABS_DBFS    (-70.0f)

/* The startup aim's bounds from the acquisition state @p d: the burst floor (when a burst has completed over three held
   levels) and the loudest frame's cap (when one is known); -1e30 / +1e30 when absent. */
static float
acq_burst_floor(const dsd_speech_agc_learned* d, float lo, float hi) {
    if (d->bursts < 1 || d->peaks < DSD_SPEECH_AGC_PEAKS) {
        return -1e30f;
    }
    return clampf(ACQ_BURST_FLOOR_DBFS - d->peak_db[DSD_SPEECH_AGC_PEAKS - 1], lo, hi);
}

static float
acq_loud_cap(const dsd_speech_agc_learned* d, float lo, float hi) {
    return d->loudest_known ? clampf(ACQ_CAP_DBFS - d->loudest_db, lo, hi) : 1e30f;
}

/* The startup law's blend for a step that finds @p b before it and leaves @p a: P + (W - P) n^2 / (n^2 + 24). */
static float
acq_blend(const dsd_speech_agc* agc, const dsd_speech_agc_learned* b, const dsd_speech_agc_learned* a) {
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const float w = clampf(agc->target_db - a->level_db, lo, hi);
    const float p = b->steps == 0 ? b->gain_db : b->prior_db;
    const float n2 = (float)a->commits * (float)a->commits;
    return p + (w - p) * (n2 / (n2 + ACQ_WEIGHT_C));
}

/* The gain a frame's step leaves (its acquisition update not yet applied): the blend, within the burst floor and the
   loudest frame's cap of the state before it while fewer than 20 frames have committed, straight for the first 20
   steps and at +0.12 / -0.20 dB after them; the gain before it on a frame that does not step. */
static float
acq_after_step(const dsd_speech_agc* agc, const dsd_speech_agc_learned* b, const dsd_speech_agc_learned* a) {
    if (a->steps == b->steps) {
        return b->gain_db;
    }
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    float aim = acq_blend(agc, b, a);
    if (a->commits < 20) {
        const float floor_db = acq_burst_floor(b, lo, hi);
        const float cap = acq_loud_cap(b, lo, hi);
        aim = aim > floor_db ? aim : floor_db;
        aim = aim < cap ? aim : cap;
    }
    if (b->steps < 20) {
        return aim;
    }
    return b->gain_db + clampf(aim - b->gain_db, -0.20f, 0.12f);
}

/* The acquisition state a frame of level @p level at or over the absolute floor leaves: the held loudest frame (a frame
   within 6 dB of it renews the hold; more than 10 frames after the last such one it eases 1 dB a frame; a louder frame
   takes it at once), the three loudest levels at or over -70 dBFS (descending), and the bursts over the last of
   them. */
static void
acq_expected_state(const dsd_speech_agc_learned* b, float level, dsd_speech_agc_learned* e) {
    *e = *b;
    if (b->loudest_known) {
        e->loudest_age = level >= b->loudest_db - 6.0f ? 0 : b->loudest_age + 1;
        if (e->loudest_age > 10) {
            e->loudest_db = b->loudest_db - 1.0f;
        }
    }
    if (!b->loudest_known || level > e->loudest_db) {
        e->loudest_db = level;
        e->loudest_age = 0;
    }
    e->loudest_known = 1;
    if (level >= ACQ_PEAK_ABS_DBFS) {
        int at = 0;
        while (at < e->peaks && !(level > e->peak_db[at])) {
            at++;
        }
        for (int i = DSD_SPEECH_AGC_PEAKS - 1; i > at; i--) {
            e->peak_db[i] = e->peak_db[i - 1];
        }
        if (at < DSD_SPEECH_AGC_PEAKS) {
            e->peak_db[at] = level;
            e->peaks += e->peaks < DSD_SPEECH_AGC_PEAKS;
        }
    }
    if (e->peaks < DSD_SPEECH_AGC_PEAKS) {
        return;
    }
    const float third = e->peak_db[DSD_SPEECH_AGC_PEAKS - 1];
    if (level >= third - 6.0f) {
        e->high_run++;
        e->burst_armed |= e->high_run >= 3;
        return;
    }
    const int low = level <= third - 12.0f;
    if (e->burst_armed && low) {
        e->burst_armed = 0;
        e->bursts++;
    }
    if (low || !e->burst_armed) {
        e->high_run = 0;
    }
}

static int
acq_state_equal(const dsd_speech_agc_learned* a, const dsd_speech_agc_learned* e) {
    int ok = same_bits(&a->prior_db, &e->prior_db, sizeof(float)) && same_bits(&a->open_db, &e->open_db, sizeof(float))
             && a->loudest_known == e->loudest_known && a->loudest_age == e->loudest_age && a->peaks == e->peaks
             && a->high_run == e->high_run && a->burst_armed == e->burst_armed && a->bursts == e->bursts;
    ok = ok && (!e->loudest_known || same_bits(&a->loudest_db, &e->loudest_db, sizeof(float)));
    for (int i = 0; i < e->peaks; i++) {
        ok = ok && same_bits(&a->peak_db[i], &e->peak_db[i], sizeof(float));
    }
    return ok;
}

/* The acquisition state an eligible frame under the absolute floor (digital silence) leaves: once three levels are
   held it is a low frame for the burst watch (it completes an armed burst and ends a run of high frames), and nothing
   else. */
static void
acq_expected_silence(const dsd_speech_agc_learned* b, dsd_speech_agc_learned* e) {
    *e = *b;
    if (b->peaks < DSD_SPEECH_AGC_PEAKS) {
        return;
    }
    if (b->burst_armed) {
        e->burst_armed = 0;
        e->bursts++;
    }
    e->high_run = 0;
}

/* For an eligible frame @p f at or over the absolute floor, from @p g, the gain its step left: the acquisition state
   it leaves in @p e (acq_expected_state()), and the gain it leaves after the return, the guard and the burst rise
   (check_acq_law()). */
static float
acq_sound_gain(const dsd_speech_agc* agc, const frame_rec* f, float g, dsd_speech_agc_learned* e) {
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const dsd_speech_agc_learned* a = &f->after;
    acq_expected_state(&f->before, f->level, e);
    if (a->commits == 0 && f->level + g <= ACQ_CAP_DBFS) {
        const float held = clampf(ACQ_CAP_DBFS - e->loudest_db, lo, hi);
        const float to = f->before.open_db < held ? f->before.open_db : held;
        if (g < to) {
            g += to - g < 1.0f ? to - g : 1.0f;
        }
    }
    const int early = a->commits < 20;
    if (early && f->level + g > ACQ_CAP_DBFS) {
        const float capped = clampf(ACQ_CAP_DBFS - f->level, lo, hi);
        g = capped < g ? capped : g;
    }
    const float cap = clampf(ACQ_CAP_DBFS - e->loudest_db, lo, hi);
    float floor_db = acq_burst_floor(e, lo, hi);
    floor_db = floor_db < cap ? floor_db : cap;
    if (early && g < floor_db) {
        g += floor_db - g < 1.0f ? floor_db - g : 1.0f;
    }
    return g;
}

/* The acquisition stage on every frame of @p t: the step (acq_after_step()), then for an eligible frame at or over the
   absolute floor its own level: the held loudest frame (acq_expected_state()); while nothing has committed and the
   frame does not play over -12 dBFS, the return of at most 1 dB toward min(the opening gain, clamp(-12 - held
   loudest)); while fewer than 20 frames have committed, the guard (a frame that would play over -12 dBFS takes the gain
   to clamp(-12 - L) when that is lower) and, once a burst has completed, the rise of at most 1 dB toward
   min(clamp(-17 - third loudest), clamp(-12 - held loudest)) (acq_sound_gain()). An eligible frame under the floor is
   a low frame for the burst watch and moves no gain (acq_expected_silence()); a frame that is not eligible changes
   nothing. Recomputed in the AGC's own float order from each frame's state before and after it, so it holds bit for
   bit, with the state itself. Returns the frames where it does not. */
static int
check_acq_law(const dsd_speech_agc* agc, const trace_t* t) {
    int bad = 0;
    for (size_t i = 0; i < t->count; i++) {
        const frame_rec* f = &t->f[i];
        const dsd_speech_agc_learned* a = &f->after;
        float g = acq_after_step(agc, &f->before, a);
        dsd_speech_agc_learned e = f->before;
        if (f->eligible && f->level < agc->abs_floor_db) {
            acq_expected_silence(&f->before, &e);
        } else if (f->eligible) {
            g = acq_sound_gain(agc, f, g, &e);
        }
        e.prior_db = (a->steps != f->before.steps && f->before.steps == 0) ? f->before.gain_db : f->before.prior_db;
        bad += !same_bits(&g, &a->gain_db, sizeof g) || !acq_state_equal(a, &e);
    }
    return bad;
}

/* The acquisition state with the stage off, or after a reset: 0, but for the opening gain @p open_db. */
static void
acq_off_state(float open_db, dsd_speech_agc_learned* z) {
    DSD_MEMSET(z, 0, sizeof *z);
    z->open_db = open_db;
}

/* Today's law with the acquisition stage off, on every frame of @p t, bit for bit: a step moves G by
   clamp(clamp(T - S, lo, hi) - G, -dn, +up) (2 dB for the first 20 steps, then +0.12 / -0.20), every other frame
   leaves G alone, and the acquisition state stays 0 (the opening gain as it was). Returns the frames where it does
   not. */
static int
check_off_law(const dsd_speech_agc* agc, const trace_t* t) {
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    int bad = 0;
    for (size_t i = 0; i < t->count; i++) {
        const dsd_speech_agc_learned* b = &t->f[i].before;
        const dsd_speech_agc_learned* a = &t->f[i].after;
        float g = b->gain_db;
        if (a->steps != b->steps) {
            const float want = clampf(agc->target_db - a->level_db, lo, hi);
            const float up = b->steps < 20 ? 2.0f : 0.12f;
            const float dn = b->steps < 20 ? 2.0f : 0.20f;
            g += clampf(want - g, -dn, up);
        }
        dsd_speech_agc_learned zero;
        acq_off_state(b->open_db, &zero);
        bad += !same_bits(&g, &a->gain_db, sizeof g) || !acq_state_equal(a, &zero);
    }
    return bad;
}

/* Blocks of unvoiced sound at @p rate: @p hi_frames frames of voice-band noise at @p hi_db, then @p lo_frames at
   @p lo_db, the same samples in every block, @p reps times. Returns the samples written. */
static size_t
synth_blocks(float* x, int rate, int reps, int hi_frames, double hi_db, int lo_frames, double lo_db, uint64_t seed) {
    const size_t F = (size_t)frame_len(rate);
    const size_t nh = (size_t)hi_frames * F;
    const size_t nl = (size_t)lo_frames * F;
    assert(dsd_test_synth_noise(x, nh, rate, seed, DSD_TEST_NOISE_VOICE_BAND, hi_db) == 0);
    assert(dsd_test_synth_noise(x + nh, nl, rate, seed + 1U, DSD_TEST_NOISE_VOICE_BAND, lo_db) == 0);
    for (int r = 1; r < reps; r++) {
        DSD_MEMCPY(x + (size_t)r * (nh + nl), x, (nh + nl) * sizeof(float));
    }
    return (size_t)reps * (nh + nl);
}

/* Blocks as synth_blocks() writes them, with each block's @p lo_frames frames zero-filled (digital silence). */
static size_t
synth_blocks_in_silence(float* x, int rate, int reps, int hi_frames, double hi_db, int lo_frames, uint64_t seed) {
    const size_t F = (size_t)frame_len(rate);
    const size_t nh = (size_t)hi_frames * F;
    const size_t nl = (size_t)lo_frames * F;
    const size_t n = synth_blocks(x, rate, reps, hi_frames, hi_db, lo_frames, -68.0, seed);
    for (int r = 0; r < reps; r++) {
        DSD_MEMSET(x + (size_t)r * (nh + nl) + nh, 0, nl * sizeof(float));
    }
    return n;
}

/* The first frame of @p t from @p from on whose step or update left the gain below the gain before it, or -1. */
static long
first_cut(const trace_t* t, long from) {
    for (long i = from; i < (long)t->count; i++) {
        if (t->f[i].after.gain_db < t->f[i].before.gain_db) {
            return i;
        }
    }
    return -1;
}

/* The steps of a reception with the acquisition stage on, from its first commit @p k1: each of the first 20 goes to
   the aim for its n (acq_after_step()), then the frame's own level may cut it (the guard) or a burst completing on it
   raise it by at most 1 dB; later frames move by at most +0.12 / -0.20 dB. Returns the startup steps; @p aimed gets
   those that stayed at the aim and @p later the frames after them. */
static int
check_startup_steps(const dsd_speech_agc* agc, const trace_t* t, long k1, int* aimed, int* later) {
    int startup = 0;
    *aimed = 0;
    *later = 0;
    for (size_t i = (size_t)k1; i < t->count; i++) {
        const frame_rec* f = &t->f[i];
        if (f->before.steps >= 20) {
            const float dg = f->after.gain_db - f->before.gain_db;
            assert(dg <= 0.12f + 1e-5f && dg >= -0.20f - 1e-5f);
            (*later)++;
            continue;
        }
        if (f->after.steps == f->before.steps) {
            continue;
        }
        const float aim = acq_after_step(agc, &f->before, &f->after);
        const int cut = f->after.commits < 20 && f->level + aim > -12.0f;
        const int rose = !cut && f->after.bursts >= 1 && f->after.gain_db > aim;
        if (cut) {
            assert(f->after.gain_db < aim);
        } else if (rose) {
            assert(f->after.gain_db - aim <= 1.0f + 1e-6f);
        } else {
            assert(fabsf(f->after.gain_db - aim) < 1e-6f);
        }
        startup++;
        *aimed += !cut && !rose;
    }
    return startup;
}

/* The startup law on a talker 20 dB under the level the start gain suits (its settled aim W 20-21 dB over the start
   gain once 12 frames have committed), with the acquisition stage on: the first commit (the run of six) takes the
   gain exactly to P + 0.6 (W - P), P the start gain and W the settled aim of that moment; every further step of the
   first 20 goes to the aim for its n and later ones move by at most +0.12 / -0.20 dB (check_startup_steps()), and
   every frame follows the stage's law bit for bit. Once 12 frames have committed the gain is within 3 dB of the
   settled aim (the law leaves 1 - 144/168 of the way). With the stage off the same talker still
   steps 2 dB at a time and is more than 6 dB short then. */
static void
test_acq_startup_law(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const size_t n = secs(rate, 10.0);
    float* x = alloc_f(n);
    speech_into(x, n, rate, 250, -42.2, 130.0, 0.0);
    float short_db[2] = {0.0f, 0.0f};
    for (int acq = 1; acq >= 0; acq--) {
        trace_t t;
        float* y = alloc_f(n);
        DSD_MEMCPY(y, x, n * sizeof(float));
        dsd_speech_agc* agc = acq ? new_acq_agc(rate, g0) : new_agc(rate, g0);
        trace_init(&t, n / (size_t)agc->frame_len);
        run_traced(agc, y, n, NULL, k_learn, &t);
        const long k1 = first_commit(&t, 0);
        const long k12 = frame_of_committed(&t, 0, 12);
        assert(k1 >= 5 && k12 > k1 && t.f[k1].committed == DSD_SPEECH_AGC_RUN_FRAMES);
        const frame_rec* f1 = &t.f[k1];
        const float w1 = clampf(agc->target_db - f1->after.level_db, lo, hi);
        const float w12 = clampf(agc->target_db - t.f[k12].after.level_db, lo, hi);
        short_db[acq] = w12 - t.f[k12].after.gain_db;
        int startup = 0;
        int aimed = 0;
        int later = 0;
        if (acq) {
            assert(check_acq_law(agc, &t) == 0);
            assert(w12 - g0 >= 20.0f && w12 - g0 <= 21.0f);
            assert(fabsf(f1->before.gain_db - g0) < 1e-6f && f1->before.bursts == 0);
            assert(fabsf(f1->after.gain_db - (g0 + 0.6f * (w1 - g0))) < 1e-4f);
            assert(same_bits(&f1->after.prior_db, &f1->before.gain_db, sizeof(float)));
            startup = check_startup_steps(agc, &t, k1, &aimed, &later);
            assert(startup == 20 && aimed >= 10 && later > 100);
        } else {
            talker_result r;
            DSD_MEMSET(&r, 0, sizeof r);
            check_gain_law(agc, &t, &r);
            assert(r.law_bad == 0 && r.held_bad == 0 && check_off_law(agc, &t) == 0);
            assert(fabsf(f1->after.gain_db - f1->before.gain_db - 2.0f) < 1e-5f);
        }
        SAY("startup law, acquisition %s: first commit at frame %ld: G %+.2f -> %+.2f (W %+.2f); after 12 committed "
            "frames (frame %ld) G %+.2f, W %+.2f, short by %.2f dB; %d startup steps (%d at the aim), %d frames "
            "later\n",
            acq ? "on " : "off", k1, (double)f1->before.gain_db, (double)f1->after.gain_db, (double)w1, k12,
            (double)t.f[k12].after.gain_db, (double)w12, (double)short_db[acq], startup, aimed, later);
        trace_free(&t);
        free(agc);
        free(y);
    }
    assert(short_db[1] >= 0.0f && short_db[1] <= 3.0f && short_db[0] > 6.0f);
    free(x);
}

/* With the acquisition stage on, the first commit after dsd_speech_agc_open_at() takes the opened gain as P: a quiet
   voiced sound opened 10 dB over the start gain steps to P + 0.6 (W - P) from there. */
static void
test_acq_open_at_prior(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t n = secs(rate, 1.0);
    float* x = alloc_f(n);
    voiced_params vp;
    voiced_defaults(&vp, 120.0, 0.015, 255);
    synth_voiced(x, n, rate, &vp, -44.0);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    dsd_speech_agc_open_at(agc, g0 + 10.0f, 0);
    trace_t t;
    trace_init(&t, n / (size_t)agc->frame_len);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0);
    const long k1 = first_commit(&t, 0);
    assert(k1 >= 0);
    const frame_rec* f1 = &t.f[k1];
    const float w1 = clampf(agc->target_db - f1->after.level_db, g0 - 12.0f, g0 + 30.0f);
    const float p = g0 + 10.0f;
    SAY("open_at then commit: P %+.2f, W %+.2f, first step to %+.2f\n", (double)f1->after.prior_db, (double)w1,
        (double)f1->after.gain_db);
    assert(fabsf(f1->after.prior_db - p) < 1e-6f && fabsf(f1->after.gain_db - (p + 0.6f * (w1 - p))) < 1e-4f);
    trace_free(&t);
    free(agc);
    free(x);
}

/* The output of each whole frame of @p y in dBFS RMS, into @p out (one per frame of @p t). */
static void
frame_rms(const float* y, size_t F, const trace_t* t, double* out) {
    for (size_t i = 0; i < t->count; i++) {
        out[i] = rms_dbfs(y + i * F, F);
    }
}

/* The loud-frame guard: a talker the start gain suits, opened 8 dB over it. The first frame that would play over -12
   dBFS takes the gain to exactly -12 - L for its level L. After the next frame (the ramp) no frame plays more than -12
   dBFS RMS while fewer than 20 frames have committed, unless it is louder than every frame up to two frames before it,
   and then by no more than that: the guard acts on a frame after it has played. Once 20 frames have committed a loud
   frame no longer lowers the gain at once; before, it does. */
static void
test_acq_loud_guard(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t F = (size_t)frame_len(rate);
    const size_t n = secs(rate, 6.0);
    float* x = alloc_f(n);
    speech_into(x, n, rate, 260, -24.0, 150.0, 0.0);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    dsd_speech_agc_open_at(agc, g0 + 8.0f, 0);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0);
    const long j = first_cut(&t, 0);
    assert(j >= 0 && t.f[j].after.steps == 0 && t.f[j].level + t.f[j].before.gain_db > -12.0f);
    assert(fabsf(t.f[j].after.gain_db - (-12.0f - t.f[j].level)) < 1e-6f);
    double* out = (double*)calloc(t.count, sizeof(double));
    assert(out != NULL);
    frame_rms(x, F, &t, out);
    int checked = 0;
    int newer = 0;
    double worst = -1e9;
    for (long k = j + 2; k < (long)t.count && t.f[k - 1].after.commits < 20; k++) {
        const double over = (double)t.f[k].level - (double)t.f[k - 2].after.loudest_db;
        const double bound = -12.0 + (over > 0.0 ? over : 0.0);
        newer += over > 0.0;
        worst = out[k] - bound > worst ? out[k] - bound : worst;
        assert(out[k] <= bound + 0.01);
        checked++;
    }
    SAY("loud guard: opened %+.0f dB, first cut at frame %ld (level %.2f dBFS) to %+.2f dB; %d frames after the ramp "
        "checked (%d louder than all before), worst %+.3f dB against the bound\n",
        (double)(g0 + 8.0f), j, (double)t.f[j].level, (double)t.f[j].after.gain_db, checked, newer, worst);
    assert(checked >= 10);
    /* A loud frame after 20 commits leaves G alone; the same frame early cuts it. */
    assert(agc->learned.commits >= 20);
    float* loud = alloc_f(F);
    float* loud2 = alloc_f(F);
    assert(dsd_test_synth_noise(loud, F, rate, 261, DSD_TEST_NOISE_VOICE_BAND, -6.0) == 0);
    DSD_MEMCPY(loud2, loud, F * sizeof(float));
    const float g_late = agc->learned.gain_db;
    dsd_speech_agc_process(agc, loud, F, 1, 1, 0);
    assert(same_bits(&agc->learned.gain_db, &g_late, sizeof g_late));
    dsd_speech_agc* early = new_acq_agc(rate, g0);
    dsd_speech_agc_process(early, loud2, F, 1, 1, 0);
    assert(early->learned.gain_db < g0 - 1.0f);
    free(early);
    free(loud2);
    free(loud);
    free(out);
    trace_free(&t);
    free(agc);
    free(x);
}

/* The burst trace @p t of a fresh instance at start gain @p g0 on repeated bursts whose first completes on frame 5:
   from there the gain rises exactly 1 dB a frame up to -17 - H, H the frame's third loudest level (the loudest frame's
   cap above it), then holds. Returns the frame it got there; @p rises gets the frames of 1 dB. */
static long
check_burst_rise(const trace_t* t, float g0, int* rises) {
    for (long i = 0; i < 5; i++) {
        assert(fabsf(t->f[i].after.gain_db - g0) < 1e-6f && t->f[i].after.bursts == 0);
    }
    assert(t->f[5].after.bursts == 1);
    long reached = -1;
    *rises = 0;
    for (long i = 5; i < (long)t->count; i++) {
        const frame_rec* f = &t->f[i];
        const float floor_db = -17.0f - f->after.peak_db[2];
        assert(-12.0f - f->after.loudest_db > floor_db);
        const float g = f->before.gain_db;
        if (reached >= 0) {
            assert(same_bits(&f->after.gain_db, &g, sizeof g)); /* holds */
        } else if (g + 1.0f < floor_db) {
            assert(fabsf(f->after.gain_db - (g + 1.0f)) < 1e-6f);
            (*rises)++;
        } else {
            assert(fabsf(f->after.gain_db - floor_db) < 1e-6f);
            reached = i;
        }
    }
    return reached;
}

/* The burst fallback on unvoiced bursts (voice-band noise, 5 frames at -45 dBFS, then 10 at -68, repeated: nothing
   commits): no change through the first burst; from the frame that completes it the gain rises exactly 1 dB a frame
   up to -17 - H, H the third loudest frame (two frames of one level are two of the three), and holds there
   (check_burst_rise()). Bursts then loud speech: the guard cuts the gain on the speech's first loud frame, and the
   first voiced commits take over from there. */
static void
test_acq_burst_fallback(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t F = (size_t)frame_len(rate);
    const size_t ns = secs(rate, 6.0);
    float* x = alloc_f(120 * F + ns);
    const size_t nb = synth_blocks(x, rate, 8, 5, -45.0, 10, -68.0, 270);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    trace_t t;
    trace_init(&t, nb / F);
    run_traced(agc, x, nb, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.commits == 0);
    int rises = 0;
    const long reached = check_burst_rise(&t, g0, &rises);
    SAY("bursts: complete at frame 5, the gain rises 1 dB a frame for %d frames and reaches %+.2f dB (-17 - %.2f) at "
        "frame %ld, then holds; %d bursts\n",
        rises, (double)t.f[reached].after.gain_db, (double)t.f[reached].after.peak_db[2], reached, agc->learned.bursts);
    assert(rises >= 20 && reached > 5 && reached < (long)t.count - 30 && agc->learned.bursts == 8);
    trace_free(&t);
    /* Bursts then loud speech. */
    speech_into(x + nb, ns, rate, 271, -24.0, 140.0, 0.0);
    trace_init(&t, ns / F);
    run_traced(agc, x + nb, ns, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0);
    long first_loud = -1;
    for (long i = 0; i < (long)t.count && first_loud < 0; i++) {
        first_loud = t.f[i].level + t.f[i].before.gain_db > -12.0f ? i : -1;
    }
    const long cut = first_cut(&t, 0);
    const long k1 = first_commit(&t, 0);
    float settled = 0.0f;
    assert(dsd_speech_agc_settled_gain_db(agc, &settled) == 1);
    SAY("bursts then speech: first loud frame %ld, cut at frame %ld to %+.2f dB; first commit at frame %ld from %+.2f; "
        "G at the end %+.2f, settled %+.2f\n",
        first_loud, cut, (double)t.f[cut].after.gain_db, k1, (double)t.f[k1].before.gain_db,
        (double)agc->learned.gain_db, (double)settled);
    assert(first_loud >= 0 && cut == first_loud && k1 > cut);
    assert(fabsf(t.f[cut].after.gain_db - (-12.0f - t.f[cut].level)) < 1e-6f);
    assert(same_bits(&t.f[k1].after.prior_db, &t.f[k1].before.gain_db, sizeof(float)));
    assert(fabsf(agc->learned.gain_db - settled) <= 2.0f);
    trace_free(&t);
    free(agc);
    free(x);
}

/* No burst completes, so the gain never rises, on sound that does not rise and fall: steady noise at -45 dBFS, silence
   (-80 dBFS) then steady noise (a step up with no fall), two-frame blips at -45 dBFS over -75, and bursts all under
   -70 dBFS, which never enter the three loudest. */
static void
test_acq_no_bursts(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t F = (size_t)frame_len(rate);
    const size_t n = secs(rate, 4.0);
    float* x = alloc_f(n);
    static const char* names[] = {"steady noise", "silence then noise", "two-frame blips", "bursts under -70 dBFS"};
    for (int c = 0; c < 4; c++) {
        size_t m = n;
        if (c == 0) {
            assert(dsd_test_synth_noise(x, m, rate, 272, DSD_TEST_NOISE_VOICE_BAND, -45.0) == 0);
        } else if (c == 1) {
            const size_t q = secs(rate, 1.0);
            assert(dsd_test_synth_noise(x, q, rate, 273, DSD_TEST_NOISE_VOICE_BAND, -80.0) == 0);
            assert(dsd_test_synth_noise(x + q, m - q, rate, 274, DSD_TEST_NOISE_VOICE_BAND, -45.0) == 0);
        } else if (c == 2) {
            m = synth_blocks(x, rate, 13, 2, -45.0, 13, -75.0, 275);
        } else {
            m = synth_blocks(x, rate, 13, 5, -72.0, 10, -83.0, 277);
        }
        dsd_speech_agc* agc = new_acq_agc(rate, g0);
        trace_t t;
        trace_init(&t, m / F);
        run_traced(agc, x, m, NULL, k_learn, &t);
        SAY("no bursts, %s: %d bursts, %d levels held, G %+.2f dB\n", names[c], agc->learned.bursts, agc->learned.peaks,
            (double)agc->learned.gain_db);
        assert(check_acq_law(agc, &t) == 0);
        assert(agc->learned.bursts == 0 && agc->learned.commits == 0 && fabsf(agc->learned.gain_db - g0) < 1e-6f);
        assert(c != 3 || agc->learned.peaks == 0);
        trace_free(&t);
        free(agc);
    }
    free(x);
}

/* Bursts that end in digital silence (5 frames of voice-band noise at -45 dBFS, then 10 zero-filled frames, repeated:
   nothing commits) complete as the same bursts with quiet noise between them do: the first silent frame after each
   burst is its low frame. Silence moves no gain and leaves the held loudest frame and its age alone; from the first
   burst on, the gain rises exactly 1 dB on each frame at or above the absolute floor until it plays the third loudest
   frame at -17 dBFS, then holds. */
static void
test_acq_silence_completes_bursts(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(120 * F);
    const size_t n = synth_blocks_in_silence(x, rate, 8, 5, -45.0, 10, 270);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    SAY("bursts ending in silence: %d bursts, G %+.2f dB\n", agc->learned.bursts, (double)agc->learned.gain_db);
    assert(agc->learned.bursts == 8 && agc->learned.commits == 0);
    assert(check_acq_law(agc, &t) == 0);
    long reached = -1;
    int rises = 0;
    for (long i = 0; i < (long)t.count; i++) {
        const frame_rec* f = &t.f[i];
        const float g = f->before.gain_db;
        if (f->level < agc->abs_floor_db) {
            assert(same_bits(&f->after.gain_db, &g, sizeof g) && f->after.loudest_age == f->before.loudest_age
                   && same_bits(&f->after.loudest_db, &f->before.loudest_db, sizeof(float)));
            continue;
        }
        assert(f->after.bursts == f->before.bursts); /* only silence completes these bursts */
        const float floor_db = -17.0f - f->after.peak_db[2];
        if (f->before.bursts == 0 || reached >= 0) {
            assert(same_bits(&f->after.gain_db, &g, sizeof g));
        } else if (g + 1.0f < floor_db) {
            assert(fabsf(f->after.gain_db - (g + 1.0f)) < 1e-6f);
            rises++;
        } else {
            assert(fabsf(f->after.gain_db - floor_db) < 1e-6f);
            reached = i;
        }
    }
    assert(t.f[5].level < agc->abs_floor_db && t.f[4].after.bursts == 0 && t.f[5].after.bursts == 1);
    /* The same bursts with -68 dBFS noise between them. */
    float* y = alloc_f(120 * F);
    assert(synth_blocks(y, rate, 8, 5, -45.0, 10, -68.0, 270) == n);
    dsd_speech_agc* noisy = new_acq_agc(rate, g0);
    dsd_speech_agc_process(noisy, y, n, 1, 1, 0);
    SAY("bursts ending in silence: the gain rises 1 dB on %d frames of sound and reaches %+.2f dB (-17 - %.2f) at "
        "frame %ld, then holds; with quiet noise between the bursts %d bursts, %+.2f dB\n",
        rises, (double)agc->learned.gain_db, (double)agc->learned.peak_db[2], reached, noisy->learned.bursts,
        (double)noisy->learned.gain_db);
    assert(rises >= 20 && reached > 15 && reached < (long)t.count - 30 && noisy->learned.bursts == 8);
    assert(fabsf(agc->learned.gain_db - (-17.0f - agc->learned.peak_db[2])) < 1e-6f);
    free(noisy);
    free(y);
    trace_free(&t);
    free(agc);
    free(x);
}

/* Two-frame blips at -45 dBFS with pauses of 5 frames after each, 6 times, then 40 frames of quiet noise at -68 dBFS,
   into @p agc, every frame traced: the blips and the noise learn, the pauses (zero-filled) take @p pause. */
static void
run_blips(dsd_speech_agc* agc, flags_t pause, trace_t* t) {
    const int rate = agc->rate_hz;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(82 * F);
    const size_t nb = synth_blocks_in_silence(x, rate, 6, 2, -45.0, 5, 290);
    assert(dsd_test_synth_noise(x + nb, 40 * F, rate, 291, DSD_TEST_NOISE_VOICE_BAND, -68.0) == 0);
    trace_init(t, 82);
    for (size_t b = 0; b < 6; b++) {
        run_traced(agc, x + b * 7 * F, 2 * F, NULL, k_learn, t);
        run_traced(agc, x + b * 7 * F + 2 * F, 5 * F, NULL, pause, t);
    }
    run_traced(agc, x + nb, 40 * F, NULL, k_learn, t);
    assert(t->count == 82);
    free(x);
}

/* A run of high frames does not go on across digital silence: two-frame blips with silent pauses between them never
   make three high frames in a row, so the quiet noise after them completes no burst and the gain stays at the opening.
   Pauses that are not eligible (learn 0: undecodable, a tone, not admitted) are invisible: they neither end the run
   nor complete the burst it arms, and the quiet noise completes it (one burst; the gain rises). With the stage off the
   silent pauses change nothing (check_off_law()). */
static void
test_acq_silence_ends_high_run(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const flags_t closed = {0, 1, 0};
    /* Eligible silence. */
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    trace_t t;
    run_blips(agc, k_learn, &t);
    int longest = 0;
    int moved = 0;
    for (size_t i = 0; i < t.count; i++) {
        moved += !same_bits(&t.f[i].after.gain_db, &g0, sizeof g0);
        longest = t.f[i].after.high_run > longest ? t.f[i].after.high_run : longest;
    }
    SAY("blips between silences: %d bursts, longest run %d, %d frames off the opening gain, G %+.2f dB\n",
        agc->learned.bursts, longest, moved, (double)agc->learned.gain_db);
    assert(agc->learned.bursts == 0 && moved == 0 && longest == 2 && agc->learned.peaks == DSD_SPEECH_AGC_PEAKS);
    assert(check_acq_law(agc, &t) == 0);
    trace_free(&t);
    free(agc);
    /* Pauses that are not eligible. */
    dsd_speech_agc* unheard = new_acq_agc(rate, g0);
    run_blips(unheard, closed, &t);
    assert(check_acq_law(unheard, &t) == 0);
    int armed_pauses = 0;
    for (size_t i = 0; i < 42; i++) {
        const frame_rec* f = &t.f[i];
        if (!f->eligible) {
            assert(acq_state_equal(&f->after, &f->before)
                   && same_bits(&f->after.gain_db, &f->before.gain_db, sizeof(float)));
            armed_pauses += f->before.burst_armed && f->after.burst_armed && f->after.bursts == 0;
        }
    }
    SAY("blips between closed pauses: %d pauses armed and kept, %d bursts (the first at frame 42: %d), G %+.2f dB\n",
        armed_pauses, unheard->learned.bursts, t.f[42].after.bursts, (double)unheard->learned.gain_db);
    assert(armed_pauses >= 15 && t.f[41].after.bursts == 0 && t.f[42].after.bursts == 1);
    assert(unheard->learned.bursts == 1 && unheard->learned.gain_db > g0 + 20.0f);
    trace_free(&t);
    free(unheard);
    /* The stage off. */
    dsd_speech_agc* off = new_agc(rate, g0);
    run_blips(off, k_learn, &t);
    dsd_speech_agc_learned zero;
    acq_off_state(g0, &zero);
    assert(check_off_law(off, &t) == 0 && acq_state_equal(&off->learned, &zero));
    assert(same_bits(&off->learned.gain_db, &g0, sizeof g0));
    trace_free(&t);
    free(off);
}

/* The burst fallback and voiced commits together: one burst starts the gain rising, then voiced sound at the bursts'
   level commits. The first commit takes the gain the bursts left as P, and while fewer than 20 frames have committed
   no step leaves the gain under the burst floor (within the loudest frame's cap), though the blend alone would. */
static void
test_acq_burst_then_voiced(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const size_t F = (size_t)frame_len(rate);
    const size_t nv = secs(rate, 3.0);
    float* x = alloc_f(15 * F + nv);
    const size_t nb = synth_blocks(x, rate, 1, 5, -45.0, 10, -68.0, 280);
    voiced_params vp;
    voiced_defaults(&vp, 120.0, 0.015, 281);
    synth_voiced(x + nb, nv, rate, &vp, -45.0);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    trace_t t;
    trace_init(&t, (nb + nv) / F);
    run_traced(agc, x, nb + nv, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0);
    const long k1 = first_commit(&t, 0);
    assert(k1 > 15 && t.f[k1].before.bursts == 1 && t.f[k1].before.gain_db > g0 + 5.0f);
    assert(same_bits(&t.f[k1].after.prior_db, &t.f[k1].before.gain_db, sizeof(float)));
    const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
    int steps = 0;
    int bound = 0;
    for (size_t i = (size_t)k1; i < t.count && t.f[i].after.commits < 20; i++) {
        const frame_rec* f = &t.f[i];
        if (f->after.steps == f->before.steps) {
            continue;
        }
        const float floor_db = acq_burst_floor(&f->before, lo, hi);
        const float cap = acq_loud_cap(&f->before, lo, hi);
        const float least = floor_db < cap ? floor_db : cap;
        const float step = acq_after_step(agc, &f->before, &f->after);
        assert(step >= least);
        steps++;
        bound += acq_blend(agc, &f->before, &f->after) < least;
    }
    SAY("burst then voiced: first commit at frame %ld from P %+.2f; %d early steps, the floor held %d of them\n", k1,
        (double)t.f[k1].after.prior_db, steps, bound);
    assert(steps >= 10 && bound >= 1);
    trace_free(&t);
    free(agc);
    free(x);
}

/* A pop (@p pop_frames frames of voice-band noise at @p pop_db), @p gap_frames frames of digital silence, then
   @p quiet_frames frames of voice-band noise at @p quiet_db, into @p x. Returns the samples written. */
static size_t
synth_pop(float* x, int rate, int pop_frames, double pop_db, int gap_frames, int quiet_frames, double quiet_db,
          uint64_t seed) {
    const size_t F = (size_t)frame_len(rate);
    const size_t np = (size_t)pop_frames * F;
    const size_t ng = (size_t)gap_frames * F;
    const size_t nq = (size_t)quiet_frames * F;
    assert(dsd_test_synth_noise(x, np, rate, seed, DSD_TEST_NOISE_VOICE_BAND, pop_db) == 0);
    DSD_MEMSET(x + np, 0, ng * sizeof(float));
    if (nq > 0) {
        assert(dsd_test_synth_noise(x + np + ng, nq, rate, seed + 1U, DSD_TEST_NOISE_VOICE_BAND, quiet_db) == 0);
    }
    return np + ng + nq;
}

/* The louder of a trace's first two frames: the pop of the cases below. */
static float
pop_level(const trace_t* t) {
    return t->f[0].level > t->f[1].level ? t->f[0].level : t->f[1].level;
}

/* A pop, then quieter sound, nothing committed, opened 8 dB over the start gain: the pop's two frames cut the gain to
   where the louder of them plays at -12 dBFS. @p gap frames of digital silence after the pop (under the absolute floor)
   do not count toward the hold. The ten quieter frames that follow leave the gain and the held level where they are;
   from the eleventh the held level eases 1 dB a frame, the gain returns with it 1 dB a frame, and it stops exactly at
   the opening gain. */
static void
test_acq_pop_returns(int gap) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const float open = g0 + 8.0f;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f((size_t)(2 + gap + 40) * F);
    const size_t n = synth_pop(x, rate, 2, -10.0, gap, 40, -40.0, 300);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    dsd_speech_agc_open_at(agc, open, 0);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.commits == 0 && agc->learned.bursts == 0);
    const float pop = pop_level(&t);
    const float cut = -12.0f - pop;
    assert(t.f[0].after.gain_db < open && same_bits(&t.f[1].after.gain_db, &cut, sizeof cut));
    const long q = 2 + gap; /* the first quieter frame */
    for (long i = 1; i < q + 10; i++) {
        const frame_rec* f = &t.f[i];
        assert(same_bits(&f->after.gain_db, &cut, sizeof cut) && same_bits(&f->after.loudest_db, &pop, sizeof pop));
        assert(f->after.loudest_age == (i < q ? 0 : (int)(i - q + 1)));
    }
    /* 14 frames of exactly 1 dB from the eleventh quieter frame (frame 12 after the pop alone), then the last part of a
       dB onto the opening gain (frame 26), where the gain stays. */
    const int ones = (int)(open - cut);
    const long land = q + 10 + ones;
    float held = pop;
    for (long i = q + 10; i < (long)t.count; i++) {
        const frame_rec* f = &t.f[i];
        if (i <= land) {
            held -= 1.0f;
            assert(same_bits(&f->after.loudest_db, &held, sizeof held));
        }
        if (i < land) {
            assert(fabsf(f->after.gain_db - f->before.gain_db - 1.0f) < 1e-4f);
        } else {
            assert(same_bits(&f->after.gain_db, &open, sizeof open));
        }
    }
    SAY("pop then quieter sound (%d frames of silence between): pop %.2f dBFS cuts %+.2f to %+.2f dB, held through "
        "frame %ld, %d rises of 1 dB from frame %ld, at %+.2f dB from frame %ld\n",
        gap, (double)pop, (double)open, (double)cut, q + 9, ones, q + 10, (double)agc->learned.gain_db, land);
    assert(ones == 14 && q + 10 == 12 + gap && land == 26 + gap && t.count == (size_t)(q + 40));
    trace_free(&t);
    free(agc);
    free(x);
}

/* A loud talker that does not commit (three loud unvoiced frames, then eight quieter ones: gaps shorter than the hold,
   repeated), opened 8 dB over the start gain: the cut stays where the loudest frame plays at -12 dBFS, the held level
   never eases, and the gain never rises. Its bursts complete, but their floor lies under the cut. */
static void
test_acq_loud_unvoiced_holds(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(110U * F);
    const size_t n = synth_blocks(x, rate, 10, 3, -15.0, 8, -40.0, 305);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    dsd_speech_agc_open_at(agc, g0 + 8.0f, 0);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.commits == 0 && agc->learned.bursts >= 5);
    float loud = t.f[0].level;
    for (long i = 1; i < 3; i++) {
        loud = t.f[i].level > loud ? t.f[i].level : loud;
    }
    const float cut = clampf(-12.0f - loud, lo, hi);
    int oldest = 0;
    for (long i = 0; i < (long)t.count; i++) {
        const frame_rec* f = &t.f[i];
        assert(f->after.gain_db <= f->before.gain_db);
        if (i >= 3) {
            assert(same_bits(&f->after.gain_db, &cut, sizeof cut)
                   && same_bits(&f->after.loudest_db, &loud, sizeof loud));
            oldest = f->after.loudest_age > oldest ? f->after.loudest_age : oldest;
        }
    }
    SAY("loud unvoiced talker: loudest %.2f dBFS, cut to %+.2f dB and held for %zu frames (longest gap %d frames), "
        "%d bursts\n",
        (double)loud, (double)cut, t.count - 3U, oldest, agc->learned.bursts);
    assert(oldest == 8);
    trace_free(&t);
    free(agc);
    free(x);
}

/* The return stops at the first commit: a pop, then a voiced talker at -26 dBFS whose first run commits before the
   pop's hold ends, opened 8 dB over the start gain. The first step starts from the pop's cut (P) and every frame after
   it is the startup law's step exactly (acq_after_step()), though on some of them (10 measured: once the hold has
   eased past the aim and before it comes to rest over the talker) the gain stands under both the opening gain and the
   held level's cap, where a return would have lifted it. */
static void
test_acq_return_stops_at_commit(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const float open = g0 + 8.0f;
    const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const size_t F = (size_t)frame_len(rate);
    const size_t nv = secs(rate, 2.0);
    float* x = alloc_f(2 * F + nv);
    const size_t np = synth_pop(x, rate, 2, -10.0, 0, 0, 0.0, 310);
    voiced_params vp;
    voiced_defaults(&vp, 120.0, 0.015, 311);
    synth_voiced(x + np, nv, rate, &vp, -26.0);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    dsd_speech_agc_open_at(agc, open, 0);
    trace_t t;
    trace_init(&t, (np + nv) / F);
    run_traced(agc, x, np + nv, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.bursts == 0);
    const float cut = -12.0f - pop_level(&t);
    const long k1 = first_commit(&t, 0);
    assert(k1 > 2 && k1 < 12);
    assert(same_bits(&t.f[k1].before.gain_db, &cut, sizeof cut));
    assert(same_bits(&t.f[k1].after.prior_db, &cut, sizeof cut));
    int would = 0;
    for (long i = k1; i < (long)t.count; i++) {
        const frame_rec* f = &t.f[i];
        const float step = acq_after_step(agc, &f->before, &f->after);
        assert(f->after.commits > 0 && same_bits(&f->after.gain_db, &step, sizeof step));
        const float held = clampf(-12.0f - f->after.loudest_db, lo, hi);
        would += f->level + step <= -12.0f && step < (open < held ? open : held);
    }
    SAY("return stops at the first commit: first commit at frame %ld from the cut %+.2f dB; %d frames the law left "
        "under the opening gain and the held cap; G at the end %+.2f dB\n",
        k1, (double)cut, would, (double)agc->learned.gain_db);
    assert(would >= 8 && agc->learned.gain_db < open - 1.0f);
    trace_free(&t);
    free(agc);
    free(x);
}

/* A pop before a talker's first voiced run, then 40 frames of quieter sound: the pop's hold eases and the gain returns
   to the opening gain, so the first step goes to the startup law's aim P + 0.6 (W - P) from there, where a loudest
   frame that never eased would have capped it at the pop's cut. */
static void
test_acq_pop_eased_before_talker(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const size_t F = (size_t)frame_len(rate);
    const size_t nv = secs(rate, 2.0);
    float* x = alloc_f(42 * F + nv);
    const size_t np = synth_pop(x, rate, 2, -10.0, 0, 40, -50.0, 315);
    voiced_params vp;
    voiced_defaults(&vp, 120.0, 0.015, 317);
    synth_voiced(x + np, nv, rate, &vp, -40.0);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    trace_t t;
    trace_init(&t, (np + nv) / F);
    run_traced(agc, x, np + nv, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0);
    const long k1 = first_commit(&t, 0);
    assert(k1 > 42);
    const frame_rec* f1 = &t.f[k1];
    const float never = clampf(-12.0f - pop_level(&t), lo, hi);
    const float aim = acq_blend(agc, &f1->before, &f1->after);
    SAY("pop eased before the talker: first commit at frame %ld from %+.2f dB to the aim %+.2f dB; a loudest frame "
        "that never eased would have capped it at %+.2f dB\n",
        k1, (double)f1->before.gain_db, (double)aim, (double)never);
    assert(same_bits(&f1->before.gain_db, &g0, sizeof g0) && same_bits(&f1->after.prior_db, &g0, sizeof g0));
    assert(same_bits(&f1->after.gain_db, &aim, sizeof aim) && aim > never + 10.0f);
    trace_free(&t);
    free(agc);
    free(x);
}

/* The held level after a pop: ten quieter frames hold it, then it eases exactly 1 dB a frame and comes to rest within
   6 dB over the quieter sound (a frame that near renews the hold); a new louder frame takes it at once. */
static void
test_acq_held_peak_rests(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(103 * F);
    size_t n = synth_pop(x, rate, 2, -10.0, 0, 80, -40.0, 320);
    assert(dsd_test_synth_noise(x + n, F, rate, 322, DSD_TEST_NOISE_VOICE_BAND, -20.0) == 0);
    assert(dsd_test_synth_noise(x + n + F, 20 * F, rate, 323, DSD_TEST_NOISE_VOICE_BAND, -55.0) == 0);
    n += 21 * F;
    dsd_speech_agc* agc = new_acq_agc(rate, 4.0f);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.commits == 0);
    const float pop = pop_level(&t);
    for (long i = 2; i < 12; i++) {
        assert(same_bits(&t.f[i].after.loudest_db, &pop, sizeof pop));
    }
    long rest_at = 12;
    while (rest_at < 82 && t.f[rest_at].after.loudest_age != 0) {
        const float eased = t.f[rest_at].before.loudest_db - 1.0f;
        assert(same_bits(&t.f[rest_at].after.loudest_db, &eased, sizeof eased));
        rest_at++;
    }
    const float rest = t.f[rest_at].after.loudest_db;
    float top = -1e30f;
    for (long i = rest_at; i < 82; i++) {
        assert(same_bits(&t.f[i].after.loudest_db, &rest, sizeof rest));
        top = t.f[i].level > top ? t.f[i].level : top;
    }
    const frame_rec* louder = &t.f[82];
    SAY("held level: pop %.2f dBFS, eased from frame 12 to rest at %.2f dBFS from frame %ld (the quieter sound's "
        "loudest frame %.2f); a frame at %.2f takes it\n",
        (double)pop, (double)rest, rest_at, (double)top, (double)louder->level);
    assert(rest_at > 30 && rest_at < 60 && rest >= top && rest - top <= 6.0f);
    assert(same_bits(&louder->after.loudest_db, &louder->level, sizeof(float)) && louder->after.loudest_age == 0);
    trace_free(&t);
    free(agc);
    free(x);
}

/* The burst fallback after a pop (then bursts of 5 frames at -45 dBFS and 10 at -68, repeated: nothing commits): the
   pop cuts the gain; while its hold lasts the fallback's rise stops at the held level's cap; once the hold has eased
   the rise reaches the burst floor -17 - H, H the third loudest frame, far over where the pop alone would have held
   it, and no rise ever goes past the held level's cap. */
static void
test_acq_burst_after_pop(void) {
    const int rate = 8000;
    const float g0 = 4.0f;
    const float lo = g0 - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = g0 + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(122 * F);
    const size_t np = synth_pop(x, rate, 2, -10.0, 0, 0, 0.0, 330);
    const size_t n = np + synth_blocks(x + np, rate, 8, 5, -45.0, 10, -68.0, 331);
    dsd_speech_agc* agc = new_acq_agc(rate, g0);
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.commits == 0 && agc->learned.bursts == 8);
    int bound = 0;
    for (size_t i = 0; i < t.count; i++) {
        const frame_rec* f = &t.f[i];
        const float held = clampf(-12.0f - f->after.loudest_db, lo, hi);
        assert(f->after.gain_db <= f->before.gain_db || f->after.gain_db <= held);
        bound += f->after.bursts >= 1 && same_bits(&f->after.gain_db, &held, sizeof held)
                 && held < clampf(-17.0f - f->after.peak_db[2], lo, hi);
    }
    const frame_rec* last = &t.f[t.count - 1U];
    const float floor_db = clampf(-17.0f - last->after.peak_db[2], lo, hi);
    const float never = clampf(-12.0f - pop_level(&t), lo, hi);
    SAY("bursts after a pop: the held level's cap bound the rise on %d frames; G at the end %+.2f dB (floor %+.2f), "
        "where the pop alone held it at %+.2f\n",
        bound, (double)agc->learned.gain_db, (double)floor_db, (double)never);
    assert(bound >= 3 && fabsf(agc->learned.gain_db - floor_db) < 1e-6f && floor_db > never + 20.0f);
    for (size_t i = t.count - 20U; i < t.count; i++) {
        assert(same_bits(&t.f[i].after.gain_db, &agc->learned.gain_db, sizeof(float)));
    }
    trace_free(&t);
    free(agc);
    free(x);
}

/* Leaving fixed mode with the stage on and nothing committed is a new opening: after a pop the gain returns to the
   fixed gain it left at, never toward the earlier opening. */
static void
test_acq_fixed_to_auto_stays(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    float* x = alloc_f(42 * F);
    const size_t n = synth_pop(x, rate, 2, -10.0, 0, 40, -40.0, 335);
    dsd_speech_agc* agc = new_acq_agc(rate, 4.0f);
    dsd_speech_agc_open_at(agc, 16.0f, 0);
    dsd_speech_agc_set_fixed(agc, 2.0f);
    dsd_speech_agc_set_fixed(agc, 0.0f);
    const float fixed_db = agc->learned.gain_db;
    assert(fabsf(fixed_db - 6.0206f) < 1e-3f && same_bits(&agc->learned.open_db, &fixed_db, sizeof fixed_db));
    trace_t t;
    trace_init(&t, n / F);
    run_traced(agc, x, n, NULL, k_learn, &t);
    assert(check_acq_law(agc, &t) == 0 && agc->learned.commits == 0);
    assert(t.f[1].after.gain_db < fixed_db - 5.0f);
    for (size_t i = 0; i < t.count; i++) {
        assert(t.f[i].after.gain_db <= fixed_db);
    }
    SAY("fixed then auto: opened +16, left fixed mode at %+.2f dB; the pop cut it to %+.2f, back at %+.2f dB\n",
        (double)fixed_db, (double)t.f[1].after.gain_db, (double)agc->learned.gain_db);
    assert(same_bits(&agc->learned.gain_db, &fixed_db, sizeof fixed_db));
    trace_free(&t);
    free(agc);
    free(x);
}

/* A reset forgets the acquisition state (the loudest frames, the bursts and P), opens at the start gain and keeps the
   stage on. */
static void
test_acq_reset_forgets(void) {
    const int rate = 8000;
    const size_t F = (size_t)frame_len(rate);
    const size_t nv = secs(rate, 2.0);
    float* x = alloc_f(30 * F + nv);
    const size_t nb = synth_blocks(x, rate, 2, 5, -45.0, 10, -68.0, 285);
    speech_into(x + nb, nv, rate, 286, -30.0, 140.0, 0.0);
    dsd_speech_agc* agc = new_acq_agc(rate, 4.0f);
    dsd_speech_agc_open_at(agc, 15.0f, 0);
    assert(fabsf(agc->learned.open_db - 15.0f) < 1e-6f);
    dsd_speech_agc_process(agc, x, nb + nv, 1, 1, 0);
    const dsd_speech_agc_learned* d = &agc->learned;
    assert(d->bursts >= 1 && d->loudest_known && d->peaks == DSD_SPEECH_AGC_PEAKS && d->steps > 0);
    assert(fabsf(d->prior_db) > 1.0f);
    dsd_speech_agc_reset(agc);
    dsd_speech_agc_learned zero;
    acq_off_state(4.0f, &zero);
    assert(acq_state_equal(d, &zero) && agc->acquisition == 1);
    dsd_speech_agc_set_acquisition(agc, 0);
    assert(agc->acquisition == 0);
    dsd_speech_agc_set_acquisition(agc, 5);
    assert(agc->acquisition == 1);
    dsd_speech_agc_set_acquisition(NULL, 1);
    free(agc);
    free(x);
}

/* With the acquisition stage off the gain follows today's law bit for bit and its state stays 0 (check_off_law()) on
   long mixed signals: the cross-rate material (speech, vowels and tones, 33.9 s) at 8 and 48 kHz and the partition
   stream (speech, DTMF, a tone, undecodable audio, closes, non-finite samples) through per-call flags at 8 kHz, each
   with several hundred commits; and a pop before a talker, which it never cuts, holds or returns from. */
static void
test_acq_off_is_off(void) {
    static const int rates[] = {8000, 48000};
    for (size_t ri = 0; ri < 2; ri++) {
        const int rate = rates[ri];
        const size_t n = secs(rate, XRATE_TOTAL_S);
        float* x = alloc_f(n);
        xrate_make(x, n, rate);
        dsd_speech_agc* agc = new_agc(rate, 4.0f);
        trace_t t;
        trace_init(&t, n / (size_t)agc->frame_len);
        run_traced(agc, x, n, NULL, k_learn, &t);
        const int bad = check_off_law(agc, &t);
        SAY("acquisition off, %d Hz mixed material: %d commits, %d steps, %d frames off today's law\n", rate,
            agc->learned.commits, agc->learned.steps, bad);
        assert(agc->learned.commits > 300 && bad == 0);
        trace_free(&t);
        free(agc);
        free(x);
    }
    stream_t s;
    stream_make(&s, 8000);
    float* y = alloc_f(s.n);
    dsd_speech_agc* agc = drive_fresh(&s, 8000, 4.0f, 0, 0, 0, y, 160);
    dsd_speech_agc_learned zero;
    acq_off_state(4.0f, &zero);
    assert(agc->learned.commits > 100 && acq_state_equal(&agc->learned, &zero));
    free(agc);
    free(y);
    stream_free(&s);
    /* A pop, quieter sound, then a talker, opened 8 dB over the start gain: no cut, no hold, no return. */
    {
        const size_t F = (size_t)frame_len(8000);
        const size_t nv = secs(8000, 3.0);
        float* x = alloc_f(42 * F + nv);
        const size_t np = synth_pop(x, 8000, 2, -10.0, 0, 40, -40.0, 340);
        speech_into(x + np, nv, 8000, 341, -30.0, 140.0, 0.0);
        dsd_speech_agc* opened = new_agc(8000, 4.0f);
        dsd_speech_agc_open_at(opened, 12.0f, 0);
        trace_t t;
        trace_init(&t, (np + nv) / F);
        run_traced(opened, x, np + nv, NULL, k_learn, &t);
        const int bad = check_off_law(opened, &t);
        SAY("acquisition off, a pop then a talker: G %+.2f dB after the pop, %d commits, %d frames off today's law\n",
            (double)t.f[1].after.gain_db, opened->learned.commits, bad);
        assert(bad == 0 && opened->learned.commits > 20
               && same_bits(&t.f[41].after.gain_db, &(float){12.0f}, sizeof(float)));
        acq_off_state(12.0f, &zero);
        assert(acq_state_equal(&opened->learned, &zero));
        trace_free(&t);
        free(opened);
        free(x);
    }
}

/* A non-finite (or absurdly large) sample comes out as 0 and leaves the state alone: the output around it is what the
   same signal without it gives. */
static void
test_non_finite(void) {
    const int rate = 8000;
    const size_t n = secs(rate, 4.0);
    float* x = alloc_f(n);
    speech_into(x, n, rate, 190, -30.0, 120.0, 0.0);
    static const size_t at[] = {0, 159, 160, 161, 5000, 5001, 20000};
    static const float bad[] = {NAN, INFINITY, -INFINITY, 1e20f, -NAN, 1e15f, NAN};
    const size_t nb = sizeof at / sizeof at[0];
    float* xi = alloc_f(n + nb);
    size_t w = 0;
    size_t b = 0;
    for (size_t i = 0; i < n; i++) {
        while (b < nb && at[b] == i) {
            xi[w++] = bad[b++];
        }
        xi[w++] = x[i];
    }
    assert(w == n + nb);
    dsd_speech_agc* a = new_agc(rate, 0.0f);
    dsd_speech_agc* c = new_agc(rate, 0.0f);
    dsd_speech_agc_process(a, x, n, 1, 1, 0);
    dsd_speech_agc_process(c, xi, n + nb, 1, 1, 0);
    w = 0;
    b = 0;
    for (size_t i = 0; i < n; i++) {
        while (b < nb && at[b] == i) {
            assert(xi[w] == 0.0f);
            w++;
            b++;
        }
        assert(same_bits(&xi[w], &x[i], sizeof(float)));
        w++;
    }
    assert(same_bits(&a->learned, &c->learned, sizeof a->learned));
    free(c);
    free(a);
    free(xi);
    free(x);
}

/* Fixed mode: the fixed gain exactly, nothing learned, tones not capped, the limiter still on; back to auto keeps the
   estimate and starts from the fixed gain clamped into range, with E at 0. The start gain takes effect at a reset. */
static void
test_fixed_mode_and_start_gain(void) {
    const int rate = 8000;
    const size_t n = secs(rate, 6.0);
    float* x = alloc_f(n);
    float* y = alloc_f(n);
    speech_into(x, n, rate, 200, -36.0, 120.0, 0.0);
    dsd_speech_agc* agc = new_agc(rate, 0.0f);
    assert(fabsf(dsd_speech_agc_gain_db(agc)) < 1e-5f);
    learn_talker(agc, rate, 4.0, 201, -36.0, 120.0);
    const dsd_speech_agc_learned learned = agc->learned;
    dsd_speech_agc_set_fixed(agc, 2.0f);
    assert(fabsf(dsd_speech_agc_gain_db(agc) - 6.0206f) < 1e-3f);
    DSD_MEMCPY(y, x, n * sizeof(float));
    run_traced(agc, y, n, NULL, k_learn, NULL);
    for (size_t i = 0; i < n; i++) {
        assert(same_bits(&(float){x[i] * 2.0f}, &y[i], sizeof(float)));
    }
    assert(agc->learned.commits == learned.commits && agc->learned.steps == learned.steps);
    assert(same_bits(&agc->learned.level_db, &learned.level_db, sizeof(float)));
    /* Leaving fixed mode: x100 (40 dB) clamps to the start gain + 30 dB. */
    dsd_speech_agc_set_fixed(agc, 100.0f);
    dsd_speech_agc_set_fixed(agc, 0.0f);
    assert(fabsf(agc->learned.gain_db - DSD_SPEECH_AGC_MAX_BOOST_DB) < 1e-5f && agc->learned.atten_db == 0.0f);
    assert(agc->learned.commits == learned.commits);
    assert(fabsf(20.0f * log10f(agc->g_cur) - DSD_SPEECH_AGC_MAX_BOOST_DB) < 1e-4f);
    dsd_speech_agc_set_fixed(agc, 0.5f);
    dsd_speech_agc_set_fixed(agc, -1.0f); /* anything not positive is auto */
    assert(!agc->fixed && fabsf(agc->learned.gain_db + 6.0206f) < 1e-3f);
    /* The start gain applies from the next reset. */
    dsd_speech_agc_set_start_gain_db(agc, 8.537f);
    assert(fabsf(agc->start_gain_db) < 1e-6f);
    dsd_speech_agc_reset(agc);
    assert(fabsf(agc->learned.gain_db - 8.537f) < 1e-6f && fabsf(dsd_speech_agc_gain_db(agc) - 8.537f) < 1e-4f);
    assert(fabsf(agc->abs_floor_db - (-20.0f - 8.537f - 60.0f)) < 1e-4f);
    assert(fabsf(agc->learned.noise_floor_db - agc->abs_floor_db) < 1e-6f);
    free(agc);
    free(y);
    free(x);
}

/* The standalone limiter: transparent under the ceiling, instant attack to it, 50 dB/s release, partition-free, and
   non-finite samples out as 0. */
static void
test_peak_limiter(void) {
    dsd_peak_limiter l;
    assert(dsd_peak_limiter_init(NULL, 8000, -1.0f) == -1);
    assert(dsd_peak_limiter_init(&l, 0, -1.0f) == -1);
    assert(dsd_peak_limiter_init(&l, 8000, 1.0f) == -1);
    assert(dsd_peak_limiter_init(&l, 8000, NAN) == -1);
    assert(dsd_peak_limiter_init(&l, 8000, -91.0f) == -1);
    assert(dsd_peak_limiter_init(&l, 8000, -90.0f) == 0 && l.ceiling == 1.0f);
    assert(dsd_peak_limiter_init(&l, 8000, -1.0f) == 0);
    assert(l.ceiling == 29204.0f && l.gain == 1.0f);
    float quiet[4] = {1000.0f, -29204.0f, 29204.0f, 0.5f};
    dsd_peak_limiter_process(&l, quiet, 4);
    assert(quiet[0] == 1000.0f && quiet[1] == -29204.0f && quiet[2] == 29204.0f && quiet[3] == 0.5f);
    float hit = 2.0f * 29204.0f;
    dsd_peak_limiter_process(&l, &hit, 1);
    assert(hit <= 29204.0f && hit > 29203.9f);
    assert(fabsf(20.0f * log10f(l.gain) + 6.0206f) < 1e-3f);
    /* 100 ms of quiet samples later the gain has released by 5 dB. */
    float* q = alloc_f(800);
    for (size_t i = 0; i < 800; i++) {
        q[i] = 100.0f;
    }
    dsd_peak_limiter_process(&l, q, 800);
    assert(fabsf(20.0f * log10f(l.gain) - (-6.0206f + 5.0f)) < 0.01f);
    float nf[3] = {NAN, INFINITY, 1e16f};
    const float g = l.gain;
    dsd_peak_limiter_process(&l, nf, 3);
    assert(nf[0] == 0.0f && nf[1] == 0.0f && nf[2] == 0.0f && same_bits(&g, &l.gain, sizeof g));
    dsd_peak_limiter_reset(&l);
    assert(l.gain == 1.0f);
    /* Partitions. */
    const size_t n = 48000;
    float* x = alloc_f(n);
    float* y1 = alloc_f(n);
    float* y2 = alloc_f(n);
    speech_into(x, n, 48000, 210, -6.0, 150.0, 0.0);
    DSD_MEMCPY(y1, x, n * sizeof(float));
    DSD_MEMCPY(y2, x, n * sizeof(float));
    dsd_peak_limiter a;
    dsd_peak_limiter b;
    assert(dsd_peak_limiter_init(&a, 48000, -1.0f) == 0 && dsd_peak_limiter_init(&b, 48000, -1.0f) == 0);
    dsd_peak_limiter_process(&a, y1, n);
    for (size_t i = 0; i < n; i += 37) {
        dsd_peak_limiter_process(&b, y2 + i, i + 37 <= n ? 37 : n - i);
    }
    assert(same_bits(y1, y2, n * sizeof(float)));
    assert(max_abs(y1, n) <= 29204.0f && max_abs(x, n) > 29204.0f);
    free(y2);
    free(y1);
    free(x);
    free(q);
}

int
main(int argc, char** argv) {
    int run_long = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
            (void)setvbuf(stdout, NULL, _IONBF, 0);
        } else if (strcmp(argv[i], "--long") == 0) {
            run_long = 1;
        }
    }
    if (run_long) {
        test_floor_recovery_after_long_hiss();
        test_veto_floor_recovery_after_long_hiss();
        DSD_FPRINTF(stdout, "DSP_SPEECH_AGC_LONG: OK\n");
        return 0;
    }
    test_init_and_sizes();
    test_established_talkers();
    test_first_commit_ramp();
    test_start_inside_speech_and_after_zeros();
    test_vowels(0);
    test_vowels(1);
    test_steady_sounds_rejected();
    test_pitch_edges_and_rumble(0);
    test_pitch_edges_and_rumble(1);
    test_noise_after_speech_and_snr();
    test_attenuator_hiss_and_quieting();
    test_attenuator_release_on_speech();
    test_noise_veto_hiss();
    test_tone_cap();
    test_decodability_return();
    test_rollback_matches_control(0);
    test_rollback_matches_control(1);
    test_rollback_keeps_the_commit_clock();
    test_partition_identity(0);
    test_partition_identity(1);
    test_cross_rate(0);
    test_cross_rate(1);
    test_ceiling();
    test_reset_is_fresh(0);
    test_reset_is_fresh(1);
    test_open_at();
    test_open_at_mid_frame();
    test_open_at_mid_frame_learning();
    test_settled_gain_db();
    test_acq_startup_law();
    test_acq_open_at_prior();
    test_acq_loud_guard();
    test_acq_burst_fallback();
    test_acq_no_bursts();
    test_acq_silence_completes_bursts();
    test_acq_silence_ends_high_run();
    test_acq_burst_then_voiced();
    test_acq_pop_returns(0);
    test_acq_pop_returns(25);
    test_acq_loud_unvoiced_holds();
    test_acq_return_stops_at_commit();
    test_acq_pop_eased_before_talker();
    test_acq_held_peak_rests();
    test_acq_burst_after_pop();
    test_acq_fixed_to_auto_stays();
    test_acq_reset_forgets();
    test_acq_off_is_off();
    test_non_finite();
    test_fixed_mode_and_start_gain();
    test_fixed_to_auto_then_close();
    test_reset_wins_over_the_fixed_gain();
    test_start_gain_range();
    test_stale_counters_saturate();
    test_peak_limiter();
    DSD_FPRINTF(stdout, "DSP_SPEECH_AGC: OK\n");
    return 0;
}
