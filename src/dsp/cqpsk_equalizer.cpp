// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdint.h>

/* Saturate 32-bit to 16-bit */
static inline int16_t
sat16_i32(int32_t x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (int16_t)x;
}

void
cqpsk_eq_init(cqpsk_eq_state_t* st) {
    if (!st) {
        return;
    }
    /* Identity response: y = x (complex 1.0 in Q14) */
    st->c0_i = 1 << 14;
    st->c0_q = 0;
    st->c1_i = 0;
    st->c1_q = 0;
    st->max_abs_q14 = (int16_t)(1 << 14);
    st->x1_i = 0;
    st->x1_q = 0;
    st->lms_enable = 0;
    st->mu_q15 = 1;        /* very small default step */
    st->update_stride = 4; /* update every 4 complex samples */
    st->update_count = 0;
    for (int i = 0; i < (int)(sizeof st->_rsvd / sizeof st->_rsvd[0]); i++) {
        st->_rsvd[i] = 0;
    }
}

void
cqpsk_eq_process_block(cqpsk_eq_state_t* st, int16_t* in_out, int len) {
    if (!st || !in_out || len < 2) {
        return;
    }
    /* Pass-through equalizer: multiply by 1.0 + j0.0 (Q14) */
    int16_t c0i = st->c0_i;
    int16_t c0q = st->c0_q;
    int16_t c1i = st->c1_i;
    int16_t c1q = st->c1_q;
    int16_t x1i = st->x1_i;
    int16_t x1q = st->x1_q;
    int maxc = st->max_abs_q14;
    int update_stride = (st->update_stride > 0) ? st->update_stride : 4;
    int mu = st->mu_q15; /* Q15 */

    for (int i = 0; i + 1 < len; i += 2) {
        int32_t xi = in_out[i];
        int32_t xq = in_out[i + 1];
        /* y = c0*x[n] + c1*x[n-1] */
        int32_t t0i = (xi * c0i - xq * c0q) >> 14;
        int32_t t0q = (xi * c0q + xq * c0i) >> 14;
        int32_t t1i = (x1i * c1i - x1q * c1q) >> 14;
        int32_t t1q = (x1i * c1q + x1q * c1i) >> 14;
        int32_t yi = t0i + t1i;
        int32_t yq = t0q + t1q;

        /* Write output */
        in_out[i] = sat16_i32(yi);
        in_out[i + 1] = sat16_i32(yq);

        /* Decision-directed LMS (experimental; disabled unless lms_enable=1) */
        if (st->lms_enable) {
            st->update_count++;
            if ((st->update_count % update_stride) == 0) {
                /* One-bit decision: target on the QPSK axes */
                int32_t di = (yi >= 0) ? (1 << 14) : -(1 << 14); /* Q14 */
                int32_t dq = (yq >= 0) ? (1 << 14) : -(1 << 14);
                /* Bring y into Q14 by saturating/limiting scale; treat samples as Q14 domain approx */
                int32_t yi_q14 = yi; /* assuming yi in roughly same dynamic range */
                int32_t yq_q14 = yq;
                /* Error e = d - y (Q14) */
                int32_t ei = di - yi_q14;
                int32_t eq = dq - yq_q14;
                /* Gradient for c0: e * conj(x) */
                int64_t g0i = (int64_t)ei * xi + (int64_t)eq * xq; /* real: ei*xi + eq*xq */
                int64_t g0q = (int64_t)eq * xi - (int64_t)ei * xq; /* imag: eq*xi - ei*xq */
                /* Gradient for c1: e * conj(x1) */
                int64_t g1i = (int64_t)ei * x1i + (int64_t)eq * x1q;
                int64_t g1q = (int64_t)eq * x1i - (int64_t)ei * x1q;
                /* Apply step size (Q15) and scale down to Q14 */
                g0i = (g0i * mu) >> 15; /* still in ~Q? large; reduce */
                g0q = (g0q * mu) >> 15;
                g1i = (g1i * mu) >> 15;
                g1q = (g1q * mu) >> 15;
                /* Additional shift to keep coefficients bounded */
                int32_t dc0i = (int32_t)(g0i >> 16);
                int32_t dc0q = (int32_t)(g0q >> 16);
                int32_t dc1i = (int32_t)(g1i >> 16);
                int32_t dc1q = (int32_t)(g1q >> 16);
                int32_t nc0i = c0i + dc0i;
                int32_t nc0q = c0q + dc0q;
                int32_t nc1i = c1i + dc1i;
                int32_t nc1q = c1q + dc1q;
                /* Clamp real/imag parts to +/- max_abs_q14 */
                if (nc0i > maxc) {
                    nc0i = maxc;
                }
                if (nc0i < -maxc) {
                    nc0i = -maxc;
                }
                if (nc0q > maxc) {
                    nc0q = maxc;
                }
                if (nc0q < -maxc) {
                    nc0q = -maxc;
                }
                if (nc1i > maxc) {
                    nc1i = maxc;
                }
                if (nc1i < -maxc) {
                    nc1i = -maxc;
                }
                if (nc1q > maxc) {
                    nc1q = maxc;
                }
                if (nc1q < -maxc) {
                    nc1q = -maxc;
                }
                c0i = (int16_t)nc0i;
                c0q = (int16_t)nc0q;
                c1i = (int16_t)nc1i;
                c1q = (int16_t)nc1q;
            }
        }

        /* Update delay line */
        x1i = (int16_t)xi;
        x1q = (int16_t)xq;
    }

    /* Persist state */
    st->c0_i = c0i;
    st->c0_q = c0q;
    st->c1_i = c1i;
    st->c1_q = c1q;
    st->x1_i = x1i;
    st->x1_q = x1q;
}
