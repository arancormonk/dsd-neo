// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: power squelch zeros lowpassed when below threshold; passes when above. */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp
int use_halfband_decimator = 0;
int fll_lut_enabled = 0;

static int
all_zero(const int16_t* x, int n) {
    for (int i = 0; i < n; i++) {
        if (x[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    const int pairs = 200;
    static int16_t buf[(size_t)pairs * 2];
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->mode_demod = &raw_demod; // copy lowpassed -> result

    // Below threshold: set small magnitude
    for (int n = 0; n < pairs; n++) {
        buf[(size_t)(2 * n) + 0] = 50;
        buf[(size_t)(2 * n) + 1] = -30;
    }
    s->squelch_level = 5000;     // per-component mean power threshold
    s->squelch_decim_stride = 8; // small for test

    full_demod(s);
    if (!all_zero(s->lowpassed, s->lp_len)) {
        fprintf(stderr, "squelch: below threshold not zeroed\n");
        free(s);
        return 1;
    }

    // Above threshold: larger magnitude should pass
    for (int n = 0; n < pairs; n++) {
        buf[(size_t)(2 * n) + 0] = 400;
        buf[(size_t)(2 * n) + 1] = 300;
    }
    s->squelch_running_power = 0; // reset
    full_demod(s);
    if (all_zero(s->lowpassed, s->lp_len)) {
        fprintf(stderr, "squelch: above threshold unexpectedly zeroed\n");
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
