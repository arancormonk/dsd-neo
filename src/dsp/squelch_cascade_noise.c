// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The noise covariance through the half-band cascade and the channel taps (issue #899); see squelch_cascade_noise.h. */

#include "squelch_cascade_noise.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* E{z_a z_b} of @p r for any samples a and b: the row of the earlier one, at their distance; 0 at lags beyond. */
static double
scn_cov(const dsd_squelch_floor_periodic_noise* r, int a, int b) {
    int n = a;
    int m = b - a;
    if (m < 0) {
        n = b;
        m = -m;
    }
    if (m >= r->lags) {
        return 0.0;
    }
    int phase = n % r->period;
    if (phase < 0) {
        phase += r->period;
    }
    return r->cov[((size_t)phase * (size_t)r->lags) + (size_t)m];
}

/* A stage of @p len taps on noise of @p period phases and @p lags lags: the shape of its output. */
static void
scn_stage_shape(int period, int lags, int len, int* out_period, int* out_lags) {
    *out_period = (period % 2 == 0) ? period / 2 : period;
    *out_lags = ((lags - 1 + len - 1) / 2) + 1;
}

/* period x lags doubles, or 0 when that does not fit a size_t. */
static size_t
scn_size(int period, int lags) {
    if (period < 1 || lags < 1 || (size_t)lags > SIZE_MAX / (size_t)period) {
        return 0U;
    }
    return (size_t)period * (size_t)lags;
}

static int
scn_stages_ok(const dsd_noise_squelch_stage* stages, int count) {
    if (count < 0 || (count > 0 && !stages)) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!stages[i].taps || stages[i].len < 1 || stages[i].len > DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS) {
            return 0;
        }
    }
    return 1;
}

size_t
dsd_squelch_cascade_noise_work_len(int in_period, int in_lags, const dsd_noise_squelch_stage* stages, int count) {
    /* A stage's lags come from in_lags + len - 2 (scn_stage_shape()): held where that cannot overflow. */
    if (!scn_stages_ok(stages, count) || in_lags > INT_MAX - DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS) {
        return 0U;
    }
    size_t most = scn_size(in_period, in_lags);
    int period = in_period;
    int lags = in_lags;
    for (int i = 0; i < count && most > 0U; i++) {
        scn_stage_shape(period, lags, stages[i].len, &period, &lags);
        const size_t size = scn_size(period, lags);
        most = size == 0U ? 0U : (size > most ? size : most);
    }
    return most <= SIZE_MAX / 2U ? 2U * most : 0U;
}

/* The autocorrelation of @p len real taps at lags 0..len - 1 into @p out. */
static void
scn_autocorr(const float* taps, int len, double* out) {
    for (int j = 0; j < len; j++) {
        double sum = 0.0;
        for (int t = 0; t + j < len; t++) {
            sum += (double)taps[t] * (double)taps[t + j];
        }
        out[j] = sum;
    }
}

/* One stage on stationary noise: R'[m] = sum_j g[j] R[2m - j], g the taps' autocorrelation, into @p out. */
static void
scn_stage_stationary(const dsd_squelch_floor_periodic_noise* in, const dsd_noise_squelch_stage* stage, int out_lags,
                     double* out) {
    double g[DSD_SQUELCH_CASCADE_MAX_STAGE_TAPS];
    const int len = stage->len;
    scn_autocorr(stage->taps, len, g);
    for (int k = 0; k < out_lags; k++) {
        double sum = 0.0;
        for (int j = -(len - 1); j <= len - 1; j++) {
            const int at = abs((2 * k) - j);
            if (at < in->lags) {
                sum += g[abs(j)] * in->cov[at];
            }
        }
        out[k] = sum;
    }
}

/* One stage on periodic noise: R'(q, q + m) = sum_i sum_j h_i h_j R(2q - i, 2q + 2m - j), into @p out. */
static void
scn_stage_periodic(const dsd_squelch_floor_periodic_noise* in, const dsd_noise_squelch_stage* stage, int out_period,
                   int out_lags, double* out) {
    const float* h = stage->taps;
    const int len = stage->len;
    for (int q = 0; q < out_period; q++) {
        for (int m = 0; m < out_lags; m++) {
            double sum = 0.0;
            for (int i = 0; i < len; i++) {
                double row = 0.0;
                for (int j = 0; j < len; j++) {
                    row += (double)h[j] * scn_cov(in, (2 * q) - i, (2 * q) + (2 * m) - j);
                }
                sum += (double)h[i] * row;
            }
            out[((size_t)q * (size_t)out_lags) + (size_t)m] = sum;
        }
    }
}

int
dsd_squelch_cascade_noise(const dsd_squelch_floor_periodic_noise* in, const dsd_noise_squelch_stage* stages, int count,
                          double* work, size_t work_len, dsd_squelch_floor_periodic_noise* out) {
    if (!work || !out || (in && (!in->cov || in->period < 1 || in->lags < 1))) {
        return -1;
    }
    const int in_period = in ? in->period : 1;
    const int in_lags = in ? in->lags : 1;
    const size_t need = dsd_squelch_cascade_noise_work_len(in_period, in_lags, stages, count);
    if (need == 0U || work_len < need) {
        return -1;
    }
    /* The two halves of the workspace by turns: the input copied into the first, each stage into the other. */
    double* half[2] = {work, work + (need / 2U)};
    int at = 0;
    if (in) {
        DSD_MEMCPY(half[0], in->cov, scn_size(in_period, in_lags) * sizeof(double));
    } else {
        half[0][0] = count > 0 ? ldexp(1.0, count - 1) : 1.0;
    }
    dsd_squelch_floor_periodic_noise r;
    r.period = in_period;
    r.lags = in_lags;
    r.cov = half[0];
    for (int i = 0; i < count; i++) {
        int period = 0;
        int lags = 0;
        scn_stage_shape(r.period, r.lags, stages[i].len, &period, &lags);
        double* next = half[at ^ 1];
        if (r.period == 1) {
            scn_stage_stationary(&r, &stages[i], lags, next);
        } else {
            scn_stage_periodic(&r, &stages[i], period, lags, next);
        }
        at ^= 1;
        r.period = period;
        r.lags = lags;
        r.cov = next;
    }
    *out = r;
    return 0;
}

