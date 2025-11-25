// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for Gardner TED timing adjustment with native float implementation. */

#include <dsd-neo/dsp/ted.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int
main(void) {
    // Construct a constant complex sequence → Gardner error ~ 0
    const int N0 = 20; // complex samples
    float x[2 * N0];
    for (int k = 0; k < N0; k++) {
        x[2 * k + 0] = 0.1f;
        x[2 * k + 1] = -0.05f;
    }
    float y[2 * N0];
    int N = 2 * N0;

    ted_config_t cfg;
    cfg.enabled = 1;
    cfg.force = 1;     // ensure it runs regardless of sps
    cfg.gain = 0.001f; // tiny native float gain for test
    cfg.sps = 10;      // typical

    ted_state_t st;
    ted_init_state(&st);

    gardner_timing_adjust(&cfg, &st, x, &N, y);

    // Expected: length reduced by 2 complex samples → 4 elements
    int expected_elems = 2 * (N0 - 1);
    if (N != expected_elems) {
        fprintf(stderr, "TED: adjusted length elems=%d expected=%d\n", N, expected_elems);
        return 1;
    }

    // mu should have advanced by iter * mu_nom (mod 1.0) in native float
    int iter = (expected_elems) / 2; // complex samples output
    float mu_nom = 1.0f / (float)cfg.sps;
    float mu_expected = fmodf(iter * mu_nom, 1.0f);
    if (fabsf(st.mu - mu_expected) > 0.001f) {
        fprintf(stderr, "TED: mu=%f expected=%f\n", st.mu, mu_expected);
        return 1;
    }

    // Residual should remain ~0 for constant signal
    if (fabsf(st.e_ema) > 0.001f) {
        fprintf(stderr, "TED: residual e_ema=%f expected ~0\n", st.e_ema);
        return 1;
    }

    return 0;
}
