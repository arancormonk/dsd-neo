// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: DQPSK decision mode should not degrade and typically improves EVM
 * under a constant incremental phase rotation compared to axis decision.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void
gen_rotating_qpsk(int16_t* dst, int pairs, double deg_step, int amp) {
    double ph = 0.0;
    for (int n = 0; n < pairs; n++) {
        double c = cos(ph), s = sin(ph);
        int16_t i = (int16_t)(amp * c);
        int16_t q = (int16_t)(amp * s);
        dst[2 * n + 0] = i;
        dst[2 * n + 1] = q;
        ph += deg_step * (M_PI / 180.0);
        if (ph > M_PI) {
            ph -= 2 * M_PI;
        }
    }
}

static long long
evm_tail(const int16_t* xy, int pairs, int tail) {
    int start = pairs - tail;
    if (start < 0) {
        start = 0;
    }
    long long ssq = 0;
    for (int k = start; k < pairs; k++) {
        long long yi = xy[2 * k + 0];
        long long yq = xy[2 * k + 1];
        // For rotating grid, evaluate fidelity to same radius
        long long r = (llabs(yi) > llabs(yq)) ? llabs(yi) : llabs(yq);
        long long di = (yi >= 0) ? r : -r;
        long long dq = (yq >= 0) ? r : -r;
        long long ei = di - yi, eq = dq - yq;
        ssq += ei * ei + eq * eq;
    }
    return ssq;
}

int
main(void) {
    const int N = 1200; // pairs
    int16_t buf[2 * N];
    // 45 deg / symbol, challenging for axis slicer; amplitude modest
    gen_rotating_qpsk(buf, N, 45.0, 7000);

    // Pass through a mild ISI channel for realism: 0.9 + 0.1 z^-1
    int16_t ch[2 * N];
    int32_t pi = 0, pq = 0;
    for (int n = 0; n < N; n++) {
        int32_t xi = buf[2 * n + 0];
        int32_t xq = buf[2 * n + 1];
        ch[2 * n + 0] = (int16_t)(((xi * 29491) + (pi * 3277)) >> 15);
        ch[2 * n + 1] = (int16_t)(((xq * 29491) + (pq * 3277)) >> 15);
        pi = xi;
        pq = xq;
    }

    // Axis decision
    cqpsk_eq_state_t ax;
    memset(&ax, 0, sizeof(ax));
    cqpsk_eq_init(&ax);
    ax.lms_enable = 1;
    ax.mu_q15 = 64;
    ax.update_stride = 1;
    ax.sym_stride = 1;
    ax.dqpsk_decision = 0;
    int16_t b1[2 * N];
    memcpy(b1, ch, sizeof(ch));
    cqpsk_eq_process_block(&ax, b1, 2 * N);
    int16_t s1[N * 2];
    int n1 = cqpsk_eq_get_symbols(&ax, s1, N);
    if (n1 <= 0) {
        fprintf(stderr, "DQPSK: axis no symbols\n");
        return 1;
    }
    long long e1 = evm_tail(s1, n1, 256);

    // DQPSK decision
    cqpsk_eq_state_t dq;
    memset(&dq, 0, sizeof(dq));
    cqpsk_eq_init(&dq);
    dq.lms_enable = 1;
    dq.mu_q15 = 64;
    dq.update_stride = 1;
    dq.sym_stride = 1;
    dq.dqpsk_decision = 1;
    int16_t b2[2 * N];
    memcpy(b2, ch, sizeof(ch));
    cqpsk_eq_process_block(&dq, b2, 2 * N);
    int16_t s2[N * 2];
    int n2 = cqpsk_eq_get_symbols(&dq, s2, N);
    if (n2 <= 0) {
        fprintf(stderr, "DQPSK: dqpsk no symbols\n");
        return 1;
    }
    long long e2 = evm_tail(s2, n2, 256);

    // Allow a small tolerance; DQPSK should be no worse than ~5%
    if (!(e2 * 100LL <= e1 * 105LL)) {
        fprintf(stderr, "DQPSK: no improvement within tol (axis=%lld dq=%lld)\n", e1, e2);
        return 1;
    }
    return 0;
}
