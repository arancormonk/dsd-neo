// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: complex half-band decimator path via full_demod (DC preservation and decimation). */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp
int use_halfband_decimator = 1; // exercise HB path

static int
approx_eq(int a, int b, int tol) {
    int d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    // Prepare constant DC complex input
    const int pairs = 128;
    static int16_t in[(size_t)pairs * 2];
    for (int k = 0; k < pairs; k++) {
        in[(size_t)(2 * k) + 0] = 1000; // I
        in[(size_t)(2 * k) + 1] = -500; // Q
    }
    s->lowpassed = in;
    s->lp_len = pairs * 2;
    s->downsample_passes = 1;
    s->mode_demod = &raw_demod; // copy lowpassed -> result
    s->iq_dc_block_enable = 0;
    s->fm_agc_enable = 0;
    s->fm_cma_enable = 0;
    s->iqbal_enable = 0;
    s->fll_enabled = 0;
    s->ted_enabled = 0;

    full_demod(s);

    // Expect 2:1 complex decimation (elements halved)
    if (s->result_len != pairs) {
        fprintf(stderr, "HB complex: result_len=%d want %d\n", s->result_len, pairs);
        free(s);
        return 1;
    }
    // After warmup (~HB_TAPS), DC should be preserved within a few LSBs
    for (int k = 16; k < (s->result_len / 2) - 8; k++) {
        int I = s->result[(size_t)(2 * k) + 0];
        int Q = s->result[(size_t)(2 * k) + 1];
        if (!approx_eq(I, 1000, 4) || !approx_eq(Q, -500, 4)) {
            fprintf(stderr, "HB complex: sample %d=(%d,%d) deviates from DC\n", k, I, Q);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
