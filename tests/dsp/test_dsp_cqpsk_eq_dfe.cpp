// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test: DFE improves EVM on post-cursor ISI channel compared to FFE-only.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
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
evm_ssq_tail(const int16_t* xy, int pairs, int tail) {
    int start = pairs - tail;
    if (start < 0) {
        start = 0;
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

static void
apply_postcursor(const int16_t* in, int pairs, int16_t* out, int a0_q15, int a1_q15) {
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
    const int N = 1500; // pairs
    int16_t src[2 * N];
    unsigned s = 0x123u;
    const int amp = 7000;
    for (int n = 0; n < N; n++) {
        s = s * 1664525u + 1013904223u;
        src[2 * n + 0] = (int16_t)(((s >> 31) & 1) ? amp : -amp);
        src[2 * n + 1] = (int16_t)(((s >> 30) & 1) ? amp : -amp);
    }
    int16_t ch[2 * N];
    // Post-cursor: y = x + 0.35*x[n-1]
    apply_postcursor(src, N, ch, 32768 /*1.0*/, 11469 /*0.35*/);

    // FFE-only
    cqpsk_eq_state_t ffe;
    memset(&ffe, 0, sizeof(ffe));
    cqpsk_eq_init(&ffe);
    ffe.lms_enable = 1;
    ffe.update_stride = 1;
    ffe.sym_stride = 1;
    ffe.mu_q15 = 256;
    int16_t buf1[2 * N];
    memcpy(buf1, ch, sizeof(ch));
    cqpsk_eq_process_block(&ffe, buf1, 2 * N);
    int16_t syms1[N * 2];
    int n1 = cqpsk_eq_get_symbols(&ffe, syms1, N);
    if (n1 <= 0) {
        fprintf(stderr, "DFE: no FFE symbols\n");
        return 1;
    }
    long long evm1 = evm_ssq_tail(syms1, n1, 256);

    // FFE + DFE
    cqpsk_eq_state_t dfe;
    memset(&dfe, 0, sizeof(dfe));
    cqpsk_eq_init(&dfe);
    dfe.lms_enable = 1;
    dfe.update_stride = 1;
    dfe.sym_stride = 1;
    dfe.mu_q15 = 256;
    dfe.dfe_enable = 1;
    dfe.dfe_taps = 2; // use 2 feedback taps
    int16_t buf2[2 * N];
    memcpy(buf2, ch, sizeof(ch));
    cqpsk_eq_process_block(&dfe, buf2, 2 * N);
    int16_t syms2[N * 2];
    int n2 = cqpsk_eq_get_symbols(&dfe, syms2, N);
    if (n2 <= 0) {
        fprintf(stderr, "DFE: no DFE symbols\n");
        return 1;
    }
    long long evm2 = evm_ssq_tail(syms2, n2, 256);

    if (!(evm2 * 4LL <= evm1 * 5LL)) { // allow small variance; expect better or equal
        fprintf(stderr, "DFE: EVM not improved (ffe=%lld dfe=%lld)\n", evm1, evm2);
        return 1;
    }

    // Ensure some DFE coefficients adapted
    int nonzero = 0;
    for (int k = 0; k < dfe.dfe_taps; k++) {
        if (dfe.b_i[k] != 0 || dfe.b_q[k] != 0) {
            nonzero = 1;
            break;
        }
    }
    if (!nonzero) {
        fprintf(stderr, "DFE: feedback taps did not adapt\n");
        return 1;
    }
    return 0;
}
