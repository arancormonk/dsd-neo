// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: parameter extremes and bounds—tap clamps and WL caps respected with near-full-scale input.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

static int
abs16(int v) {
    return v >= 0 ? v : -v;
}

int
main(void) {
    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);
    st.lms_enable = 1;
    st.update_stride = 1;
    st.sym_stride = 1;
    st.mu_q15 = 4096; // large
    st.eps_q15 = 1;   // tiny epsilon
    st.wl_enable = 1;
    st.wl_mu_q15 = 4096;
    st.wl_leak_shift = 10;

    const int N = 2048;
    int16_t buf[2 * N];
    for (int n = 0; n < N; n++) {
        int16_t v = (int16_t)((n & 1) ? 32000 : -32000);
        buf[2 * n + 0] = v;
        buf[2 * n + 1] = (int16_t)((n & 2) ? 30000 : -30000);
    }
    cqpsk_eq_process_block(&st, buf, 2 * N);

    int maxc = st.max_abs_q14;
    int wl_cap = maxc >> 3;
    if (wl_cap < 1) {
        wl_cap = 1;
    }
    // FFE tap bounds
    for (int k = 0; k < st.num_taps; k++) {
        if (abs16(st.c_i[k]) > maxc || abs16(st.c_q[k]) > maxc) {
            fprintf(stderr, "BOUNDS: FFE tap out of bounds at %d\n", k);
            return 1;
        }
        if (abs16(st.cw_i[k]) > wl_cap || abs16(st.cw_q[k]) > wl_cap) {
            fprintf(stderr, "BOUNDS: WL tap out of cap at %d\n", k);
            return 1;
        }
    }
    return 0;
}
