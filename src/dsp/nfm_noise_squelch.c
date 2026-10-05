// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The NFM noise squelch's band, calibration, quieting and gate (issue #518 follow-up); see nfm_noise_squelch.h. */

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The band (tools/noise_squelch_model.py): from just above voice to the channel edge less a guard, under 0.45 fs. */
static const double k_band_lo_hz = 3800.0;
static const double k_edge_guard_hz = 800.0;
static const double k_nyquist_fraction = 0.45;
static const double k_min_band_hz = 1200.0;
static const double k_sub_band_hz = 300.0;
/* With no channel filter the half-band cascade's gentle roll-off is the channel's edge, and it truncates any signal
   that reaches the rate's Nyquist: a plan needs the widest NFM signal the gate tests (5 kHz deviation, 3 kHz audio:
   8 kHz from the carrier) to fit under it. */
static const double k_unfiltered_signal_edge_hz = 8000.0;
/* The channel taps' edge: the first 10 Hz step 1 dB under their DC gain (10^(-1/20)). */
static const double k_edge_step_hz = 10.0;
static const double k_edge_ratio = 0.89125093813374556;
/* Q = max(Q_sum, Q_max - 4 dB); the gate closes 3 dB under N, and never under 1.5 dB (noise reads about 0 dB). */
static const double k_guard_db = 4.0;
static const double k_hysteresis_db = 3.0;
static const double k_close_floor_db = 1.5;
/* Calibration: 2 s of noise, the first 30 ms of each band-pass's output left out (its start-up). */
static const double k_calibration_s = 2.0;
static const double k_settle_s = 0.03;
/* A window at or below this mean |z|^2 holds no signal (exact zeros); a band's power at or below it is held there. */
static const double k_min_power = 1e-30;
/* Q never reads beyond +-200 dB (a noise-free synthetic carrier quiets the band to nothing). */
static const double k_q_cap_db = 200.0;
/* Biquad state this small is flushed to zero at each half-window (exact-zero input would otherwise go denormal). */
static const double k_state_flush = 1e-150;
/* Relative tolerance for plans that decide the same way. */
static const double k_plan_same = 1e-12;

/* Half-windows per second (20 ms); a window is two of them. */
enum { NSQ_HALVES_PER_S = 50, NSQ_HB_MAX = 64 };

/* ---------------------------------------------------------------------------------------------- complex helpers */

typedef struct {
    double re;
    double im;
} nsq_cplx;

static nsq_cplx
nsq_c(double re, double im) {
    nsq_cplx z = {re, im};
    return z;
}

static nsq_cplx
nsq_add(nsq_cplx a, nsq_cplx b) {
    return nsq_c(a.re + b.re, a.im + b.im);
}

static nsq_cplx
nsq_sub(nsq_cplx a, nsq_cplx b) {
    return nsq_c(a.re - b.re, a.im - b.im);
}

static nsq_cplx
nsq_mul(nsq_cplx a, nsq_cplx b) {
    return nsq_c((a.re * b.re) - (a.im * b.im), (a.re * b.im) + (a.im * b.re));
}

static nsq_cplx
nsq_scale(nsq_cplx a, double s) {
    return nsq_c(a.re * s, a.im * s);
}

static nsq_cplx
nsq_div(nsq_cplx a, nsq_cplx b) {
    const double d = (b.re * b.re) + (b.im * b.im);
    return nsq_c(((a.re * b.re) + (a.im * b.im)) / d, ((a.im * b.re) - (a.re * b.im)) / d);
}

static double
nsq_abs(nsq_cplx a) {
    return hypot(a.re, a.im);
}

/* The principal square root. */
static nsq_cplx
nsq_sqrt(nsq_cplx a) {
    const double r = nsq_abs(a);
    const double re = sqrt(0.5 * (r + a.re));
    const double im = sqrt(0.5 * (r - a.re));
    return nsq_c(re, a.im < 0.0 ? -im : im);
}

/* ---------------------------------------------------------------------------------------------- the band */

/* The first 10 Hz step under @p limit_hz at which @p taps, run at @p sample_rate, fall 1 dB under their DC gain;
   @p limit_hz when they never do. */
