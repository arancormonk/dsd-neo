// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for half-band decimator (real, Q15). */

#include <dsd-neo/dsp/halfband.h>
#include <stdio.h>
#include <stdlib.h>

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
    const int N = 64;
    int16_t in[N];
    int16_t out[N];
    int16_t hist[HB_TAPS - 1] = {0};

    // Constant DC input should pass with ~unity gain (Q15 taps normalized)
    for (int i = 0; i < N; i++) {
        in[i] = 1000;
    }

    int out_len = hb_decim2_real(in, N, out, hist);
    if (out_len != (N >> 1)) {
        fprintf(stderr, "HB: unexpected out_len=%d (want %d)\n", out_len, N >> 1);
        return 1;
    }
    // Skip initial transient due to zeroed history (warm-up ~HB_TAPS)
    for (int i = HB_TAPS; i < out_len; i++) {
        if (!approx_eq(out[i], 1000, 4)) {
            fprintf(stderr, "HB: output[%d]=%d not within tol of 1000\n", i, (int)out[i]);
            return 1;
        }
    }

    // Run a second block to exercise history maintenance
    int16_t out2[N];
    int out_len2 = hb_decim2_real(in, N, out2, hist);
    if (out_len2 != (N >> 1)) {
        fprintf(stderr, "HB: second call out_len=%d (want %d)\n", out_len2, N >> 1);
        return 1;
    }
    for (int i = 0; i < out_len2; i++) {
        if (!approx_eq(out2[i], 1000, 4)) {
            fprintf(stderr, "HB: second output[%d]=%d not within tol of 1000\n", i, (int)out2[i]);
            return 1;
        }
    }

    return 0;
}
