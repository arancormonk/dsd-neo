// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Timing Error Detector (TED) implementation: Gardner TED and fractional
 * delay timing correction for symbol synchronization in digital demodulation modes.
 */

#include <dsd-neo/dsp/ted.h>

/**
 * @brief Initialize TED state with default values.
 *
 * @param state TED state to initialize.
 */
void
ted_init_state(ted_state_t* state) {
    state->mu_q20 = 0;
    state->e_ema = 0;
}

/**
 * @brief Lightweight Gardner timing correction.
 *
 * Uses linear interpolation between adjacent complex samples around the
 * nominal samples-per-symbol to reduce timing error; intended for digital
 * modes when enabled.
 *
 * @param config TED configuration.
 * @param state  TED state (updates mu_q20).
 * @param x      Input/output I/Q buffer (modified in-place if timing adjustment applied).
 * @param N      Length of buffer (must be even; updated if timing adjustment applied).
 * @param y      Work buffer for timing-adjusted I/Q (must be at least size N).
 * @note Skips processing when samples-per-symbol is large unless forced.
 */
/* Cubic Farrow (Catmull-Rom) fractional-delay interpolator for one component. */
static inline int32_t
farrow_cubic_eval(int32_t s_m1, int32_t s0, int32_t s1, int32_t s2, int u_q15) {
    /* Coefficients for cubic convolution (a = -0.5, Catmull-Rom):
       p(u) = c0 + c1*u + c2*u^2 + c3*u^3, u in [0,1]. */
    int32_t c0 = s0;
    int32_t c1 = (s1 - s_m1) >> 1; /* 0.5*(s1 - s-1) */
    /* c2 = s-1 - 2.5*s0 + 2*s1 - 0.5*s2 */
    int32_t c2 = s_m1 - ((5 * s0) >> 1) + (s1 << 1) - (s2 >> 1);
    /* c3 = 0.5*(s2 - s-1) + 1.5*(s0 - s1) */
    int32_t c3 = ((s2 - s_m1) >> 1) + ((3 * (s0 - s1)) >> 1);

    /* Horner evaluation with fixed-point u (Q15). */
    int32_t u = u_q15;
    int32_t u2 = (int32_t)(((int64_t)u * u) >> 15);
    int32_t u3 = (int32_t)(((int64_t)u2 * u) >> 15);

    int64_t acc_q15 = ((int64_t)c3 * u3) + ((int64_t)c2 * u2) + ((int64_t)c1 * u) + (((int64_t)c0) << 15);
    /* Round and shift back to Q0 */
    int32_t y = (int32_t)((acc_q15 + (1 << 14)) >> 15);
    return y;
}