static double
nsq_edge_hz(const float* taps, int taps_len, double sample_rate, double limit_hz) {
    if (!taps || taps_len <= 0 || !(sample_rate > 0.0)) {
        return limit_hz;
    }
    double dc = 0.0;
    for (int n = 0; n < taps_len; n++) {
        dc += (double)taps[n];
    }
    if (!(fabs(dc) > 0.0)) {
        return limit_hz;
    }
    const double limit = k_edge_ratio * fabs(dc);
    for (int step = 0;; step++) {
        const double f = (double)step * k_edge_step_hz;
        if (f >= limit_hz) {
            break;
        }
        const double w = 2.0 * M_PI * f / sample_rate;
        double re = 0.0;
        double im = 0.0;
        for (int n = 0; n < taps_len; n++) {
            re += (double)taps[n] * cos(w * (double)n);
            im -= (double)taps[n] * sin(w * (double)n);
        }
        if (hypot(re, im) < limit) {
            return f;
        }
    }
    return limit_hz;
}

double
dsd_noise_squelch_passband_edge_hz(const float* taps, int taps_len, int rate_hz) {
    return rate_hz > 0 ? nsq_edge_hz(taps, taps_len, (double)rate_hz, 0.5 * (double)rate_hz) : 0.0;
}

/* The four biquads of a Butterworth band-pass of prototype order 4 over [f1, f2] at fs: the prototype's poles moved to
   the band (low-pass to band-pass on pre-warped edges), then the bilinear transform; each biquad takes one conjugate
   pole pair, a zero at z = 1 and one at z = -1, and unit gain at the band's centre. */
static void
nsq_design_band_pass(double f1, double f2, double fs, dsd_noise_squelch_biquad* out) {
    const double w1 = 2.0 * fs * tan(M_PI * f1 / fs);
    const double w2 = 2.0 * fs * tan(M_PI * f2 / fs);
    const double bw = w2 - w1;
    const double w0sq = w1 * w2;
    const double centre = 2.0 * atan(sqrt(w0sq) / (2.0 * fs));
    const nsq_cplx e1 = nsq_c(cos(-centre), sin(-centre));
    const nsq_cplx e2 = nsq_mul(e1, e1);
    const nsq_cplx two_fs = nsq_c(2.0 * fs, 0.0);
    int made = 0;
    for (int k = 0; k < DSD_NOISE_SQUELCH_SECTIONS / 2; k++) {
        /* The order-4 prototype's upper-half-plane poles, at pi (2k + 1) / 8; their conjugates give the conjugate
           band-pass poles. */
        const double theta = M_PI * (double)(2 * k + 1) / (double)(2 * DSD_NOISE_SQUELCH_SECTIONS);
        const nsq_cplx p = nsq_c(-sin(theta), cos(theta));
        const nsq_cplx half = nsq_scale(p, 0.5 * bw);
        const nsq_cplx root = nsq_sqrt(nsq_sub(nsq_mul(half, half), nsq_c(w0sq, 0.0)));
        const nsq_cplx s_pair[2] = {nsq_add(half, root), nsq_sub(half, root)};
        for (int j = 0; j < 2; j++) {
            const nsq_cplx z = nsq_div(nsq_add(two_fs, s_pair[j]), nsq_sub(two_fs, s_pair[j]));
            const double a1 = -2.0 * z.re;
            const double a2 = (z.re * z.re) + (z.im * z.im);
            const nsq_cplx num = nsq_sub(nsq_c(1.0, 0.0), e2);
            const nsq_cplx den = nsq_add(nsq_add(nsq_c(1.0, 0.0), nsq_scale(e1, a1)), nsq_scale(e2, a2));
            out[made].gain = 1.0 / nsq_abs(nsq_div(num, den));
            out[made].a1 = a1;
            out[made].a2 = a2;
            made++;
        }
    }
}

/* ---------------------------------------------------------------------------------------------- calibration */

typedef struct {
    uint64_t state;
    int have_spare;
    double spare;
} nsq_rng;

