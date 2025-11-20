// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: In presence of moderate AWGN, adaptation improves tail EVM vs baseline.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static inline int
sgn(int v) {
    return (v >= 0) ? 1 : -1;
}

static inline void
slicer_target(int16_t i, int16_t q, int16_t* oi, int16_t* oq) {
    int ai = (i >= 0) ? i : -i;
    int aq = (q >= 0) ? q : -q;
    int r = (ai > aq) ? ai : aq;
    *oi = (int16_t)(sgn(i) * r);
    *oq = (int16_t)(sgn(q) * r);
}

static long long
evm_tail(const int16_t* xy, int pairs, int tail) {
    int start = pairs - tail;
    if (start < 0) {
        start = 0;
    }
    long long ssq = 0;
    for (int k = start; k < pairs; k++) {
        int16_t yi = xy[2 * k + 0], yq = xy[2 * k + 1];
        int16_t di, dq;
        slicer_target(yi, yq, &di, &dq);
        long long ei = (long long)di - yi, eq = (long long)dq - yq;
        ssq += ei * ei + eq * eq;
    }
    return ssq;
}

static int16_t
clip16(int x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (int16_t)x;
}

static void
apply_channel_2tap(const int16_t* in, int pairs, int16_t* out, int a0_q15, int a1_q15) {
    int32_t pi = 0, pq = 0;
    for (int n = 0; n < pairs; n++) {
        int32_t xi = in[2 * n + 0];
        int32_t xq = in[2 * n + 1];
        out[2 * n + 0] = clip16(((xi * a0_q15) + (pi * a1_q15)) >> 15);
        out[2 * n + 1] = clip16(((xq * a0_q15) + (pq * a1_q15)) >> 15);
        pi = xi;
        pq = xq;
    }
}

int
main(void) {
    const int N = 1600;
    int16_t clean[2 * N];
    unsigned s = 0xCAFEBABE;
    const int amp = 7000;
    for (int n = 0; n < N; n++) {
        s = s * 1103515245u + 12345u;
        clean[2 * n + 0] = (int16_t)(((s >> 31) & 1) ? amp : -amp);
        clean[2 * n + 1] = (int16_t)(((s >> 30) & 1) ? amp : -amp);
    }
    // Apply mild ISI channel then AWGN approx via LCG -> uniform -> CLT 12-sample sum
    int16_t ch[2 * N];
    apply_channel_2tap(clean, N, ch, 30147 /*0.92*/, 2621 /*0.08*/);
    int16_t noisy[2 * N];
    for (int n = 0; n < N; n++) {
        int ni = 0, nq = 0;
        for (int k = 0; k < 12; k++) {
            s = s * 1664525u + 1013904223u;
            ni += (int)((s >> 16) & 0xFFFF) - 32768;
        }
        for (int k = 0; k < 12; k++) {
            s = s * 1664525u + 1013904223u;
            nq += (int)((s >> 16) & 0xFFFF) - 32768;
        }
        // scale noise to ~18 dB SNR relative to amp
        ni /= 2048;
        nq /= 2048;
        noisy[2 * n + 0] = clip16((int)ch[2 * n + 0] + ni);
        noisy[2 * n + 1] = clip16((int)ch[2 * n + 1] + nq);
    }

    // Baseline (no adaptation)
    cqpsk_eq_state_t base;
    memset(&base, 0, sizeof(base));
    cqpsk_eq_init(&base);
    base.sym_stride = 1;
    int16_t b1[2 * N];
    memcpy(b1, noisy, sizeof(noisy));
    cqpsk_eq_process_block(&base, b1, 2 * N);
    int16_t s1[N * 2];
    int n1 = cqpsk_eq_get_symbols(&base, s1, N);
    if (n1 <= 0) {
        fprintf(stderr, "AWGN: base no syms\n");
        return 1;
    }
    long long e1 = evm_tail(s1, n1, 256);

    // Adapted
    cqpsk_eq_state_t ad;
    memset(&ad, 0, sizeof(ad));
    cqpsk_eq_init(&ad);
    // CMA warmup helps stabilize DD under noise
    ad.cma_warmup = 256;
    ad.cma_mu_q15 = 64;
    ad.lms_enable = 1;
    ad.mu_q15 = 64;
    ad.update_stride = 1;
    ad.sym_stride = 1;
    ad.eps_q15 = 4;
    int16_t b2[2 * N];
    memcpy(b2, noisy, sizeof(noisy));
    cqpsk_eq_process_block(&ad, b2, 2 * N);
    int16_t s2[N * 2];
    int n2 = cqpsk_eq_get_symbols(&ad, s2, N);
    if (n2 <= 0) {
        fprintf(stderr, "AWGN: adapt no syms\n");
        return 1;
    }
    long long e2 = evm_tail(s2, n2, 256);

    if (!(e2 * 9LL <= e1 * 10LL)) { // >=10% improvement expected
        fprintf(stderr, "AWGN: insufficient EVM improvement (base=%lld adapt=%lld)\n", e1, e2);
        return 1;
    }
    return 0;
}
