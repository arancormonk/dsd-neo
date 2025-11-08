// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit tests for FLL mix/update helpers. */

#include <dsd-neo/dsp/fll.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int
arrays_close(const int16_t* a, const int16_t* b, int n, int tol) {
    for (int i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) {
            d = -d;
        }
        if (d > tol) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    // Test 1: mix with freq=0 should be a no-op
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = 0; // no rotation

        int16_t x[20];
        for (int i = 0; i < 20; i++) {
            x[i] = (int16_t)(i * 17 - 100);
        }
        int16_t y[20];
        memcpy(y, x, sizeof(x));

        fll_mix_and_update(&cfg, &st, x, 20);
        if (!arrays_close(x, y, 20, 1)) {
            fprintf(stderr, "FLL mix (fast): freq=0 deviated >1 LSB\n");
            return 1;
        }

        /* Re-run once more to ensure determinism */
        memcpy(x, y, sizeof(x));
        fll_mix_and_update(&cfg, &st, x, 20);
        if (!arrays_close(x, y, 20, 1)) {
            fprintf(stderr, "FLL mix: freq=0 deviated >1 LSB\n");
            return 1;
        }
    }

    // Test 2: update_error should move freq in sign of observed CFO
    {
        const int N = 100; // even
        int16_t iq[2 * N];
        double r = 12000.0;                   // arbitrary radius within int16 range
        double dtheta = (2.0 * M_PI) / 200.0; // small positive rotation per complex sample
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            int16_t i = (int16_t)lrint(r * cos(th));
            int16_t q = (int16_t)lrint(r * sin(th));
            iq[2 * k + 0] = i;
            iq[2 * k + 1] = q;
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 1200; // small gains
        cfg.beta_q15 = 800;
        cfg.deadband_q14 = 0;     // respond to small errors
        cfg.slew_max_q15 = 32767; // effectively unlimited for test

        fll_state_t st;
        fll_init_state(&st);
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL update: expected positive freq correction, got %d\n", st.freq_q15);
            return 1;
        }

        // Negative rotation
        for (int k = 0; k < N; k++) {
            double th = -k * dtheta;
            int16_t i = (int16_t)lrint(r * cos(th));
            int16_t q = (int16_t)lrint(r * sin(th));
            iq[2 * k + 0] = i;
            iq[2 * k + 1] = q;
        }
        fll_init_state(&st);
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 >= 0) {
            fprintf(stderr, "FLL update: expected negative freq correction, got %d\n", st.freq_q15);
            return 1;
        }
    }

    return 0;
}
