// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: symbol ring capacity and ordering (returns last CQPSK_EQ_SYM_MAX in order).
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

int
main(void) {
    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);
    st.lms_enable = 0; // identity
    st.sym_stride = 1;

    const int K = CQPSK_EQ_SYM_MAX + 100; // overflow capacity
    int16_t buf[2 * K];
    for (int n = 0; n < K; n++) {
        buf[2 * n + 0] = (int16_t)(n & 0x7FFF);
        buf[2 * n + 1] = (int16_t)(((n * 3) & 0x7FFF) - 16384);
    }
    cqpsk_eq_process_block(&st, buf, 2 * K);

    int16_t out[2 * (CQPSK_EQ_SYM_MAX + 8)];
    int n = cqpsk_eq_get_symbols(&st, out, CQPSK_EQ_SYM_MAX + 8);
    if (n != CQPSK_EQ_SYM_MAX) {
        fprintf(stderr, "RINGCAP: expected %d, got %d\n", CQPSK_EQ_SYM_MAX, n);
        return 1;
    }
    // Expect sequence corresponds to buf[K - n ... K - 1]
    int start = K - n;
    for (int k = 0; k < n; k++) {
        int16_t ei = buf[2 * (start + k) + 0];
        int16_t eq = buf[2 * (start + k) + 1];
        if (out[2 * k + 0] != ei || out[2 * k + 1] != eq) {
            fprintf(stderr, "RINGCAP: mismatch at %d\n", k);
            return 1;
        }
    }
    return 0;
}
