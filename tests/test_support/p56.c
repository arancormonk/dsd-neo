// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "p56.h"

#include <math.h>
#include <stddef.h>

enum { P56_THRESHOLDS = 15 };

#define P56_MARGIN_DB     15.9
#define P56_TIME_CONSTANT 0.03
#define P56_HANGOVER_S    0.2
#define P56_INT16_FULL_DB 90.30899869919435 /* 20 log10(32768) */

typedef struct {
    double sum_sq;
    size_t active[P56_THRESHOLDS];
} p56_counts;

/* One pass over the signal: the two smoothers, and for every threshold the samples that are active at it. A sample is
   active when the envelope reached the threshold at most `hang` samples ago, itself included. */
static void
p56_count(const float* x, size_t n, int rate_hz, p56_counts* out) {
    const double g = exp(-1.0 / ((double)rate_hz * P56_TIME_CONSTANT));
    const size_t hang = (size_t)lround(P56_HANGOVER_S * (double)rate_hz);
    size_t since[P56_THRESHOLDS];
    int seen[P56_THRESHOLDS];
    double p = 0.0;
    double q = 0.0;

    out->sum_sq = 0.0;
    for (int j = 0; j < P56_THRESHOLDS; j++) {
        out->active[j] = 0;
        since[j] = 0;
        seen[j] = 0;
    }
    for (size_t i = 0; i < n; i++) {
        const double v = (double)x[i];
        p = ((1.0 - g) * fabs(v)) + (g * p);
        q = ((1.0 - g) * p) + (g * q);
        out->sum_sq += v * v;
        double c = 1.0;
        for (int j = 0; j < P56_THRESHOLDS; j++, c *= 2.0) {
            if (q >= c) {
                since[j] = 0;
                seen[j] = 1;
            } else if (seen[j]) {
                since[j]++;
            }
            if (seen[j] && since[j] <= hang) {
                out->active[j]++;
            }
        }
    }
}

double
dsd_test_p56_level_dbfs(const float* x, size_t n, int rate_hz, double* activity_out) {
    if (activity_out != NULL) {
        *activity_out = NAN;
    }
    if ((x == NULL && n > 0) || rate_hz <= 0) {
        return NAN;
    }
    if (activity_out != NULL) {
        *activity_out = 0.0;
    }
    if (n == 0) {
        return -INFINITY;
    }
    p56_counts counts;
    p56_count(x, n, rate_hz, &counts);
    if (counts.active[0] == 0 || !(counts.sum_sq > 0.0)) {
        return -INFINITY;
    }

    /* Walk up the thresholds until the active level comes within the margin of one, then interpolate in dB between it
       and the threshold below: the level where the distance crosses the margin. */
    double prev_a = 0.0;
    double prev_d = 0.0;
    for (int j = 0; j < P56_THRESHOLDS && counts.active[j] > 0; j++) {
        const double a = 10.0 * log10(counts.sum_sq / (double)counts.active[j]);
        const double d = a - (20.0 * log10(ldexp(1.0, j)));
        if (d <= P56_MARGIN_DB) {
            /* Above the lowest threshold, the one below missed the margin: prev_d > margin >= d. */
            double level = a;
            if (j > 0) {
                level = prev_a + ((prev_d - P56_MARGIN_DB) / (prev_d - d) * (a - prev_a));
            }
            if (activity_out != NULL) {
                *activity_out = (counts.sum_sq / (double)n) / pow(10.0, level / 10.0);
            }
            return level - P56_INT16_FULL_DB;
        }
        prev_a = a;
        prev_d = d;
    }
    if (activity_out != NULL) {
        *activity_out = NAN;
    }
    return NAN;
}
