// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for half-band decimator (real, float taps). */

#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/simd_fir.h>
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
    const int N = 64;
    float in[N];
    float out[N];
    float hist[HB_TAPS - 1] = {0};
    int pending = 0;

    // Constant DC input should pass with ~unity gain
    for (int i = 0; i < N; i++) {
        in[i] = 1.0f;
    }

    // A fresh stream holds back the last c = 7 samples' look-ahead: (64 - 7 + 1) / 2 = 29 outputs, pending 6.
    int out_len = simd_hb_decim2_real(in, N, out, hist, &pending, hb_q15_taps, HB_TAPS);
    if (out_len != 29 || pending != 6) {
        DSD_FPRINTF(stderr, "HB: unexpected out_len=%d pending=%d (want 29, 6)\n", out_len, pending);
        return 1;
    }
    // Skip initial transient due to zeroed history (warm-up ~HB_TAPS)
    for (int i = HB_TAPS; i < out_len; i++) {
        if (!approx_eq(out[i], 1.0f, 1e-3f)) {
            DSD_FPRINTF(stderr, "HB: output[%d]=%f not within tol of 1.0\n", i, out[i]);
            return 1;
        }
    }

    // Run a second block to exercise history maintenance: (6 + 64 - 7 + 1) / 2 = 32 outputs, pending 6 again.
    float out2[N];
    int out_len2 = simd_hb_decim2_real(in, N, out2, hist, &pending, hb_q15_taps, HB_TAPS);
    if (out_len2 != 32 || pending != 6) {
        DSD_FPRINTF(stderr, "HB: second call out_len=%d pending=%d (want 32, 6)\n", out_len2, pending);
        return 1;
    }
    for (int i = 0; i < out_len2; i++) {
        if (!approx_eq(out2[i], 1.0f, 1e-3f)) {
            DSD_FPRINTF(stderr, "HB: second output[%d]=%f not within tol of 1.0\n", i, out2[i]);
            return 1;
        }
    }

    return 0;
}