static uint64_t
nsq_rng_next(nsq_rng* r) {
    uint64_t x = r->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* A standard normal sample (Box-Muller, both values used). */
static double
nsq_rng_normal(nsq_rng* r) {
    if (r->have_spare) {
        r->have_spare = 0;
        return r->spare;
    }
    const double u1 = ((double)(nsq_rng_next(r) >> 11) + 1.0) * (1.0 / 9007199254740992.0);
    const double u2 = (double)(nsq_rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
    const double mag = sqrt(-2.0 * log(u1));
    r->spare = mag * sin(2.0 * M_PI * u2);
    r->have_spare = 1;
    return mag * cos(2.0 * M_PI * u2);
}

/* Unit-variance complex Gaussian noise. */
static nsq_cplx
nsq_rng_complex(nsq_rng* r) {
    const double s = 0.70710678118654752;
    const double re = nsq_rng_normal(r) * s;
    const double im = nsq_rng_normal(r) * s;
    return nsq_c(re, im);
}

static double
nsq_biquad_run(const dsd_noise_squelch_biquad* q, double* s1, double* s2, double x) {
    const double y = (q->gain * x) + *s1;
    *s1 = *s2 - (q->a1 * y);
    *s2 = (-q->gain * x) - (q->a2 * y);
    return y;
}

/* One discriminator output's pass through sub-band k's biquads. */
static double
nsq_sub_band_run(const dsd_noise_squelch_plan* plan, double* s1, double* s2, int k, double x) {
    double y = x;
    for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
        y = nsq_biquad_run(&plan->section[k][s], &s1[s], &s2[s], y);
    }
    return y;
}

/* The calibration noise as the channel filter puts it out: white complex noise at twice the rate through the plan's
   cascade FIR (the stages ahead of the last) and the last half-band stage (one output per two inputs), then the
   channel taps. Each ring holds its newest sample at pos - 1. */
typedef struct {
    nsq_rng rng;
    const double* pre;
    int pre_len;
    const float* hb;
    int hb_len;
    const float* taps;
    int taps_len;
    nsq_cplx pre_ring[DSD_NOISE_SQUELCH_CASCADE_TAPS];
    int pre_pos;
    nsq_cplx hb_ring[NSQ_HB_MAX];
    int hb_pos;
    nsq_cplx ch_ring[DSD_NOISE_SQUELCH_MAX_TAPS];
    int ch_pos;
} nsq_noise_source;

static nsq_cplx
nsq_ring_fir(const nsq_cplx* ring, int pos, const float* h, int len) {
    nsq_cplx acc = nsq_c(0.0, 0.0);
    for (int j = 0; j < len; j++) {
        int at = pos - 1 - j;
        if (at < 0) {
            at += len;
        }
        acc = nsq_add(acc, nsq_scale(ring[at], (double)h[j]));
    }
    return acc;
}

static nsq_cplx
nsq_ring_fir_d(const nsq_cplx* ring, int pos, const double* h, int len) {
    nsq_cplx acc = nsq_c(0.0, 0.0);
    for (int j = 0; j < len; j++) {
        int at = pos - 1 - j;
        if (at < 0) {
            at += len;
        }
        acc = nsq_add(acc, nsq_scale(ring[at], h[j]));
    }
    return acc;
}

/* One noise sample at twice the rate, through the cascade FIR when the plan has one. */
static nsq_cplx
nsq_noise_wide(nsq_noise_source* src) {
    const nsq_cplx x = nsq_rng_complex(&src->rng);
    if (src->pre_len <= 0) {
        return x;
    }
    src->pre_ring[src->pre_pos] = x;
    src->pre_pos = (src->pre_pos + 1) % src->pre_len;
    return nsq_ring_fir_d(src->pre_ring, src->pre_pos, src->pre, src->pre_len);
}

static nsq_cplx
nsq_noise_next(nsq_noise_source* src) {
    nsq_cplx in;
    if (src->hb_len > 0) {
        for (int k = 0; k < 2; k++) {
            src->hb_ring[src->hb_pos] = nsq_noise_wide(src);
            src->hb_pos = (src->hb_pos + 1) % src->hb_len;
        }
        in = nsq_ring_fir(src->hb_ring, src->hb_pos, src->hb, src->hb_len);
    } else {
        in = nsq_rng_complex(&src->rng);
    }
    src->ch_ring[src->ch_pos] = in;
    src->ch_pos = (src->ch_pos + 1) % src->taps_len;
    return nsq_ring_fir(src->ch_ring, src->ch_pos, src->taps, src->taps_len);
}

/* Each band-pass's mean output power for noise alone: 2 s through the cascade FIR and the last half-band stage, the
   channel taps, the discriminator and the band-pass, once the filters have filled and settled. */
static void
nsq_calibrate(dsd_noise_squelch_plan* plan, const float* taps, int taps_len, const float* hb, int hb_len) {
    static const float k_unit_tap[1] = {1.0f};
    nsq_noise_source src;
    DSD_MEMSET(&src, 0, sizeof(src));
    src.rng.state = 0x9E3779B97F4A7C15ULL;
    src.hb = hb;
    src.hb_len = hb ? hb_len : 0;
    src.pre = plan->cascade;
    src.pre_len = src.hb_len > 0 ? plan->cascade_len : 0;
    src.taps = taps ? taps : k_unit_tap;
    src.taps_len = taps ? taps_len : 1;
    const int fill = src.taps_len + src.hb_len + src.pre_len;
    const int settle = (int)(k_settle_s * (double)plan->rate_hz);
    const int measured = (int)(k_calibration_s * (double)plan->rate_hz);
    double s1[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double s2[DSD_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double energy[DSD_NOISE_SQUELCH_MAX_BANDS];
    DSD_MEMSET(s1, 0, sizeof(s1));
    DSD_MEMSET(s2, 0, sizeof(s2));
    DSD_MEMSET(energy, 0, sizeof(energy));
    nsq_cplx prev = nsq_c(0.0, 0.0);
    for (int n = 0; n < fill + settle + measured; n++) {
        const nsq_cplx z = nsq_noise_next(&src);
        const nsq_cplx step = nsq_mul(z, nsq_c(prev.re, -prev.im));
        prev = z;
        if (n < fill) {
            continue;
        }
        const double x = atan2(step.im, step.re);
        for (int k = 0; k < plan->bands; k++) {
            const double y = nsq_sub_band_run(plan, s1[k], s2[k], k, x);
            if (n >= fill + settle) {
                energy[k] += y * y;
            }
        }
    }
    plan->p_ref_sum = 0.0;
    for (int k = 0; k < plan->bands; k++) {
        plan->p_ref[k] = energy[k] / (double)measured;
        if (k < plan->sub_bands) {
            plan->p_ref_sum += plan->p_ref[k];
        }
    }
}

/* |H(f)| of a stage's (symmetric) taps run at @p rate, against their DC gain. */
static double
nsq_stage_gain(const dsd_noise_squelch_stage* stage, double f, double rate) {
    const double centre = 0.5 * (double)(stage->len - 1);
    double dc = 0.0;
    double re = 0.0;
    for (int n = 0; n < stage->len; n++) {
        dc += (double)stage->taps[n];
        re += (double)stage->taps[n] * cos(2.0 * M_PI * f * ((double)n - centre) / rate);
    }
    return fabs(dc) > 0.0 ? fabs(re) / fabs(dc) : 0.0;
}

/* The stages ahead of the last as one linear-phase FIR at twice the rate (tools/noise_squelch_model.py
   cascade_fir()): DSD_NOISE_SQUELCH_CASCADE_TAPS taps sampled from their composite response on 0..fs (type I
   frequency sampling), unit DC gain. Stage i (top first) runs at fs * 2^(count - i). */
static void
nsq_design_cascade(dsd_noise_squelch_plan* plan, const dsd_noise_squelch_stage* stages, int count) {
    plan->cascade_len = 0;
    if (count < 2) {
        return;
    }

    enum { N = DSD_NOISE_SQUELCH_CASCADE_TAPS, HALF = (DSD_NOISE_SQUELCH_CASCADE_TAPS - 1) / 2 };

    const double fs = (double)plan->rate_hz;
    double g[HALF + 1];
    g[0] = 1.0;
    for (int k = 1; k <= HALF; k++) {
        const double f = (double)k * 2.0 * fs / (double)N;
        double v = 1.0;
        for (int i = 0; i + 1 < count; i++) {
            v *= nsq_stage_gain(&stages[i], f, fs * pow(2.0, (double)(count - i)));
        }
        g[k] = v;
    }
    double sum = 0.0;
    for (int n = 0; n < N; n++) {
        double h = g[0];
        for (int k = 1; k <= HALF; k++) {
            h += 2.0 * g[k] * cos(2.0 * M_PI * (double)k * (double)(n - HALF) / (double)N);
        }
        plan->cascade[n] = h;
        sum += h;
    }
    for (int n = 0; n < N; n++) {
        plan->cascade[n] /= sum;
    }
    plan->cascade_len = N;
}

/* Whether the arguments describe a plan the squelch can take: a rate it can window, taps it can hold, and at most
   DSD_NOISE_SQUELCH_MAX_STAGES stages with taps it can ring. */
static int
nsq_design_args_ok(int taps_len, const dsd_noise_squelch_stage* stages, int stage_count, int rate_hz) {
    if (rate_hz < 2 * NSQ_HALVES_PER_S || taps_len > DSD_NOISE_SQUELCH_MAX_TAPS
        || stage_count > DSD_NOISE_SQUELCH_MAX_STAGES) {
        return 0;
    }
    for (int i = 0; i < stage_count; i++) {
        if (!stages[i].taps || stages[i].len <= 0 || stages[i].len > NSQ_HB_MAX) {
            return 0;
        }
    }
    return 1;
}

/* The band for the channel taps (NULL: no channel filter) behind @p stage_count half-band stages at @p rate_hz: the
   rate, edge, band edges and band-pass counts into @p out; -1 when no band fits. */
static int
nsq_design_band(dsd_noise_squelch_plan* out, const float* taps, int taps_len, int stage_count, int rate_hz) {
    const double fs = (double)rate_hz;
    if (!taps && stage_count > 0 && 0.5 * fs <= k_unfiltered_signal_edge_hz) {
        return -1;
    }
    const double edge = dsd_noise_squelch_passband_edge_hz(taps, taps_len, rate_hz);
    double hi = edge - k_edge_guard_hz;
    if (hi > k_nyquist_fraction * fs) {
        hi = k_nyquist_fraction * fs;
    }
    const double width = hi - k_band_lo_hz;
    if (width < k_min_band_hz) {
        return -1;
    }
    int k_count = (int)floor(width / k_sub_band_hz);
    if (k_count < 1) {
        k_count = 1;
    } else if (k_count > DSD_NOISE_SQUELCH_MAX_SUB_BANDS) {
        k_count = DSD_NOISE_SQUELCH_MAX_SUB_BANDS;
    }
    out->rate_hz = rate_hz;
    out->edge_hz = edge;
    out->lo_hz = k_band_lo_hz;
    out->hi_hz = hi;
    out->sub_bands = k_count;
    out->bands = (2 * k_count) - 1;
    return 0;
}

/* The K sub-bands' band-passes, then the staggered set's: each a sub-band shifted by half its width, so a line on a
   boundary of the first set sits inside one of these. */
static void
nsq_design_band_passes(dsd_noise_squelch_plan* out) {
    const double fs = (double)out->rate_hz;
    const int k_count = out->sub_bands;
    const double step = (out->hi_hz - out->lo_hz) / (double)k_count;
    for (int k = 0; k < k_count; k++) {
        const double f1 = out->lo_hz + (step * (double)k);
        nsq_design_band_pass(f1, f1 + step, fs, out->section[k]);
    }
    for (int k = 0; k + 1 < k_count; k++) {
        const double f1 = out->lo_hz + (step * ((double)k + 0.5));
        nsq_design_band_pass(f1, f1 + step, fs, out->section[k_count + k]);
    }
}

int
dsd_noise_squelch_plan_design(dsd_noise_squelch_plan* out, const float* taps, int taps_len,
                              const dsd_noise_squelch_stage* stages, int stage_count, int rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!stages || stage_count <= 0) {
        stages = NULL;
        stage_count = 0;
    }
    if (!nsq_design_args_ok(taps_len, stages, stage_count, rate_hz)) {
        return -1;
    }
    if (!taps || taps_len <= 0) {
        taps = NULL;
        taps_len = 0;
    }
    if (nsq_design_band(out, taps, taps_len, stage_count, rate_hz) != 0) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return -1;
    }
    nsq_design_band_passes(out);
    nsq_design_cascade(out, stages, stage_count);
    const dsd_noise_squelch_stage* last = stage_count > 0 ? &stages[stage_count - 1] : NULL;
    nsq_calibrate(out, taps, taps_len, last ? last->taps : NULL, last ? last->len : 0);
    if (!(out->p_ref_sum > k_min_power)) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return -1;
    }
    out->valid = 1;
    return 0;
}

