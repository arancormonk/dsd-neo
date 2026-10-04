// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The NFM noise squelch (issue #518 follow-up), in sample time on seeded signals through the repository's own channel
 * taps and the discriminator full_demod() runs: the band rule and its sub-bands' response against the closed-form
 * Butterworth band-pass, a calibration that reproduces exactly and agrees with a long run, noise that reads about 0 dB
 * and never opens the gate, quieting that follows the carrier-to-noise ratio and not the input level, strong wanted
 * modulation that never closes it (tools/noise_squelch_model.py), the hysteresis, the flags' timing, and flags that do
 * not depend on how the samples are cut into blocks.
 */

#include <assert.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analog_tone_synth.h"

enum { MAX_TAPS = 512, HALF_PER_S = 50 };

static double
db(double ratio) {
    return 10.0 * log10(ratio);
}

/* The same double, bit for bit. */
static int
same_double(double a, double b) {
    uint64_t ua = 0U;
    uint64_t ub = 0U;
    DSD_MEMCPY(&ua, &a, sizeof ua);
    DSD_MEMCPY(&ub, &b, sizeof ub);
    return ua == ub;
}

static double
rated_deviation_hz(double width_hz) {
    const double a = (width_hz / 2.0) - 3000.0;
    const double b = width_hz / 8.0;
    const double dev = a > b ? a : b;
    return dev < 5000.0 ? dev : 5000.0;
}

/* ------------------------------------------------------------------------------------------------- channel plans */

typedef struct {
    int rate;
    int width; /* 0: no channel filter */
    float taps[MAX_TAPS];
    int taps_len;
    const float* hb;
    int hb_len;
    double sum_h2;
    dsd_noise_squelch_plan plan;
    int plan_rc;
} channel;

/* The channel taps full_demod() designs for @p width at @p rate (0: none), and the plan for them with the half-band
   stage @p hb (NULL: none). */
static void
channel_make(channel* ch, int rate, int width, const float* hb, int hb_len) {
    DSD_MEMSET(ch, 0, sizeof(*ch));
    ch->rate = rate;
    ch->width = width;
    ch->hb = hb;
    ch->hb_len = hb ? hb_len : 0;
    if (width > 0) {
        ch->taps_len = dsd_channel_lpf_design_analog(rate, width, ch->taps, MAX_TAPS);
        assert(ch->taps_len > 0);
    } else {
        ch->taps[0] = 1.0f;
        ch->taps_len = 1;
    }
    for (int i = 0; i < ch->taps_len; i++) {
        ch->sum_h2 += (double)ch->taps[i] * (double)ch->taps[i];
    }
    ch->plan_rc = dsd_noise_squelch_plan_design(&ch->plan, width > 0 ? ch->taps : NULL, width > 0 ? ch->taps_len : 0,
                                                hb, ch->hb_len, rate);
}

/* ------------------------------------------------------------------------------------------------ signal source */

/* White complex noise (through the half-band stage at twice the rate when the channel has one) plus an optional FM
   carrier, through the channel taps; then dsd_fm_demod()'s discriminator. */
typedef struct {
    synth_rng rng;
    const channel* ch;
    double hb_re[64];
    double hb_im[64];
    int hb_pos;
    double re[2 * MAX_TAPS];
    double im[2 * MAX_TAPS];
    int pos;
    double noise_sd; /* per component, at the channel input */
    double scale;    /* applied to the channel output (the input level) */
    int carrier;
    double amp;
    double carrier_phase;
    double carrier_step;
    double mod_phase;
    double mod_step;
    double fm_index;
    float prev_re;
    float prev_im;
    int have_prev;
} source;

static void
source_init(source* s, const channel* ch, uint64_t seed) {
    DSD_MEMSET(s, 0, sizeof(*s));
    synth_rng_seed(&s->rng, seed);
    s->ch = ch;
    s->noise_sd = sqrt(0.5);
    s->scale = 1.0;
}

static void
source_noise_only(source* s) {
    s->carrier = 0;
}

/* An FM carrier @p cnr_db above the channel-output noise, offset @p offset_hz, a @p tone_hz tone at @p dev_hz peak
   deviation (0: unmodulated). The half-band stage passes the channel's band at unity. */
