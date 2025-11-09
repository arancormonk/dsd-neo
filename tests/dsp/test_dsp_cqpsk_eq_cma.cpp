// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: CMA warmup updates FFE taps, keeps WL frozen except leakage, and stays in FFE mode.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

static void
make_qpsk(int16_t* dst, int pairs, int amp, unsigned* seed) {
    unsigned s = *seed;
    for (int n = 0; n < pairs; n++) {
        s = s * 1103515245u + 12345u;
        dst[2 * n + 0] = (int16_t)(((s >> 31) & 1) ? amp : -amp);
        dst[2 * n + 1] = (int16_t)(((s >> 30) & 1) ? amp : -amp);
    }
    *seed = s;
}

int
main(void) {
    const int N = 1024;
    int16_t buf[2 * N];
    unsigned seed = 0xBEEF;
    make_qpsk(buf, N, 6000, &seed);

    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);
    st.sym_stride = 1;   // symbol ticks every pair
    st.cma_warmup = 256; // run CMA for first 256 samples
    st.cma_mu_q15 = 64;
    st.lms_enable = 1; // will be skipped during warmup
    st.update_stride = 1;
    st.mu_q15 = 64;
    // Preload WL taps to non-zero to observe leakage
    st.wl_enable = 1;
    st.cw_i[0] = 500;
    st.cw_q[0] = -400;

    // Capture initial center + neighbor
    int16_t c0i0 = st.c_i[0], c1i0 = st.c_i[1];
    int16_t wli0 = st.cw_i[0], wlq0 = st.cw_q[0];

    // Run exactly warmup samples to observe WL leakage-only phase
    const int warm_pairs = 256;
    cqpsk_eq_process_block(&st, buf, 2 * warm_pairs);

    // CMA should change FFE taps from initial
    if (st.c_i[0] == c0i0 && st.c_i[1] == c1i0) {
        fprintf(stderr, "CMA: FFE taps unchanged\n");
        return 1;
    }
    // WL should not be increased during warmup; leakage should reduce magnitude
    int wl0 = (wli0 >= 0 ? wli0 : -wli0) + (wlq0 >= 0 ? wlq0 : -wlq0);
    int wl1 = (st.cw_i[0] >= 0 ? st.cw_i[0] : -st.cw_i[0]) + (st.cw_q[0] >= 0 ? st.cw_q[0] : -st.cw_q[0]);
    if (!(wl1 <= wl0)) {
        fprintf(stderr, "CMA: WL leakage not observed (wl0=%d wl1=%d)\n", wl0, wl1);
        return 1;
    }
    // Ensure mode is FFE at end
    if (st.adapt_mode != 0) {
        fprintf(stderr, "CMA: adapt_mode not FFE at end (%d)\n", st.adapt_mode);
        return 1;
    }
    return 0;
}
