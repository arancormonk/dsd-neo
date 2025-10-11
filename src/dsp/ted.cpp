// SPDX-License-Identifier: GPL-2.0-or-later
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
        /* Base and next complex sample indices */
        int a_c = n_c;
        int b_c = n_c + 1;

        int a = a_c << 1;
        int b = b_c << 1;

        /* Interpolation fraction from mu (Q20) */
        int frac = mu & (one - 1); /* 0..one-1 */
        int inv = one - frac;

        /* Linear interpolation y_mid between x[a_c] and x[b_c] (complex) */
        int32_t ar = x[a];
        int32_t aj = x[a + 1];
        int32_t br = x[b];
        int32_t bj = x[b + 1];
        int32_t yr = (int32_t)(((int64_t)inv * ar + (int64_t)frac * br) >> 20);
        int32_t yj = (int32_t)(((int64_t)inv * aj + (int64_t)frac * bj) >> 20);
        y[out_n++] = (int16_t)yr;
        y[out_n++] = (int16_t)yj;

        /* Sample at ±T/2 around current mid-sample using same frac.
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

        int l0 = l_c << 1;
        int l1 = l0 + 2;
        int r0 = r_c << 1;
        int r1 = r0 + 2;

        int32_t lr = (int32_t)(((int64_t)inv * x[l0] + (int64_t)frac * x[l1]) >> 20);
        int32_t lj = (int32_t)(((int64_t)inv * x[l0 + 1] + (int64_t)frac * x[l1 + 1]) >> 20);
        int32_t rr = (int32_t)(((int64_t)inv * x[r0] + (int64_t)frac * x[r1]) >> 20);
        int32_t rj = (int32_t)(((int64_t)inv * x[r0 + 1] + (int64_t)frac * x[r1 + 1]) >> 20);

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
