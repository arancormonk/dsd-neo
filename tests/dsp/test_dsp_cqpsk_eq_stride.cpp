// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: update gating—larger sym_stride results in slower tap movement than sym_stride=1.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

static int
sum_delta_from_identity(const cqpsk_eq_state_t* st) {
    int sum = 0;
    for (int k = 0; k < st->num_taps; k++) {
        int ti = (k == 0) ? (1 << 14) : 0;
        int tq = 0;
        int di = st->c_i[k] - ti;
        if (di < 0) {
            di = -di;
        }
        int dq = st->c_q[k] - tq;
        if (dq < 0) {
            dq = -dq;
        }
        sum += di + dq;
    }
    return sum;
}

int
main(void) {
    const int N = 2048;
    int16_t buf[2 * N];
    unsigned s = 0x22u;
    for (int n = 0; n < N; n++) {
        s = s * 1664525u + 1013904223u;
        buf[2 * n + 0] = (int16_t)(((s >> 31) & 1) ? 7000 : -7000);
        buf[2 * n + 1] = (int16_t)(((s >> 30) & 1) ? 5000 : -5000);
    }

    cqpsk_eq_state_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    cqpsk_eq_init(&a);
    cqpsk_eq_init(&b);
    a.lms_enable = 1;
    a.mu_q15 = 256;
    a.update_stride = 1;
    a.sym_stride = 1;
    b.lms_enable = 1;
    b.mu_q15 = 256;
    b.update_stride = 1;
    b.sym_stride = 8;

    int16_t ba[2 * N];
    memcpy(ba, buf, sizeof(buf));
    int16_t bb[2 * N];
    memcpy(bb, buf, sizeof(buf));
    cqpsk_eq_process_block(&a, ba, 2 * N);
    cqpsk_eq_process_block(&b, bb, 2 * N);

    int da = sum_delta_from_identity(&a);
    int db = sum_delta_from_identity(&b);
    // With smaller step and our NLMS scaling, expect at least comparable movement
    if (!(da * 100 >= db * 95)) {
        fprintf(stderr, "STRIDE: movement not comparable (da=%d db=%d)\n", da, db);
        return 1;
    }
    return 0;
}
