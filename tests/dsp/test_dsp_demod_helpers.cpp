// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: demod pipeline helpers (low_pass_simple, mean_power). */

#include <dsd-neo/dsp/demod_pipeline.h>

// Provide globals expected by demod_pipeline.cpp to avoid linking RTL front-end
int use_halfband_decimator = 0;
#include <stdio.h>

int
main(void) {
    // low_pass_simple: step=2 should average adjacent pairs with rounding
    {
        int16_t x[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        int out_len = low_pass_simple(x, 8, 2);
        if (out_len != 4) {
            fprintf(stderr, "low_pass_simple: out_len=%d want 4\n", out_len);
            return 1;
        }
        if (!(x[0] == 2 && x[1] == 4 && x[2] == 6 && x[3] == 8)) {
            fprintf(stderr, "low_pass_simple: values mismatch %d %d %d %d\n", x[0], x[1], x[2], x[3]);
            return 1;
        }
    }

    // mean_power: DC-corrected mean
    {
        int16_t a[4] = {1, 1, 1, 1};
        long mp = mean_power(a, 4, 1);
        if (mp != 0) {
            fprintf(stderr, "mean_power: DC vector expected 0 got %ld\n", mp);
            return 1;
        }
        int16_t b[4] = {1, -1, 1, -1};
        mp = mean_power(b, 4, 1);
        if (mp != 1) {
            fprintf(stderr, "mean_power: alt signs expected 1 got %ld\n", mp);
            return 1;
        }
    }

    return 0;
}