static void
source_fm(source* s, double cnr_db, double offset_hz, double tone_hz, double dev_hz) {
    double hb_gain2 = 1.0;
    if (s->ch->hb_len > 0) {
        hb_gain2 = 0.0;
        for (int k = 0; k < s->ch->hb_len; k++) {
            hb_gain2 += (double)s->ch->hb[k] * (double)s->ch->hb[k];
        }
    }
    const double noise_out = 2.0 * s->noise_sd * s->noise_sd * hb_gain2 * s->ch->sum_h2;
    double sum_h = 0.0;
    for (int k = 0; k < s->ch->taps_len; k++) {
        sum_h += (double)s->ch->taps[k];
    }
    s->carrier = 1;
    s->amp = sqrt(noise_out * pow(10.0, cnr_db / 10.0)) / sum_h;
    s->carrier_step = 2.0 * M_PI * offset_hz / (double)s->ch->rate;
    s->mod_step = 2.0 * M_PI * tone_hz / (double)s->ch->rate;
    s->fm_index = tone_hz > 0.0 ? dev_hz / tone_hz : 0.0;
}

static void
source_noise_sample(source* s, double* xr, double* xi) {
    if (s->ch->hb_len <= 0) {
        *xr = s->noise_sd * synth_gauss(&s->rng);
        *xi = s->noise_sd * synth_gauss(&s->rng);
        return;
    }
    const int len = s->ch->hb_len;
    for (int k = 0; k < 2; k++) {
        s->hb_pos = (s->hb_pos + 1) % len;
        s->hb_re[s->hb_pos] = s->hb_re[s->hb_pos + len] = s->noise_sd * synth_gauss(&s->rng);
        s->hb_im[s->hb_pos] = s->hb_im[s->hb_pos + len] = s->noise_sd * synth_gauss(&s->rng);
    }
    double yr = 0.0;
    double yi = 0.0;
    for (int k = 0; k < len; k++) {
        yr += (double)s->ch->hb[k] * s->hb_re[s->hb_pos + len - k];
        yi += (double)s->ch->hb[k] * s->hb_im[s->hb_pos + len - k];
    }
    *xr = yr;
    *xi = yi;
}

/* dsd_fm_demod()'s phase step: the small-angle series where it holds, atan2f elsewhere. */
static float
disc_step(float im, float re) {
    if (re > 1.0e-7f && fabsf(im) <= (0.35f * re)) {
        const float x = im / re;
        const float x2 = x * x;
        return x * (1.0f + x2 * (-0.3333333333333333f + x2 * 0.2f));
    }
    return atan2f(im, re);
}

/* @p count channel samples into @p iq and their discriminator outputs into @p d. */
static void
source_run(source* s, float* iq, float* d, int count) {
    const int len = s->ch->taps_len;
    for (int n = 0; n < count; n++) {
        double xr = 0.0;
        double xi = 0.0;
        source_noise_sample(s, &xr, &xi);
        if (s->carrier) {
            const double ph = s->carrier_phase + (s->fm_index * sin(s->mod_phase));
            xr += s->amp * cos(ph);
            xi += s->amp * sin(ph);
            s->carrier_phase = fmod(s->carrier_phase + s->carrier_step, 2.0 * M_PI);
            s->mod_phase = fmod(s->mod_phase + s->mod_step, 2.0 * M_PI);
        }
        s->pos = (s->pos + 1) % len;
        s->re[s->pos] = s->re[s->pos + len] = xr;
        s->im[s->pos] = s->im[s->pos + len] = xi;
        double yr = 0.0;
        double yi = 0.0;
        for (int k = 0; k < len; k++) {
            yr += (double)s->ch->taps[k] * s->re[s->pos + len - k];
            yi += (double)s->ch->taps[k] * s->im[s->pos + len - k];
        }
        const float cr = (float)(yr * s->scale);
        const float cj = (float)(yi * s->scale);
        iq[(size_t)n * 2U] = cr;
        iq[((size_t)n * 2U) + 1U] = cj;
        if (!s->have_prev) {
            s->prev_re = cr;
            s->prev_im = cj;
            s->have_prev = 1;
        }
        const float re = (cr * s->prev_re) + (cj * s->prev_im);
        const float im = (cj * s->prev_re) - (cr * s->prev_im);
        d[n] = disc_step(im, re);
        s->prev_re = cr;
        s->prev_im = cj;
    }
}