void
gardner_timing_adjust(const ted_config_t* config, ted_state_t* state, int16_t* x, int* N, int16_t* y) {
    if (!config || !state || !x || !N || !y) {
        return;
    }
    if (!config->enabled || config->sps <= 1) {
        return;
    }

    /* Guard: run TED only when we're near symbol rate to keep CPU low.
       Skip when samples-per-symbol is very high unless explicitly forced. */
    const int sps = config->sps;
    if (sps > 12 && !config->force) {
        return;
    }

    int mu = state->mu_q20;            /* Q20 fractional phase [0,1) */
    const int gain = config->gain_q20; /* small integer gain */
    const int buf_len = *N;            /* interleaved I/Q length */
    const int one = (1 << 20);
    const int mu_nom = one / (sps > 0 ? sps : 1); /* Q20 increment per complex sample */

    if (buf_len < 6) {
        return;
    }

    const int nc = buf_len >> 1; /* complex sample count */
    const int half = sps >> 1;   /* half symbol in complex samples */

    int out_n = 0; /* interleaved I/Q index for output */

    for (int n_c = 0; n_c + 1 < nc; n_c++) {
        /* Base complex sample index */
        int a_c = n_c;
        int a = a_c << 1;

        /* Interpolation fraction from mu: convert Q20 -> Q15 for Farrow */
        int frac_q20 = mu & (one - 1); /* 0..one-1 */
        int u_q15 = frac_q20 >> 5;     /* Q15 in [0,1) */

        /* Cubic Farrow interpolation at mid position using support [a_c-1..a_c+2] */
        int am1_c = a_c - 1;
        int ap1_c = a_c + 1;
        int ap2_c = a_c + 2;
        if (am1_c < 0) {
            am1_c = 0;
        }
        if (ap1_c >= nc) {
            ap1_c = nc - 1;
        }
        if (ap2_c >= nc) {
            ap2_c = nc - 1;
        }
        int am1 = am1_c << 1;
        int ap1 = ap1_c << 1;
        int ap2 = ap2_c << 1;

        int32_t yr = farrow_cubic_eval((int32_t)x[am1], (int32_t)x[a], (int32_t)x[ap1], (int32_t)x[ap2], u_q15);
        int32_t yj =
            farrow_cubic_eval((int32_t)x[am1 + 1], (int32_t)x[a + 1], (int32_t)x[ap1 + 1], (int32_t)x[ap2 + 1], u_q15);
        y[out_n++] = (int16_t)yr;
        y[out_n++] = (int16_t)yj;

        /* Sample at ±T/2 around current mid-sample using same frac (Farrow).
           Use clamped indices to avoid boundary overruns. */
        int l_c = a_c - half;
        int r_c = a_c + half;
        if (l_c < 0) {
            l_c = 0;
        }
        if (l_c >= nc - 1) {
            l_c = nc - 2;
        }
        if (r_c < 0) {
            r_c = 0;
        }
        if (r_c >= nc - 1) {
            r_c = nc - 2;
        }

        int lm1_c = l_c - 1;
        int lp1_c = l_c + 1;
        int lp2_c = l_c + 2;
        if (lm1_c < 0) {
            lm1_c = 0;
        }
        if (lp1_c >= nc) {
            lp1_c = nc - 1;
        }
        if (lp2_c >= nc) {
            lp2_c = nc - 1;
        }
        int rm1_c = r_c - 1;
        int rp1_c = r_c + 1;
        int rp2_c = r_c + 2;
        if (rm1_c < 0) {
            rm1_c = 0;
        }
        if (rp1_c >= nc) {
            rp1_c = nc - 1;
        }
        if (rp2_c >= nc) {
            rp2_c = nc - 1;
        }

        int l0 = l_c << 1;
        int lm1 = lm1_c << 1;
        int lp1 = lp1_c << 1;
        int lp2 = lp2_c << 1;
        int r0 = r_c << 1;
        int rm1 = rm1_c << 1;
        int rp1 = rp1_c << 1;
        int rp2 = rp2_c << 1;

        int32_t lr = farrow_cubic_eval((int32_t)x[lm1], (int32_t)x[l0], (int32_t)x[lp1], (int32_t)x[lp2], u_q15);
        int32_t lj =
            farrow_cubic_eval((int32_t)x[lm1 + 1], (int32_t)x[l0 + 1], (int32_t)x[lp1 + 1], (int32_t)x[lp2 + 1], u_q15);
        int32_t rr = farrow_cubic_eval((int32_t)x[rm1], (int32_t)x[r0], (int32_t)x[rp1], (int32_t)x[rp2], u_q15);
        int32_t rj =
            farrow_cubic_eval((int32_t)x[rm1 + 1], (int32_t)x[r0 + 1], (int32_t)x[rp1 + 1], (int32_t)x[rp2 + 1], u_q15);

        /* Gardner error: Re{ (x(+T/2) - x(-T/2)) * conj(y_mid) } */
        int32_t dr = (int32_t)(rr - lr);
        int32_t dj = (int32_t)(rj - lj);
        int64_t e = (int64_t)dr * (int64_t)yr + (int64_t)dj * (int64_t)yj; /* Q0 */

        /* Normalize by instantaneous power to keep scale stable. */
        int64_t p2 = (int64_t)yr * (int64_t)yr + (int64_t)yj * (int64_t)yj;
        if (p2 < 1) {
            p2 = 1;
        }
        /* e_norm in Q15 in range roughly [-1,1] */
        int32_t e_norm_q15 = (int32_t)((e << 15) / p2);
        if (e_norm_q15 > 32767) {
            e_norm_q15 = 32767;
        }
        if (e_norm_q15 < -32768) {
            e_norm_q15 = -32768;
        }

        /* Update fractional phase: nominal advance + small correction.
           Scale to Q20 with conservative shift to avoid runaway. */
        int32_t corr = (int32_t)(((int64_t)gain * (int64_t)e_norm_q15) >> 10); /* Q20 step units */

        /* Bound correction to avoid large jumps (<= ~1/2 nominal step). */
        if (corr > (mu_nom >> 1)) {
            corr = (mu_nom >> 1);
        }
        if (corr < -(mu_nom >> 1)) {
            corr = -(mu_nom >> 1);
        }
        mu += mu_nom + corr;

        /* Smooth residual using simple EMA with small weight (alpha≈1/64). */
        int ee = state->e_ema;
        int e_i32 = (int)e; /* compression to int32 is fine for EMA */
        ee += (int)((e_i32 - ee) >> 6);
        state->e_ema = ee;

        /* Wrap mu to [0, one) */
        if (mu >= one) {
            mu -= one;
        }
        if (mu < 0) {
            mu += one;
        }
    }

    if (out_n >= 2) {
        /* Copy timing-adjusted samples back to input buffer */
        for (int i = 0; i < out_n; i++) {
            x[i] = y[i];
        }
        *N = out_n;
    }

    state->mu_q20 = mu;
}