static int
nsq_same(double a, double b) {
    const double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return fabs(a - b) <= k_plan_same * (scale > 0.0 ? scale : 1.0);
}

int
dsd_noise_squelch_plan_equal(const dsd_noise_squelch_plan* a, const dsd_noise_squelch_plan* b) {
    if (!a || !b || a->valid != b->valid) {
        return 0;
    }
    if (!a->valid) {
        return 1;
    }
    if (a->rate_hz != b->rate_hz || a->sub_bands != b->sub_bands || a->bands != b->bands
        || !nsq_same(a->lo_hz, b->lo_hz) || !nsq_same(a->hi_hz, b->hi_hz)) {
        return 0;
    }
    for (int k = 0; k < a->bands; k++) {
        if (!nsq_same(a->p_ref[k], b->p_ref[k])) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------------------------- the gate */

/* The sample count at which half-window @p index (from 0) ends. */
static uint64_t
nsq_half_end(const dsd_noise_squelch* t, uint64_t index) {
    return ((index + 1U) * (uint64_t)t->plan.rate_hz) / (uint64_t)NSQ_HALVES_PER_S;
}

void
dsd_noise_squelch_reset(dsd_noise_squelch* t) {
    if (!t) {
        return;
    }
    DSD_MEMSET(t->s1, 0, sizeof(t->s1));
    DSD_MEMSET(t->s2, 0, sizeof(t->s2));
    DSD_MEMSET(t->half_e, 0, sizeof(t->half_e));
    DSD_MEMSET(t->prev_e, 0, sizeof(t->prev_e));
    t->half_iq = t->prev_iq = 0.0;
    t->half_n = t->prev_n = 0.0;
    t->have_prev = 0;
    t->samples = 0U;
    t->half_index = 0U;
    t->half_end = t->plan.valid ? nsq_half_end(t, 0U) : 0U;
    t->gate_open = 0;
    t->quieting_db = 0.0;
    t->windows = 0U;
}

void
dsd_noise_squelch_set_plan(dsd_noise_squelch* t, const dsd_noise_squelch_plan* plan) {
    if (!t || !plan) {
        return;
    }
    if (dsd_noise_squelch_plan_equal(&t->plan, plan)) {
        return;
    }
    t->plan = *plan;
    dsd_noise_squelch_reset(t);
}

void
dsd_noise_squelch_set_threshold(dsd_noise_squelch* t, int threshold_db) {
    if (!t) {
        return;
    }
    if (threshold_db < DSD_SQUELCH_MARGIN_MIN_DB) {
        threshold_db = DSD_SQUELCH_MARGIN_MIN_DB;
    } else if (threshold_db > DSD_SQUELCH_MARGIN_MAX_DB) {
        threshold_db = DSD_SQUELCH_MARGIN_MAX_DB;
    }
    t->threshold_db = threshold_db;
    t->open_db = (double)threshold_db;
    const double close = (double)threshold_db - k_hysteresis_db;
    t->close_db = close > k_close_floor_db ? close : k_close_floor_db;
}

static double
nsq_ratio_db(double ref, double power) {
    if (!(power > k_min_power)) {
        return k_q_cap_db;
    }
    const double q = 10.0 * log10(ref / power);
    if (q > k_q_cap_db) {
        return k_q_cap_db;
    }
    return q < -k_q_cap_db ? -k_q_cap_db : q;
}

double
dsd_noise_squelch_quieting_db(const dsd_noise_squelch_plan* plan, const double* p, double iq_power) {
    if (!plan || !plan->valid || !p || !(iq_power > k_min_power)) {
        return 0.0;
    }
    double sum = 0.0;
    double best = -k_q_cap_db;
    for (int k = 0; k < plan->bands; k++) {
        if (k < plan->sub_bands) {
            sum += p[k];
        }
        const double q = nsq_ratio_db(plan->p_ref[k], p[k]);
        if (q > best) {
            best = q;
        }
    }
    const double q_sum = nsq_ratio_db(plan->p_ref_sum, sum);
    const double guarded = best - k_guard_db;
    return guarded > q_sum ? guarded : q_sum;
}

static void
nsq_flush_state(dsd_noise_squelch* t) {
    for (int k = 0; k < t->plan.bands; k++) {
        for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
            if (fabs(t->s1[k][s]) < k_state_flush) {
                t->s1[k][s] = 0.0;
            }
            if (fabs(t->s2[k][s]) < k_state_flush) {
                t->s2[k][s] = 0.0;
            }
        }
    }
}

/* A half-window ends: with the one before it, it makes a window whose quieting moves the gate. */
static void
nsq_close_half(dsd_noise_squelch* t) {
    if (t->have_prev) {
        const double n = t->prev_n + t->half_n;
        double p[DSD_NOISE_SQUELCH_MAX_BANDS];
        for (int k = 0; k < t->plan.bands; k++) {
            p[k] = (t->prev_e[k] + t->half_e[k]) / n;
        }
        const double q = dsd_noise_squelch_quieting_db(&t->plan, p, (t->prev_iq + t->half_iq) / n);
        t->quieting_db = q;
        t->windows++;
        if (t->gate_open) {
            if (q < t->close_db) {
                t->gate_open = 0;
            }
        } else if (q >= t->open_db) {
            t->gate_open = 1;
        }
    }
    DSD_MEMCPY(t->prev_e, t->half_e, sizeof(t->prev_e));
    DSD_MEMSET(t->half_e, 0, sizeof(t->half_e));
    t->prev_iq = t->half_iq;
    t->prev_n = t->half_n;
    t->half_iq = 0.0;
    t->half_n = 0.0;
    t->have_prev = 1;
    nsq_flush_state(t);
}

void
dsd_noise_squelch_process(dsd_noise_squelch* t, const float* disc, const float* iq, int count, uint8_t* flags) {
    if (!t || !disc || !iq || count <= 0) {
        return;
    }
    if (!t->plan.valid) {
        if (flags) {
            DSD_MEMSET(flags, 0, (size_t)count);
        }
        return;
    }
    for (int i = 0; i < count; i++) {
        if (flags) {
            flags[i] = t->gate_open ? 0U : (uint8_t)DSD_SQUELCH_FLAG_CLOSED;
        }
        const double x = (double)disc[i];
        for (int k = 0; k < t->plan.bands; k++) {
            const double y = nsq_sub_band_run(&t->plan, t->s1[k], t->s2[k], k, x);
            t->half_e[k] += y * y;
        }
        const size_t at = (size_t)i * 2U;
        t->half_iq += ((double)iq[at] * (double)iq[at]) + ((double)iq[at + 1U] * (double)iq[at + 1U]);
        t->half_n += 1.0;
        t->samples++;
        if (t->samples >= t->half_end) {
            nsq_close_half(t);
            t->half_index++;
            t->half_end = nsq_half_end(t, t->half_index);
        }
    }
}

void
dsd_noise_squelch_get_status(const dsd_noise_squelch* t, dsd_noise_squelch_status* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!t) {
        return;
    }
    out->gate_open = t->plan.valid ? t->gate_open : 1;
    out->quieting_db = t->quieting_db;
    out->windows = t->windows;
}