/* ------------------------------------------------------------------------------------------------------- runner */

enum { CHUNK = 2048, MAX_WINDOWS = 4096 };

/* A squelch fed a source in chunks, recording each window's Q and the flags. */
typedef struct {
    dsd_noise_squelch t;
    uint64_t samples;
    uint64_t opens;       /* samples flagged open */
    int64_t first_open;   /* sample index of the first open flag since runner_mark() (-1: none) */
    int64_t first_closed; /* the same for a closed flag after an open one */
    uint64_t mark;
    double q[MAX_WINDOWS]; /* each window's Q since runner_mark() */
    int windows;
    uint64_t windows_seen;
} runner;

static void
runner_init(runner* r, const channel* ch, int threshold_db) {
    DSD_MEMSET(r, 0, sizeof(*r));
    dsd_noise_squelch_set_threshold(&r->t, threshold_db);
    dsd_noise_squelch_set_plan(&r->t, &ch->plan);
    r->first_open = -1;
    r->first_closed = -1;
}

static void
runner_mark(runner* r) {
    r->mark = r->samples;
    r->first_open = -1;
    r->first_closed = -1;
    r->opens = 0U;
    r->windows = 0;
}

static void
runner_feed(runner* r, source* s, int count) {
    static float iq[2 * CHUNK];
    static float d[CHUNK];
    static uint8_t flags[CHUNK];
    while (count > 0) {
        const int n = count < CHUNK ? count : CHUNK;
        source_run(s, iq, d, n);
        for (int i = 0; i < n; i++) {
            dsd_noise_squelch_process(&r->t, &d[i], &iq[(size_t)i * 2U], 1, &flags[i]);
            if (r->t.windows != r->windows_seen) {
                r->windows_seen = r->t.windows;
                if (r->windows < MAX_WINDOWS) {
                    r->q[r->windows++] = r->t.quieting_db;
                }
            }
            const uint64_t at = r->samples - r->mark;
            if ((flags[i] & DSD_SQUELCH_FLAG_CLOSED) == 0U) {
                r->opens++;
                if (r->first_open < 0) {
                    r->first_open = (int64_t)at;
                }
            } else if (r->first_open >= 0 && r->first_closed < 0) {
                r->first_closed = (int64_t)at;
            }
            r->samples++;
        }
        count -= n;
    }
}