/* Stationary noise through the taps: R'[m] = sum_d R_c(d) R[m - d], R_c into rc. */
static void
scn_taps_stationary(const dsd_squelch_floor_periodic_noise* in, const float* taps, int len, double* rc, int lags,
                    double* out) {
    scn_autocorr(taps, len, rc);
    for (int lag = 0; lag < lags; lag++) {
        double sum = 0.0;
        const int lo = lag - (len - 1);
        const int hi = lag + (len - 1);
        for (int k = lo; k <= hi; k++) {
            const int at = abs(k);
            if (at < in->lags) {
                sum += in->cov[at] * rc[abs(lag - k)];
            }
        }
        out[lag] = sum;
    }
}

/* Periodic noise through the taps: R'(n, n + m) = sum_i sum_j c_i c_j R(n - i, n + m - j). */
static void
scn_taps_periodic(const dsd_squelch_floor_periodic_noise* in, const float* taps, int len, int lags, double* out) {
    for (int n = 0; n < in->period; n++) {
        for (int m = 0; m < lags; m++) {
            double sum = 0.0;
            for (int i = 0; i < len; i++) {
                double row = 0.0;
                for (int j = 0; j < len; j++) {
                    row += (double)taps[j] * scn_cov(in, n - i, n + m - j);
                }
                sum += (double)taps[i] * row;
            }
            out[((size_t)n * (size_t)lags) + (size_t)m] = sum;
        }
    }
}

/* The arguments dsd_squelch_cascade_noise_through_taps() takes: noise with a shape, taps, room for R_c when the
   noise is stationary, and an output lag count that fits an int. */
static int
scn_taps_args_ok(const dsd_squelch_floor_periodic_noise* in, const float* taps, int taps_len, int have_rc,
                 size_t rc_len) {
    if (!in || !in->cov || in->period < 1 || in->lags < 1 || !taps || taps_len < 1 || in->lags > INT_MAX - taps_len) {
        return 0;
    }
    return in->period > 1 || (have_rc && rc_len >= (size_t)taps_len);
}

int
dsd_squelch_cascade_noise_through_taps(const dsd_squelch_floor_periodic_noise* in, const float* taps, int taps_len,
                                       double* rc, size_t rc_len, double* out_cov, size_t out_len,
                                       dsd_squelch_floor_periodic_noise* out) {
    if (!scn_taps_args_ok(in, taps, taps_len, rc != NULL, rc_len) || !out_cov || !out) {
        return -1;
    }
    const int lags = in->lags + taps_len - 1;
    const size_t need = scn_size(in->period, lags);
    if (need == 0U || out_len < need) {
        return -1;
    }
    if (in->period == 1) {
        scn_taps_stationary(in, taps, taps_len, rc, lags, out_cov);
    } else {
        scn_taps_periodic(in, taps, taps_len, lags, out_cov);
    }
    out->period = in->period;
    out->lags = lags;
    out->cov = out_cov;
    return 0;
}

int
dsd_squelch_cascade_noise_floor_plan(dsd_squelch_floor_plan* out, const float* taps, int taps_len,
                                     const dsd_noise_squelch_stage* stages, int count, int rate_hz) {
    static const float k_unit_tap[1] = {1.0f};
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (taps_len > DSD_NOISE_SQUELCH_MAX_TAPS) {
        return -1;
    }
    const float* run = (taps && taps_len > 0) ? taps : k_unit_tap;
    const int run_len = (taps && taps_len > 0) ? taps_len : 1;
    double work[2 * DSD_SQUELCH_CASCADE_WHITE_LAGS];
    double rc[DSD_NOISE_SQUELCH_MAX_TAPS];
    double chan[DSD_SQUELCH_CASCADE_WHITE_LAGS + DSD_NOISE_SQUELCH_MAX_TAPS - 1];
    dsd_squelch_floor_periodic_noise hb;
    dsd_squelch_floor_periodic_noise noise;
    if (dsd_squelch_cascade_noise(NULL, stages, count, work, sizeof(work) / sizeof(work[0]), &hb) != 0
        || dsd_squelch_cascade_noise_through_taps(&hb, run, run_len, rc, sizeof(rc) / sizeof(rc[0]), chan,
                                                  sizeof(chan) / sizeof(chan[0]), &noise)
               != 0) {
        return -1;
    }
    return dsd_squelch_floor_plan_design_periodic_mean(out, taps, taps_len, &noise, rate_hz);
}

/* The bound's limit, relative to the output power. It reaches 1.1e-3 on the Airspy grid (72 kHz, NFM 25k), where the
   two plans differ by under 2e-8; an upsampling reader's phases stray by about 2 % and take every phase in full. */
static const double k_mean_bound = 1e-2;

int
dsd_squelch_cascade_mean_suffices(double cyclo_dev, const float* taps, int taps_len, double power) {
    if (!taps || taps_len < 1 || !(power > 0.0)) {
        return 0;
    }
    double abs_sum = 0.0;
    for (int d = 0; d < taps_len; d++) {
        abs_sum += fabs((double)taps[d]);
    }
    return cyclo_dev * abs_sum * abs_sum / power <= k_mean_bound;
}
