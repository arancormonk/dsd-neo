// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for Gardner TED timing adjustment. */

#include <dsd-neo/dsp/ted.h>
#include <stdio.h>
#include <string.h>

int
main(void) {
    // Construct a constant complex sequence → Gardner error ~ 0
    const int N0 = 20; // even
    int16_t x[2 * N0];
    for (int k = 0; k < N0; k++) {
        x[2 * k + 0] = 5000;
        x[2 * k + 1] = -2000;
    }
    int16_t y[2 * N0];
    int N = 2 * N0;

    ted_config_t cfg;
    cfg.enabled = 1;
    cfg.force = 1;    // ensure it runs regardless of sps
    cfg.gain_q20 = 1; // tiny gain for test
    cfg.sps = 10;     // typical

    ted_state_t st;
    ted_init_state(&st);

    gardner_timing_adjust(&cfg, &st, x, &N, y);

    // Expected: length reduced by 2 complex samples → 4 elements
    int expected_elems = 2 * (N0 - 1);
    if (N != expected_elems) {
        fprintf(stderr, "TED: adjusted length elems=%d expected=%d\n", N, expected_elems);
        return 1;
    }

    // mu should have advanced by iter * mu_nom (mod 1<<20)
    const int one = (1 << 20);
    int iter = (expected_elems) / 2; // complex samples output
    int mu_nom = one / cfg.sps;
    int mu_expected = (iter * mu_nom) % one;
    if (st.mu_q20 != mu_expected) {
        fprintf(stderr, "TED: mu=%d expected=%d\n", st.mu_q20, mu_expected);
        return 1;
    }

    // Residual should remain ~0 for constant signal
    if (st.e_ema != 0) {
        fprintf(stderr, "TED: residual e_ema=%d expected 0\n", st.e_ema);
        return 1;
    }

    return 0;
}
