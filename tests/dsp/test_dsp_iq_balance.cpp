// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: mode-aware IQ balance reduces impropriety |E[z^2]| / E[|z|^2] on a QPSK-like sequence
   corrupted with a small conjugate (image) component. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp
int use_halfband_decimator = 0;
int fll_lut_enabled = 0;

static double
impropriety_ratio(const int16_t* x, int pairs) {
    double s2r = 0.0, s2i = 0.0, p2 = 0.0;
    for (int n = 0; n < pairs; n++) {
        double I = x[(size_t)(2 * n) + 0];
        double Q = x[(size_t)(2 * n) + 1];
        s2r += I * I - Q * Q;
        s2i += 2.0 * I * Q;
        p2 += I * I + Q * Q;
    }
    double num = sqrt(s2r * s2r + s2i * s2i);
    if (p2 < 1e-9) {
        p2 = 1e-9;
    }
    return num / p2;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    const int pairs = 512;
    static int16_t buf[(size_t)pairs * 2];
    // Generate QPSK-like random symbols
    int seed = 12345;
    for (int n = 0; n < pairs; n++) {
        seed = (1103515245 * seed + 12345);
        int bi = (seed >> 16) & 1;
        int bq = (seed >> 17) & 1;
        int16_t I = bi ? 8000 : -8000;
        int16_t Q = bq ? 8000 : -8000;
        // Inject small conjugate image: y = z + a*conj(z)
        // a ~ 0.1 -> (3277 in Q15), but do in float for simplicity
        double a = 0.10;
        double yI = I + a * I;
        double yQ = Q - a * Q; // conj(z) => (I, -Q)
        buf[(size_t)(2 * n) + 0] = (int16_t)lrint(yI);
        buf[(size_t)(2 * n) + 1] = (int16_t)lrint(yQ);
    }

    double pre = impropriety_ratio(buf, pairs);
    if (pre < 0.01) {
        fprintf(stderr, "IQBAL test: pre impropriety unexpectedly small %.4f\n", pre);
        free(s);
        return 1;
    }

    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->mode_demod = &raw_demod; // pass-through after preprocessing
    s->cqpsk_enable = 0;        // ensure IQ balance engages
    s->iqbal_enable = 1;
    s->iqbal_thr_q15 = 327;          // low-ish threshold to ensure engagement
    s->iqbal_alpha_ema_a_q15 = 8192; // moderate smoothing

    full_demod(s);

    double post = impropriety_ratio(s->lowpassed, s->lp_len / 2);
    if (!(post < pre)) {
        fprintf(stderr, "IQBAL test: post impropriety %.4f not reduced from %.4f\n", post, pre);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
