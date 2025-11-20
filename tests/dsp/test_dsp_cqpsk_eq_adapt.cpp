// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused unit test: CQPSK equalizer adaptation reduces EVM on a simple ISI channel.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

static inline int
sgn(int v) {
    return (v >= 0) ? 1 : -1;
}

/* Simple slicer-derived target: axis-aligned QPSK decision with per-sample radius. */
static inline void
slicer_target(int16_t i, int16_t q, int16_t* oi, int16_t* oq) {
    int ai = (i >= 0) ? i : -i;
    int aq = (q >= 0) ? q : -q;
    int r = (ai > aq) ? ai : aq;
    *oi = (int16_t)(sgn(i) * r);
    *oq = (int16_t)(sgn(q) * r);
}

static long long
evm_ssq(const int16_t* xy, int pairs, int tail_only) {
    int start = 0;
    if (tail_only) {
        start = pairs - tail_only;
        if (start < 0) {
            start = 0;
        }
    }
    long long ssq = 0;
    for (int k = start; k < pairs; k++) {
        int16_t yi = xy[2 * k + 0];
        int16_t yq = xy[2 * k + 1];
        int16_t di, dq;
        slicer_target(yi, yq, &di, &dq);
        long long ei = (long long)di - (long long)yi;
        long long eq = (long long)dq - (long long)yq;
        ssq += ei * ei + eq * eq;
    }
    return ssq;
}

/* Apply a simple real FIR channel h = [a0, a1] to complex data (int16 interleaved). */
static void
apply_channel_2tap(const int16_t* in, int pairs, int16_t* out, int a0_q15, int a1_q15) {
    int32_t prev_i = 0, prev_q = 0;
    for (int n = 0; n < pairs; n++) {
        int32_t xi = in[2 * n + 0];
        int32_t xq = in[2 * n + 1];
        int32_t yi = ((xi * a0_q15) + (prev_i * a1_q15)) >> 15;
        int32_t yq = ((xq * a0_q15) + (prev_q * a1_q15)) >> 15;
        out[2 * n + 0] = (int16_t)yi;
        out[2 * n + 1] = (int16_t)yq;
        prev_i = xi;
        prev_q = xq;
    }
}

int
main(void) {
    cqpsk_eq_state_t st_base;
    cqpsk_eq_state_t st_adapt;
    memset(&st_base, 0, sizeof(st_base));
    memset(&st_adapt, 0, sizeof(st_adapt));
    cqpsk_eq_init(&st_base);
    cqpsk_eq_init(&st_adapt);

    // Configuration for deterministic adaptation
    st_adapt.lms_enable = 1;
    st_adapt.mu_q15 = 4;        // very conservative step
    st_adapt.update_stride = 2; // update every 2 samples
    st_adapt.num_taps = 3;      // simpler channel model
    st_adapt.sym_stride = 1;    // every pair is a symbol tick
    st_adapt.eps_q15 = 4;

    const int N = 2048; // pairs
    int16_t src[2 * N];
    // Pseudo-random QPSK-like sequence
    unsigned s = 0xC0FFEEu;
    const int amp = 6000;
    for (int n = 0; n < N; n++) {
        s = s * 1103515245u + 12345u;
        int si = (s >> 30) & 1;
        int sq = (s >> 29) & 1;
        src[2 * n + 0] = (int16_t)(si ? amp : -amp);
        src[2 * n + 1] = (int16_t)(sq ? amp : -amp);
    }

    // 2-tap ISI channel: y = 0.85*x[n] + 0.15*x[n-1]
    int16_t ch[2 * N];
    apply_channel_2tap(src, N, ch, 27853 /*0.85*/, 4915 /*0.15*/);

    // Baseline (no adaptation)
    int16_t base_buf[2 * N];
    memcpy(base_buf, ch, sizeof(ch));
    st_base.sym_stride = 1;
    cqpsk_eq_process_block(&st_base, base_buf, 2 * N);
    int16_t base_syms[N * 2];
    int nb = cqpsk_eq_get_symbols(&st_base, base_syms, N);
    if (nb <= 0) {
        fprintf(stderr, "EQ_ADAPT: no baseline symbols captured\n");
        return 1;
    }
    long long evm_base = evm_ssq(base_syms, nb, 256);

    // With adaptation
    int16_t adapt_buf[2 * N];
    memcpy(adapt_buf, ch, sizeof(ch));
    cqpsk_eq_process_block(&st_adapt, adapt_buf, 2 * N);
    int16_t adapt_syms[N * 2];
    int na = cqpsk_eq_get_symbols(&st_adapt, adapt_syms, N);
    if (na <= 0) {
        fprintf(stderr, "EQ_ADAPT: no adapted symbols captured\n");
        return 1;
    }
    long long evm_adapt = evm_ssq(adapt_syms, na, 256);

    if (!(evm_adapt * 5LL <= evm_base * 4LL)) { // require >=20% EVM reduction
        fprintf(stderr, "EQ_ADAPT: EVM reduction insufficient (adapt=%lld base=%lld)\n", evm_adapt, evm_base);
        fprintf(stderr, "EQ_ADAPT: taps after adapt: ");
        for (int k = 0; k < st_adapt.num_taps; k++) {
            fprintf(stderr, "(%d,%d) ", (int)st_adapt.c_i[k], (int)st_adapt.c_q[k]);
        }
        fprintf(stderr, "\n");
        return 1;
    }

    // Also require some non-center tap energy to have developed
    int nonzero = 0;
    for (int k = 1; k < st_adapt.num_taps; k++) {
        if (st_adapt.c_i[k] != 0 || st_adapt.c_q[k] != 0) {
            nonzero = 1;
            break;
        }
    }
    if (!nonzero) {
        fprintf(stderr, "EQ_ADAPT: no non-center tap adaptation observed\n");
        return 1;
    }
    return 0;
}
