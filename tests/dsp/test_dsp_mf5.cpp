// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: 5-tap matched-like FIR on complex baseband preserves DC. */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp
int use_halfband_decimator = 0;
int fll_lut_enabled = 0;

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

    // Configure CQPSK pre-processing with 5-tap FIR
    s->cqpsk_enable = 1;
    s->cqpsk_mf_enable = 1;
    s->cqpsk_rrc_enable = 0;
    s->ted_sps = 10;    // any >1
    s->fll_enabled = 0; // disable carrier loop for this filter-only test
    s->mode_demod = &raw_demod;

    const int pairs = 64;
    s->lowpassed = s->hb_workbuf;
    for (int k = 0; k < pairs; k++) {
        s->lowpassed[(size_t)(2 * k) + 0] = 1200;
        s->lowpassed[(size_t)(2 * k) + 1] = -300;
    }
    s->lp_len = pairs * 2;

    full_demod(s);

    for (int k = 4; k < pairs - 4; k++) {
        int I = s->lowpassed[(size_t)(2 * k) + 0];
        int Q = s->lowpassed[(size_t)(2 * k) + 1];
        if (!approx_eq(I, 1200, 2) || !approx_eq(Q, -300, 2)) {
            fprintf(stderr, "MF5: sample %d=(%d,%d) deviates from DC\n", k, I, Q);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
