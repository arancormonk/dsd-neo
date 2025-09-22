// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: RRC matched filter in full_demod preserves DC (normalized gain). */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp to avoid linking RTL front-end
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

    // Configure to run only the RRC matched filter inside full_demod
    s->cqpsk_enable = 1;
    s->cqpsk_mf_enable = 1;
    s->cqpsk_rrc_enable = 1;
    s->ted_sps = 10; // typical
    s->fll_enabled = 0;
    s->ted_enabled = 0;
    s->audio_lpf_enable = 0;
    s->squelch_level = 0;
    s->mode_demod = &raw_demod; // pass-through after preprocessing

    const int pairs = 64;
    s->lowpassed = s->hb_workbuf; // use internal buffer storage
    for (int k = 0; k < pairs; k++) {
        s->lowpassed[(size_t)(2 * k) + 0] = 1200; // DC I
        s->lowpassed[(size_t)(2 * k) + 1] = -300; // DC Q
    }
    s->lp_len = pairs * 2;

    // Call the full demod; with raw_demod it returns early after preprocessing
    full_demod(s);

    // RRC gain normalized to unity → DC preserved (allow small rounding)
    for (int k = 4; k < pairs - 4; k++) { // skip a few edges
        int I = s->lowpassed[(size_t)(2 * k) + 0];
        int Q = s->lowpassed[(size_t)(2 * k) + 1];
        if (!approx_eq(I, 1200, 2) || !approx_eq(Q, -300, 2)) {
            fprintf(stderr, "RRC: sample %d=(%d,%d) deviates from DC\n", k, I, Q);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
