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
#include <math.h>

/**
 * @brief Initialize TED state with default values.
 *
 * @param state TED state to initialize.
 */
void
ted_init_state(ted_state_t* state) {
    state->mu = 0.0f;
    state->e_ema = 0.0f;
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
static inline float
farrow_cubic_eval(float s_m1, float s0, float s1, float s2, float u) {
    /* Coefficients for cubic convolution (a = -0.5, Catmull-Rom):
       p(u) = c0 + c1*u + c2*u^2 + c3*u^3, u in [0,1]. */
    float c0 = s0;
    float c1 = 0.5f * (s1 - s_m1); /* 0.5*(s1 - s-1) */
    /* c2 = s-1 - 2.5*s0 + 2*s1 - 0.5*s2 */
    float c2 = s_m1 - 2.5f * s0 + 2.0f * s1 - 0.5f * s2;
    /* c3 = 0.5*(s2 - s-1) + 1.5*(s0 - s1) */
    float c3 = 0.5f * (s2 - s_m1) + 1.5f * (s0 - s1);

    float u2 = u * u;
    float u3 = u2 * u;

    return ((c3 * u3) + (c2 * u2) + (c1 * u) + c0);
}

void
gardner_timing_adjust(const ted_config_t* config, ted_state_t* state, float* x, int* N, float* y) {
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

    float mu = state->mu; /* fractional phase [0.0, 1.0) */
    const float gain = config->gain;
    const int buf_len = *N;                                 /* interleaved I/Q length */
    const float mu_nom = 1.0f / (float)(sps > 0 ? sps : 1); /* nominal advance per sample */

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

        /* Interpolation fraction from mu [0.0, 1.0) */
        float u = mu - floorf(mu); /* ensure [0,1) */

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

        float yr = farrow_cubic_eval(x[am1], x[a], x[ap1], x[ap2], u);
        float yj = farrow_cubic_eval(x[am1 + 1], x[a + 1], x[ap1 + 1], x[ap2 + 1], u);
        y[out_n++] = yr;
        y[out_n++] = yj;

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

        float lr = farrow_cubic_eval(x[lm1], x[l0], x[lp1], x[lp2], u);
        float lj = farrow_cubic_eval(x[lm1 + 1], x[l0 + 1], x[lp1 + 1], x[lp2 + 1], u);
        float rr = farrow_cubic_eval(x[rm1], x[r0], x[rp1], x[rp2], u);
        float rj = farrow_cubic_eval(x[rm1 + 1], x[r0 + 1], x[rp1 + 1], x[rp2 + 1], u);

        /* Gardner error: Re{ (x(+T/2) - x(-T/2)) * conj(y_mid) } */
        float dr = rr - lr;
        float dj = rj - lj;
        float e = dr * yr + dj * yj; /* instantaneous Gardner error */

        /* Normalize by instantaneous power to keep scale stable. */
        float p2 = yr * yr + yj * yj;
        if (p2 < 1e-9f) {
            p2 = 1e-9f;
        }
        float e_norm = e / p2;

        /* Update fractional phase: nominal advance + small correction */
        float corr = gain * e_norm;

        /* Bound correction to avoid large jumps (<= ~1/2 nominal step). */
        float max_corr = mu_nom * 0.5f;
        if (corr > max_corr) {
            corr = max_corr;
        }
        if (corr < -max_corr) {
            corr = -max_corr;
        }
        mu += mu_nom + corr;

        /* Smooth residual using simple EMA with small weight (alpha≈1/64). */
        const float kEmaAlpha = 1.0f / 64.0f;
        state->e_ema = state->e_ema + kEmaAlpha * (e_norm - state->e_ema);

        /* Wrap mu to [0, 1) */
        while (mu >= 1.0f) {
            mu -= 1.0f;
        }
        while (mu < 0.0f) {
            mu += 1.0f;
        }
    }

    if (out_n >= 2) {
        /* Copy timing-adjusted samples back to input buffer */
        for (int i = 0; i < out_n; i++) {
            x[i] = y[i];
        }
        *N = out_n;
    }

    state->mu = mu;
}
