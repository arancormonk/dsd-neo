// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: dsd_fm_demod + polar_discriminant returns constant for constant dphi. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/polar_disc.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp to avoid undefined refs
int use_halfband_decimator = 0;

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    // Build a complex tone that advances by constant phase per sample
    const int N = 256; // complex pairs
    static int16_t iq[(size_t)N * 2];
    double Fs = 48000.0;
    double f_dev = 3000.0; // radians per second mapped to dphi = 2*pi*f/Fs
    double dphi = 2.0 * 3.14159265358979323846 * f_dev / Fs;
    double A = 12000.0;
    for (int k = 0; k < N; k++) {
        double th = k * dphi;
        iq[(size_t)(2 * k) + 0] = (int16_t)lrint(A * cos(th));
        iq[(size_t)(2 * k) + 1] = (int16_t)lrint(A * sin(th));
    }

    s->lowpassed = iq;
    s->lp_len = N * 2;
    s->discriminator = &polar_discriminant;
    s->fll_enabled = 0;
    s->pre_r = 0;
    s->pre_j = 0;

    dsd_fm_demod(s);

    // Expected Q14 value: q = dphi/pi * 2^14 = f/Fs * 2^15
    int q_expect = (int)lrint((f_dev / Fs) * 32768.0);
    if (s->result_len != N) {
        fprintf(stderr, "FM demod ref: result_len=%d want %d\n", s->result_len, N);
        free(s);
        return 1;
    }
    // Ignore the very first sample; check steady-state
    for (int i = 1; i < s->result_len; i++) {
        int v = s->result[i];
        int d = v - q_expect;
        if (d < 0) {
            d = -d;
        }
        if (d > 64) { // allow small tolerance
            fprintf(stderr, "FM demod ref: result[%d]=%d expect~%d\n", i, v, q_expect);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
