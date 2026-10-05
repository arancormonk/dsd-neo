// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The noise squelches' band-pass bank and quieting; see noise_squelch_bank.h. */

#include "noise_squelch_bank.h"

#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double re;
    double im;
} nsb_cplx;

static nsb_cplx
nsb_c(double re, double im) {
    nsb_cplx z = {re, im};
    return z;
}

static nsb_cplx
nsb_add(nsb_cplx a, nsb_cplx b) {
    return nsb_c(a.re + b.re, a.im + b.im);
}

static nsb_cplx
nsb_sub(nsb_cplx a, nsb_cplx b) {
    return nsb_c(a.re - b.re, a.im - b.im);
}

static nsb_cplx
nsb_mul(nsb_cplx a, nsb_cplx b) {
    return nsb_c((a.re * b.re) - (a.im * b.im), (a.re * b.im) + (a.im * b.re));
}

static nsb_cplx
nsb_scale(nsb_cplx a, double s) {
    return nsb_c(a.re * s, a.im * s);
}

static nsb_cplx
nsb_div(nsb_cplx a, nsb_cplx b) {
    const double d = (b.re * b.re) + (b.im * b.im);
    return nsb_c(((a.re * b.re) + (a.im * b.im)) / d, ((a.im * b.re) - (a.re * b.im)) / d);
}

static double
nsb_abs(nsb_cplx a) {
    return hypot(a.re, a.im);
}

/* The principal square root. */
static nsb_cplx
nsb_sqrt(nsb_cplx a) {
    const double r = nsb_abs(a);
    const double re = sqrt(0.5 * (r + a.re));
    const double im = sqrt(0.5 * (r - a.re));
    return nsb_c(re, a.im < 0.0 ? -im : im);
}

/* The prototype's poles moved to the band (low-pass to band-pass on pre-warped edges), then the bilinear transform;
   each biquad takes one conjugate pole pair, a zero at z = 1 and one at z = -1, and unit gain at the band's centre. */
void
dsd_noise_squelch_bank_design_band_pass(double f1, double f2, double fs, dsd_noise_squelch_biquad* out) {
    if (!out) {
        return;
    }
    const double w1 = 2.0 * fs * tan(M_PI * f1 / fs);
    const double w2 = 2.0 * fs * tan(M_PI * f2 / fs);
    const double bw = w2 - w1;
    const double w0sq = w1 * w2;
    const double centre = 2.0 * atan(sqrt(w0sq) / (2.0 * fs));
    const nsb_cplx e1 = nsb_c(cos(-centre), sin(-centre));
    const nsb_cplx e2 = nsb_mul(e1, e1);
    const nsb_cplx two_fs = nsb_c(2.0 * fs, 0.0);
    int made = 0;
    for (int k = 0; k < DSD_NOISE_SQUELCH_SECTIONS / 2; k++) {
        /* The order-4 prototype's upper-half-plane poles, at pi (2k + 1) / 8; their conjugates give the conjugate
           band-pass poles. */
        const double theta = M_PI * (double)(2 * k + 1) / (double)(2 * DSD_NOISE_SQUELCH_SECTIONS);
        const nsb_cplx p = nsb_c(-sin(theta), cos(theta));
        const nsb_cplx half = nsb_scale(p, 0.5 * bw);
        const nsb_cplx root = nsb_sqrt(nsb_sub(nsb_mul(half, half), nsb_c(w0sq, 0.0)));
        const nsb_cplx s_pair[2] = {nsb_add(half, root), nsb_sub(half, root)};
        for (int j = 0; j < 2; j++) {
            const nsb_cplx z = nsb_div(nsb_add(two_fs, s_pair[j]), nsb_sub(two_fs, s_pair[j]));
            const double a1 = -2.0 * z.re;
            const double a2 = (z.re * z.re) + (z.im * z.im);
            const nsb_cplx num = nsb_sub(nsb_c(1.0, 0.0), e2);
            const nsb_cplx den = nsb_add(nsb_add(nsb_c(1.0, 0.0), nsb_scale(e1, a1)), nsb_scale(e2, a2));
            out[made].gain = 1.0 / nsb_abs(nsb_div(num, den));
            out[made].a1 = a1;
            out[made].a2 = a2;
            made++;
        }
    }
}

double
dsd_noise_squelch_bank_ratio_db(double ref, double power) {
    if (!(power > DSD_NOISE_SQUELCH_BANK_MIN_POWER)) {
        return DSD_NOISE_SQUELCH_BANK_Q_CAP_DB;
    }
    const double q = 10.0 * log10(ref / power);
    if (q > DSD_NOISE_SQUELCH_BANK_Q_CAP_DB) {
        return DSD_NOISE_SQUELCH_BANK_Q_CAP_DB;
    }
    return q < -DSD_NOISE_SQUELCH_BANK_Q_CAP_DB ? -DSD_NOISE_SQUELCH_BANK_Q_CAP_DB : q;
}

double
dsd_noise_squelch_bank_quieting_db(const double* ref, double ref_sum, const double* p, const unsigned char* part,
                                   int sub_bands, int bands, double guard_db) {
    if (!ref || !p) {
        return 0.0;
    }
    double sum = 0.0;
    double best = -DSD_NOISE_SQUELCH_BANK_Q_CAP_DB;
    int counted = 0;
    for (int k = 0; k < bands; k++) {
        if (part && !part[k]) {
            continue;
        }
        if (k < sub_bands) {
            sum += p[k];
            counted++;
        }
        const double q = dsd_noise_squelch_bank_ratio_db(ref[k], p[k]);
        if (q > best) {
            best = q;
        }
    }
    if (counted == 0) {
        return 0.0;
    }
    const double q_sum = dsd_noise_squelch_bank_ratio_db(ref_sum, sum);
    const double guarded = best - guard_db;
    return guarded > q_sum ? guarded : q_sum;
}
