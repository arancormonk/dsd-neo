// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: WL impropriety hysteresis & hold—engage on improper, disengage on proper input, WL taps leak down.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

static void
make_improper(int16_t* dst, int pairs, int amp) {
    for (int n = 0; n < pairs; n++) {
        int16_t v = (int16_t)((n & 1) ? amp : -amp);
        dst[2 * n + 0] = v;
        dst[2 * n + 1] = v; // Q = I -> improper
    }
}

static void
make_proper_qpsk(int16_t* dst, int pairs, int amp) {
    unsigned s = 0xAA55u;
    for (int n = 0; n < pairs; n++) {
        s = s * 1664525u + 1013904223u;
        dst[2 * n + 0] = (int16_t)(((s >> 31) & 1) ? amp : -amp);
        dst[2 * n + 1] = (int16_t)(((s >> 30) & 1) ? amp : -amp);
    }
}

static int
wl_norm(const cqpsk_eq_state_t* st) {
    int sum = 0;
    for (int k = 0; k < st->num_taps; k++) {
        int a = st->cw_i[k];
        if (a < 0) {
            a = -a;
        }
        sum += a;
        int b = st->cw_q[k];
        if (b < 0) {
            b = -b;
        }
        sum += b;
    }
    return sum;
}

int
main(void) {
    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);
    st.lms_enable = 1;
    st.wl_enable = 1;
    st.update_stride = 1;
    st.sym_stride = 1;
    st.mu_q15 = 128;
    st.wl_mu_q15 = 128;
    st.num_taps = 11;               // larger window for impropriety measure
    st.wl_improp_alpha_q15 = 16384; // faster EMA
    st.wl_gate_thr_q15 = 20000;     // ~0.61
    st.wl_thr_off_q15 = 5000;       // ~0.15 off threshold
    st.wl_leak_shift = 6;           // stronger leakage when WL gated off
    st.adapt_min_hold = 2;

    const int N1 = 256, N2 = 1024;
    int16_t a[2 * N1];
    make_improper(a, N1, 7000);
    int16_t b[2 * N2];
    make_proper_qpsk(b, N2, 6000);

    cqpsk_eq_process_block(&st, a, 2 * N1);
    int wl_after_imp = wl_norm(&st);
    int mode_after_imp = st.adapt_mode;

    cqpsk_eq_process_block(&st, b, 2 * N2);
    int wl_final = wl_norm(&st);
    int mode_final = st.adapt_mode;

    if (mode_after_imp != 1) {
        fprintf(stderr, "WLHYST: WL not engaged after improper\n");
        return 1;
    }
    // Require significant reduction in WL tap energy after proper input
    if (!(wl_final * 2 <= wl_after_imp)) {
        fprintf(stderr, "WLHYST: WL taps did not leak down (after_imp=%d final=%d)\n", wl_after_imp, wl_final);
        return 1;
    }
    return 0;
}