static int
cmp_double(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/* The @p pct percentile of the runner's recorded windows (sorted copy), skipping the first @p skip. */
static double
runner_percentile(const runner* r, int skip, double pct) {
    static double sorted[MAX_WINDOWS];
    const int n = r->windows - skip;
    assert(n > 0);
    DSD_MEMCPY(sorted, &r->q[skip], (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), cmp_double);
    int at = (int)floor(pct / 100.0 * (double)(n - 1) + 0.5);
    if (at < 0) {
        at = 0;
    } else if (at >= n) {
        at = n - 1;
    }
    return sorted[at];
}

/* ------------------------------------------------------------------------------------------------------- tests */

/* The band: 3.8 kHz to the taps' -1 dB point less 800 Hz (under 0.45 fs), at least 1200 Hz of it, in 500 Hz sub-bands,
   at most nine. 8 and 10 kHz channels have none: the auto squelch runs for them. */
static void
test_band_rule(void) {
    static channel ch;
    assert(fabs(dsd_noise_squelch_passband_edge_hz(NULL, 0, 48000) - 24000.0) < 1e-9);

    channel_make(&ch, 48000, 8000, NULL, 0);
    assert(ch.plan_rc == -1 && !ch.plan.valid);
    channel_make(&ch, 48000, 10000, NULL, 0);
    assert(ch.plan_rc == -1 && !ch.plan.valid);

    channel_make(&ch, 48000, 12500, NULL, 0);
    assert(ch.plan_rc == 0 && ch.plan.valid);
    /* The analog designs pass to their width's edge and fall 1 dB a little under 200 Hz past it. */
    assert(ch.plan.edge_hz > 6250.0 && ch.plan.edge_hz < 6460.0);
    assert(fabs(ch.plan.lo_hz - 3800.0) < 1e-9);
    assert(fabs(ch.plan.hi_hz - (ch.plan.edge_hz - 800.0)) < 1e-9);
    assert(ch.plan.sub_bands == 3);

    channel_make(&ch, 48000, 16000, NULL, 0);
    assert(ch.plan.valid && ch.plan.sub_bands == (int)floor((ch.plan.hi_hz - 3800.0) / 500.0));
    channel_make(&ch, 48000, 25000, NULL, 0);
    assert(ch.plan.valid && ch.plan.sub_bands == DSD_NOISE_SQUELCH_MAX_SUB_BANDS);

    /* No channel filter: the band runs to 0.45 fs. */
    channel_make(&ch, 48000, 0, NULL, 0);
    assert(ch.plan.valid && fabs(ch.plan.edge_hz - 24000.0) < 1e-9 && fabs(ch.plan.hi_hz - 21600.0) < 1e-9);
    channel_make(&ch, 8000, 0, NULL, 0);
    assert(!ch.plan.valid);
    /* At 12 kHz the edge guard binds first: fs/2 - 800 Hz. */
    channel_make(&ch, 12000, 0, NULL, 0);
    assert(ch.plan.valid && fabs(ch.plan.hi_hz - 5200.0) < 1e-9 && ch.plan.sub_bands == 2);
    printf("band rule: ok\n");
}

/* Each sub-band's biquads are the Butterworth band-pass of prototype order 4: |H|^2 = 1 / (1 + ((W^2 - W0^2) /
   (W B))^8) on the pre-warped frequency W = 2 fs tan(pi f / fs), W0^2 = W1 W2 and B = W2 - W1 for its edges. */
static void
test_band_pass_response(void) {
    static channel ch;
    channel_make(&ch, 48000, 16000, NULL, 0);
    const double fs = 48000.0;
    const double step = (ch.plan.hi_hz - ch.plan.lo_hz) / (double)ch.plan.sub_bands;
    double worst = 0.0;
    for (int k = 0; k < ch.plan.sub_bands; k++) {
        const double f1 = ch.plan.lo_hz + (step * (double)k);
        const double w1 = 2.0 * fs * tan(M_PI * f1 / fs);
        const double w2 = 2.0 * fs * tan(M_PI * (f1 + step) / fs);
        for (int i = 1; i < 400; i++) {
            const double f = (fs / 2.0) * (double)i / 400.0;
            const double w = 2.0 * fs * tan(M_PI * f / fs);
            const double x = ((w * w) - (w1 * w2)) / (w * (w2 - w1));
            const double expect = 1.0 / (1.0 + pow(x, 8.0));
            const double om = 2.0 * M_PI * f / fs;
            double h2 = 1.0;
            for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
                const dsd_noise_squelch_biquad* q = &ch.plan.section[k][s];
                /* H = g (1 - z^-2) / (1 + a1 z^-1 + a2 z^-2) on the unit circle. */
                const double nr = 1.0 - cos(2.0 * om);
                const double ni = sin(2.0 * om);
                const double dr = 1.0 + (q->a1 * cos(om)) + (q->a2 * cos(2.0 * om));
                const double di = -(q->a1 * sin(om)) - (q->a2 * sin(2.0 * om));
                h2 *= q->gain * q->gain * ((nr * nr) + (ni * ni)) / ((dr * dr) + (di * di));
            }
            const double err = fabs(h2 - expect) / (expect > 1e-12 ? expect : 1e-12);
            if (expect > 1e-12 && err > worst) {
                worst = err;
            }
        }
    }
    printf("band-pass response: worst relative |H|^2 error %.2e\n", worst);
    assert(worst < 1e-8);
}

