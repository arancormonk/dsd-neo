// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: complex half-band decimator path via full_demod (DC preservation and decimation). */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
approx_eq(float a, float b, float tol) {
    float d = a - b;
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
    DSD_MEMSET(s, 0, sizeof(*s));

    // Prepare constant DC complex input
    const int pairs = 128;
    static float in[(size_t)pairs * 2];
    for (int k = 0; k < pairs; k++) {
        in[(size_t)(2 * k) + 0] = 0.25f;   // I
        in[(size_t)(2 * k) + 1] = -0.125f; // Q
    }
    s->lowpassed = in;
    s->lp_len = pairs * 2;
    s->downsample_passes = 1;
    s->mode_demod = &raw_demod; // copy lowpassed -> result
    s->iq_dc_block_enable = 0;
    s->iqbal_enable = 0;
    s->ted_enabled = 0;

    full_demod(s);

    // Expect 2:1 complex decimation of what the first (31-tap, c = 15) stage can centre: a fresh stream holds back the
    // last c samples' look-ahead, so 128 pairs make (128 - 15 + 1) / 2 = 57 pairs.
    const int want_pairs = (pairs - 15 + 1) / 2;
    if (s->result_len != want_pairs * 2) {
        DSD_FPRINTF(stderr, "HB complex: result_len=%d want %d\n", s->result_len, want_pairs * 2);
        free(s);
        return 1;
    }
    // From the first output whose 31-tap window lies wholly in the block (2k - 15 >= 0) to the last, DC is preserved
    // within a few LSBs: no output reads past the block, so no tail is excluded.
    for (int k = 8; k < s->result_len / 2; k++) {
        float I = s->result[(size_t)(2 * k) + 0];
        float Q = s->result[(size_t)(2 * k) + 1];
        if (!approx_eq(I, 0.25f, 1e-3f) || !approx_eq(Q, -0.125f, 1e-3f)) {
            DSD_FPRINTF(stderr, "HB complex: sample %d=(%f,%f) deviates from DC\n", k, I, Q);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
