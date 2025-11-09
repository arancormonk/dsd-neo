// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused unit test: WL impropriety gate engages and WL taps adapt on improper input.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

int
main(void) {
    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);
    st.lms_enable = 1;
    st.wl_enable = 1;
    st.mu_q15 = 128;
    st.wl_mu_q15 = 128;
    st.update_stride = 1;
    st.sym_stride = 1;     // every pair is a symbol tick
    st.adapt_min_hold = 8; // allow switching fairly quickly

    // Improper input: Q = I causes |E[x^2]|/E[|x|^2] to be large
    const int N = 512;
    int16_t buf[2 * N];
    for (int n = 0; n < N; n++) {
        int16_t v = (int16_t)(((n & 1) ? 7000 : -7000));
        buf[2 * n + 0] = v; // I
        buf[2 * n + 1] = v; // Q = I
    }

    cqpsk_eq_process_block(&st, buf, 2 * N);

    // Expect WL mode engaged at some point (adapt_mode==1)
    if (st.adapt_mode != 1) {
        fprintf(stderr, "WL_GATE: WL mode not engaged (adapt_mode=%d)\n", st.adapt_mode);
        return 1;
    }

    // Expect some WL tap energy developed
    int wl_nonzero = 0;
    for (int k = 0; k < st.num_taps; k++) {
        if (st.cw_i[k] != 0 || st.cw_q[k] != 0) {
            wl_nonzero = 1;
            break;
        }
    }
    if (!wl_nonzero) {
        fprintf(stderr, "WL_GATE: WL taps did not adapt\n");
        return 1;
    }
    return 0;
}