/* The calibration reproduces exactly, and 60 s of the same kind of noise agrees with it within the model's figures. */
static void
test_calibration(void) {
    static channel ch;
    static channel again;
    channel_make(&ch, 48000, 12500, hb_q15_taps, HB_TAPS);
    channel_make(&again, 48000, 12500, hb_q15_taps, HB_TAPS);
    assert(ch.plan.valid && dsd_noise_squelch_plan_equal(&ch.plan, &again.plan));
    for (int k = 0; k < DSD_NOISE_SQUELCH_MAX_SUB_BANDS; k++) {
        assert(same_double(ch.plan.p_ref[k], again.plan.p_ref[k]));
    }

    static source s;
    source_init(&s, &ch, 0x51C0FFEEULL);
    source_noise_only(&s);
    double s1[DSD_NOISE_SQUELCH_MAX_SUB_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double s2[DSD_NOISE_SQUELCH_MAX_SUB_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double e[DSD_NOISE_SQUELCH_MAX_SUB_BANDS];
    DSD_MEMSET(s1, 0, sizeof(s1));
    DSD_MEMSET(s2, 0, sizeof(s2));
    DSD_MEMSET(e, 0, sizeof(e));
    static float iq[2 * CHUNK];
    static float d[CHUNK];
    const int settle = 48000 / 10;
    const int total = 60 * 48000;
    int n_done = 0;
    while (n_done < total + settle) {
        source_run(&s, iq, d, CHUNK);
        for (int i = 0; i < CHUNK; i++, n_done++) {
            for (int k = 0; k < ch.plan.sub_bands; k++) {
                double y = (double)d[i];
                for (int q = 0; q < DSD_NOISE_SQUELCH_SECTIONS; q++) {
                    const dsd_noise_squelch_biquad* b = &ch.plan.section[k][q];
                    const double out = (b->gain * y) + s1[k][q];
                    s1[k][q] = s2[k][q] - (b->a1 * out);
                    s2[k][q] = (-b->gain * y) - (b->a2 * out);
                    y = out;
                }
                if (n_done >= settle) {
                    e[k] += y * y;
                }
            }
        }
    }
    double worst = 0.0;
    double sum = 0.0;
    for (int k = 0; k < ch.plan.sub_bands; k++) {
        const double p = e[k] / (double)(n_done - settle);
        sum += p;
        const double err = fabs(db(ch.plan.p_ref[k] / p));
        worst = err > worst ? err : worst;
    }
    const double sum_err = fabs(db(ch.plan.p_ref_sum / sum));
    printf("calibration: worst sub-band %.3f dB, whole band %.3f dB from 60 s of noise\n", worst, sum_err);
    assert(worst < 0.6);
    assert(sum_err < 0.3);
}

/* Noise alone reads about 0 dB and never reaches the lowest threshold (N = 3) at any plan; the gate stays shut. */
static void
test_noise_never_opens(void) {
    static const struct {
        int rate;
        int width;
        int hb;
    } plans[] = {
        {48000, 12500, 0}, {48000, 16000, 1}, {48000, 25000, 0}, {24000, 12500, 1}, {48000, 0, 1}, {96000, 20000, 0},
    };

    for (size_t p = 0; p < sizeof plans / sizeof plans[0]; p++) {
        static channel ch;
        channel_make(&ch, plans[p].rate, plans[p].width, plans[p].hb ? hb_q15_taps : NULL, plans[p].hb ? HB_TAPS : 0);
        assert(ch.plan.valid);
        static runner r;
        runner_init(&r, &ch, DSD_SQUELCH_MARGIN_MIN_DB);
        static source s;
        source_init(&s, &ch, 0xA11CE000ULL + (uint64_t)p);
        source_noise_only(&s);
        runner_mark(&r);
        runner_feed(&r, &s, 20 * plans[p].rate);
        const double median = runner_percentile(&r, 2, 50.0);
        const double top = runner_percentile(&r, 2, 100.0);
        printf("noise %d Hz at %d Hz: %d windows, median Q %+.2f dB, max %+.2f dB, %llu samples open\n", plans[p].width,
               plans[p].rate, r.windows, median, top, (unsigned long long)r.opens);
        assert(fabs(median) < 0.5);
        assert(top < 3.0);
        assert(r.opens == 0U);
    }
}

/* Q follows the carrier-to-noise ratio, not the input level: the same signal 40 dB quieter or louder reads the same. */
static void
test_level_independence(void) {
    static channel ch;
    channel_make(&ch, 48000, 12500, NULL, 0);
    static const double scales[] = {0.01, 1.0, 100.0};
    double q_ref[64] = {0};
    int n_ref = 0;
    for (size_t k = 0; k < sizeof scales / sizeof scales[0]; k++) {
        static runner r;
        runner_init(&r, &ch, 10);
        static source s;
        source_init(&s, &ch, 0x1E7E1ULL);
        s.scale = scales[k];
        source_fm(&s, 12.0, 0.0, 1000.0, rated_deviation_hz(12500.0));
        runner_mark(&r);
        runner_feed(&r, &s, 48000);
        if (k == 0) {
            n_ref = r.windows < 64 ? r.windows : 64;
            DSD_MEMCPY(q_ref, r.q, (size_t)n_ref * sizeof(double));
            continue;
        }
        assert(r.windows >= n_ref);
        for (int w = 0; w < n_ref; w++) {
            assert(fabs(r.q[w] - q_ref[w]) < 0.1);
        }
    }
    printf("level independence: x0.01..x100 within 0.1 dB over %d windows\n", n_ref);
}

/* Median Q rises with the CNR: about 0 dB on noise, the CNR plus a few dB on a carrier. */
static void
test_quieting_follows_cnr(void) {
    static channel ch;
    channel_make(&ch, 48000, 12500, NULL, 0);
    static const double cnrs[] = {0.0, 6.0, 10.0, 20.0, 30.0};
    double prev = -1e9;
    for (size_t k = 0; k < sizeof cnrs / sizeof cnrs[0]; k++) {
        static runner r;
        runner_init(&r, &ch, 10);
        static source s;
        source_init(&s, &ch, 0xC0FFEEULL + (uint64_t)k);
        source_fm(&s, cnrs[k], 0.0, 1000.0, rated_deviation_hz(12500.0));
        runner_mark(&r);
        runner_feed(&r, &s, 48000);
        const double median = runner_percentile(&r, 2, 50.0);
        printf("CNR %+.0f dB: median Q %.1f dB\n", cnrs[k], median);
        assert(median > prev + 2.0);
        prev = median;
        if (cnrs[k] >= 30.0) {
            assert(median > 28.0);
        }
        if (cnrs[k] <= 0.0) {
            assert(median < 5.0);
        }
    }
}

/* A strong carrier carrying a tone anywhere in 300-3000 Hz at the width's rated deviation, at every offset its Carson
   bandwidth allows, opens the gate at the highest threshold and never closes it: its harmonics fall in the band as
   lines, and the best sub-band stays clear of them. */
static void
test_strong_modulation_never_closes(void) {
    static const int widths[] = {12500, 16000, 25000};
    for (size_t w = 0; w < sizeof widths / sizeof widths[0]; w++) {
        static channel ch;
        channel_make(&ch, 48000, widths[w], NULL, 0);
        const double dev = rated_deviation_hz((double)widths[w]);
        const double room = ((double)widths[w] / 2.0) - dev - 3000.0;
        const double offsets[3] = {0.0, room, -room};
        const int n_offsets = room > 1.0 ? 3 : 1;
        double worst = 1e9;
        for (int o = 0; o < n_offsets; o++) {
            for (int tone = 300; tone <= 3000; tone += 100) {
                static runner r;
                runner_init(&r, &ch, DSD_SQUELCH_MARGIN_MAX_DB);
                static source s;
                source_init(&s, &ch, 0x70E5ULL + (uint64_t)tone + (uint64_t)o * 7919U);
                source_fm(&s, 60.0, offsets[o], (double)tone, dev);
                runner_mark(&r);
                runner_feed(&r, &s, 48000 / 4);
                assert(r.first_open >= 0);
                assert(r.first_closed < 0);
                for (int k = 1; k < r.windows; k++) {
                    worst = r.q[k] < worst ? r.q[k] : worst;
                }
            }
        }
        printf("strong modulation %d Hz (deviation %.0f Hz): never closed at N = 30, lowest Q %.1f dB\n", widths[w],
               dev, worst);
        assert(worst >= 33.0);
    }
}

/* Deterministic discriminator input for the gate logic: one tone at the centre of each sub-band, each at the amplitude
   that puts its sub-band at Pref_k / 10^(q/10). */
static void
gate_feed(dsd_noise_squelch* t, const dsd_noise_squelch_plan* plan, double q_db, int count, uint64_t* sample,
          uint8_t* flags_out) {
    const double step = (plan->hi_hz - plan->lo_hz) / (double)plan->sub_bands;
    const float iq[2] = {1.0f, 0.0f};
    for (int i = 0; i < count; i++) {
        double x = 0.0;
        for (int k = 0; k < plan->sub_bands; k++) {
            const double f = plan->lo_hz + (step * ((double)k + 0.5));
            const double amp = sqrt(2.0 * plan->p_ref[k] / pow(10.0, q_db / 10.0));
            x += amp * sin(2.0 * M_PI * f * (double)(*sample) / (double)plan->rate_hz);
        }
        const float d = (float)x;
        dsd_noise_squelch_process(t, &d, iq, 1, &flags_out[i]);
        (*sample)++;
    }
}

/* Open at Q >= N, stay open down to N - 3, close below; closed, stay closed under N. Q is measured first on the same
   input (each sub-band's leakage into its neighbours lowers it a little), then the levels are chosen with margin. */
static void
test_hysteresis(void) {
    static channel ch;
    channel_make(&ch, 48000, 16000, NULL, 0);
    static uint8_t flags[48000];
    dsd_noise_squelch t;
    DSD_MEMSET(&t, 0, sizeof(t));
    dsd_noise_squelch_set_threshold(&t, 20);
    dsd_noise_squelch_set_plan(&t, &ch.plan);
    uint64_t sample = 0U;
    /* The offset between the tones' nominal Q and what the squelch reads. */
    gate_feed(&t, &ch.plan, 30.0, 4800, &sample, flags);
    const double offset = 30.0 - t.quieting_db;
    printf("hysteresis: tone input reads %.2f dB under nominal\n", offset);
    assert(offset > -0.5 && offset < 3.0);
    dsd_noise_squelch_reset(&t);
    sample = 0U;
    const double above = 22.0 + offset;   /* reads 22: opens */
    const double between = 18.5 + offset; /* reads 18.5: holds an open gate, does not open a closed one */
    const double below = 15.5 + offset;   /* reads 15.5: closes */
    gate_feed(&t, &ch.plan, above, 4800, &sample, flags);
    assert(t.gate_open == 1);
    gate_feed(&t, &ch.plan, between, 9600, &sample, flags);
    assert(t.gate_open == 1);
    for (int i = 0; i < 9600; i++) {
        assert((flags[i] & DSD_SQUELCH_FLAG_CLOSED) == 0U);
    }
    gate_feed(&t, &ch.plan, below, 4800, &sample, flags);
    assert(t.gate_open == 0);
    gate_feed(&t, &ch.plan, between, 9600, &sample, flags);
    assert(t.gate_open == 0);
    for (int i = 4800; i < 9600; i++) {
        assert((flags[i] & DSD_SQUELCH_FLAG_CLOSED) != 0U);
    }
    gate_feed(&t, &ch.plan, above, 4800, &sample, flags);
    assert(t.gate_open == 1);

    /* N = 3 closes below 1.5 dB, not at 0 dB: noise reads about 0. */
    dsd_noise_squelch_set_threshold(&t, 3);
    assert(fabs(t.open_db - 3.0) < 1e-12 && fabs(t.close_db - 1.5) < 1e-12);
    dsd_noise_squelch_set_threshold(&t, 1);
    assert(t.threshold_db == 3);
    dsd_noise_squelch_set_threshold(&t, 99);
    assert(t.threshold_db == 30 && fabs(t.close_db - 27.0) < 1e-12);
    printf("hysteresis: ok\n");
}

/* The gate starts closed and the first window ends 40 ms in: a carrier there opens it from the next sample. */
static void
test_first_window_flags(void) {
    static channel ch;
    channel_make(&ch, 48000, 12500, NULL, 0);
    static runner r;
    runner_init(&r, &ch, 10);
    static source s;
    source_init(&s, &ch, 0xF1257ULL);
    source_fm(&s, 40.0, 0.0, 0.0, 0.0);
    runner_mark(&r);
    runner_feed(&r, &s, 4800);
    printf("first window: first open flag at sample %lld\n", (long long)r.first_open);
    assert(r.first_open == 2 * 48000 / HALF_PER_S);
}

/* The same samples cut into blocks any way give the same flags and the same windows, bit for bit. */
static void
test_block_cuts(void) {
    static channel ch;
    channel_make(&ch, 48000, 16000, hb_q15_taps, HB_TAPS);

    enum { TOTAL = 48000 * 3 };

    static float iq[2 * TOTAL];
    static float d[TOTAL];
    static source s;
    source_init(&s, &ch, 0xB10CCULL);
    /* Noise, a carrier, noise, a weaker carrier: several transitions. */
    const int seg = TOTAL / 6;
    for (int k = 0; k < 6; k++) {
        if (k == 1 || k == 2) {
            source_fm(&s, 25.0, 0.0, 1200.0, rated_deviation_hz(16000.0));
        } else if (k == 4) {
            source_fm(&s, 12.0, 0.0, 700.0, 2000.0);
        } else {
            source_noise_only(&s);
        }
        source_run(&s, &iq[(size_t)k * (size_t)seg * 2U], &d[(size_t)k * (size_t)seg], seg);
    }
    static uint8_t whole[TOTAL];
    static uint8_t cut[TOTAL];
    dsd_noise_squelch a;
    dsd_noise_squelch b;
    DSD_MEMSET(&a, 0, sizeof(a));
    DSD_MEMSET(&b, 0, sizeof(b));
    dsd_noise_squelch_set_threshold(&a, 8);
    dsd_noise_squelch_set_threshold(&b, 8);
    dsd_noise_squelch_set_plan(&a, &ch.plan);
    dsd_noise_squelch_set_plan(&b, &ch.plan);
    dsd_noise_squelch_process(&a, d, iq, TOTAL, whole);
    synth_rng rng;
    synth_rng_seed(&rng, 0xC475ULL);
    int at = 0;
    while (at < TOTAL) {
        int n = 1 + (int)(synth_uniform(&rng) * 1500.0);
        if (n > TOTAL - at) {
            n = TOTAL - at;
        }
        dsd_noise_squelch_process(&b, &d[at], &iq[(size_t)at * 2U], n, &cut[at]);
        at += n;
    }
    assert(memcmp(whole, cut, sizeof whole) == 0);
    assert(a.windows == b.windows && a.gate_open == b.gate_open);
    assert(same_double(a.quieting_db, b.quieting_db));
    int transitions = 0;
    for (int i = 1; i < TOTAL; i++) {
        transitions += whole[i] != whole[i - 1];
    }
    printf("block cuts: identical flags, %d transitions\n", transitions);
    assert(transitions >= 3);
}

/* Exact zeros (a muted capture) hold no signal and keep the gate shut; a noise-free carrier quiets the band to nothing
   and opens it. A squelch with no plan keeps its gate open. */
static void
test_zeros_and_no_plan(void) {
    static channel ch;
    channel_make(&ch, 48000, 12500, NULL, 0);
    static float iq[2 * 9600];
    static float d[9600];
    static uint8_t flags[9600];
    dsd_noise_squelch t;
    DSD_MEMSET(&t, 0, sizeof(t));
    dsd_noise_squelch_set_threshold(&t, 10);
    dsd_noise_squelch_set_plan(&t, &ch.plan);
    DSD_MEMSET(iq, 0, sizeof(iq));
    DSD_MEMSET(d, 0, sizeof(d));
    dsd_noise_squelch_process(&t, d, iq, 9600, flags);
    assert(t.gate_open == 0 && fabs(t.quieting_db) < 1e-12 && t.windows > 0U);
    for (int i = 0; i < 9600; i++) {
        iq[(size_t)i * 2U] = 0.5f;
    }
    dsd_noise_squelch_process(&t, d, iq, 9600, flags);
    assert(t.gate_open == 1 && t.quieting_db > 100.0);

    dsd_noise_squelch none;
    DSD_MEMSET(&none, 0, sizeof(none));
    DSD_MEMSET(flags, 0xFF, sizeof(flags));
    dsd_noise_squelch_process(&none, d, iq, 9600, flags);
    for (int i = 0; i < 9600; i++) {
        assert(flags[i] == 0U);
    }
    dsd_noise_squelch_status st;
    dsd_noise_squelch_get_status(&none, &st);
    assert(st.gate_open == 1);
    printf("zeros and no plan: ok\n");
}

int
main(void) {
    test_band_rule();
    test_band_pass_response();
    test_calibration();
    test_noise_never_opens();
    test_level_independence();
    test_quieting_follows_cnr();
    test_strong_modulation_never_closes();
    test_hysteresis();
    test_first_window_flags();
    test_block_cuts();
    test_zeros_and_no_plan();
    printf("DSP_NFM_NOISE_SQUELCH: OK\n");
    return 0;
}
