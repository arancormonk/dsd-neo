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
    /* Default to 5 taps with identity center tap */
    st->num_taps = 5;
    if (st->num_taps > CQPSK_EQ_MAX_TAPS) {
        st->num_taps = CQPSK_EQ_MAX_TAPS;
    }
    for (int k = 0; k < CQPSK_EQ_MAX_TAPS; k++) {
        st->c_i[k] = 0;
        st->c_q[k] = 0;
        st->x_i[k] = 0;
        st->x_q[k] = 0;
    }
    /* Center tap at index 0 in circular perspective: we will treat head as most recent sample at index 0 */
    st->c_i[0] = (int16_t)(1 << 14);
    st->c_q[0] = 0;
    st->max_abs_q14 = (int16_t)(1 << 14);
    st->head = -1; /* will increment to 0 on first sample */
    st->lms_enable = 0;
    st->mu_q15 = 1;        /* very small default step */
    st->update_stride = 4; /* update every 4 complex samples */
    st->update_count = 0;
    st->eps_q15 = 4; /* small epsilon for NLMS normalization */
    for (int i = 0; i < (int)(sizeof st->_rsvd / sizeof st->_rsvd[0]); i++) {
        st->_rsvd[i] = 0;
    }
}

void
cqpsk_eq_process_block(cqpsk_eq_state_t* st, int16_t* in_out, int len) {
    if (!st || !in_out || len < 2) {
        return;
    }
    const int T = (st->num_taps > 0 && st->num_taps <= CQPSK_EQ_MAX_TAPS) ? st->num_taps : 1;
    const int maxc = st->max_abs_q14;
    const int update_stride = (st->update_stride > 0) ? st->update_stride : 4;
    const int mu = st->mu_q15;                           /* Q15 */
    const int eps = (st->eps_q15 > 0) ? st->eps_q15 : 4; /* Q15 */

    for (int i = 0; i + 1 < len; i += 2) {
        int32_t xi = in_out[i];
        int32_t xq = in_out[i + 1];

        /* Push into circular buffer: head advances, store newest at head */
        int h = st->head + 1;
        if (h >= T) {
            h = 0;
        }
        st->head = h;
        st->x_i[h] = (int16_t)xi;
        st->x_q[h] = (int16_t)xq;

        /* y = sum_k c[k] * x[n-k]; treat k=0 as current sample (head) */
        int64_t accI_q14 = 0;
        int64_t accQ_q14 = 0;
        int idx = h;
        for (int k = 0; k < T; k++) {
            int16_t ckI = st->c_i[k];
            int16_t ckQ = st->c_q[k];
            /* x index wraps backwards */
            if (idx < 0) {
                idx += T;
            }
            int16_t xkI = st->x_i[idx];
            int16_t xkQ = st->x_q[idx];
            /* Complex MAC: (ckI + j ckQ) * (xkI + j xkQ) in Q14 */
            accI_q14 += (int32_t)xkI * ckI - (int32_t)xkQ * ckQ;
            accQ_q14 += (int32_t)xkI * ckQ + (int32_t)xkQ * ckI;
            idx--;
        }
        /* Convert Q14 to Q0 for output with rounding */
        int32_t yI = (int32_t)((accI_q14 + (1 << 13)) >> 14);
        int32_t yQ = (int32_t)((accQ_q14 + (1 << 13)) >> 14);
        in_out[i] = sat16_i32(yI);
        in_out[i + 1] = sat16_i32(yQ);

        /* Decision-directed NLMS update (optional) */
        if (st->lms_enable) {
            st->update_count++;
            if ((st->update_count % update_stride) == 0) {
                /* Target decision on QPSK axes in Q14 */
                int32_t di = (accI_q14 >= 0) ? (1 << 14) : -(1 << 14);
                int32_t dq = (accQ_q14 >= 0) ? (1 << 14) : -(1 << 14);
                /* Error e = d - y (Q14) */
                int32_t ei = di - (int32_t)accI_q14; /* Q14 */
                int32_t eq = dq - (int32_t)accQ_q14; /* Q14 */

                /* Input power over window (Q0), add small bias to avoid stall */
                uint64_t pwr = 0;
                for (int k = 0, j = h; k < T; k++) {
                    if (j < 0) {
                        j += T;
                    }
                    int32_t xr = st->x_i[j];
                    int32_t xj = st->x_q[j];
                    pwr += (uint64_t)xr * (uint64_t)xr + (uint64_t)xj * (uint64_t)xj;
                    j--;
                }
                /* Build denominator in Q15: eps_q15 + (pwr >> 8) to keep scale sane */
                uint32_t denom_q15 = (uint32_t)eps + (uint32_t)(pwr >> 8);
                if (denom_q15 == 0) {
                    denom_q15 = eps;
                }

                /* Update each tap: c_k += (mu * e * conj(x_k)) / denom */
                int j = h;
                for (int k = 0; k < T; k++) {
                    if (j < 0) {
                        j += T;
                    }
                    int32_t xr = st->x_i[j];
                    int32_t xj2 = st->x_q[j];
                    /* Real/imag grad in Q14: ei*xr + eq*xj , eq*xr - ei*xj */
                    int64_t gi_q14 = (int64_t)ei * xr + (int64_t)eq * xj2;
                    int64_t gq_q14 = (int64_t)eq * xr - (int64_t)ei * xj2;
                    /* Apply mu (Q15): -> Q29 */
                    int64_t gi_q29 = gi_q14 * (int64_t)mu;
                    int64_t gq_q29 = gq_q14 * (int64_t)mu;
                    /* Normalize by denom_q15 (Q15). Keep guard shift to control step: */
                    int32_t dci = (int32_t)(gi_q29 / (int64_t)denom_q15); /* ~Q14 after div */
                    int32_t dcq = (int32_t)(gq_q29 / (int64_t)denom_q15);
                    /* Additional gentle scaling to keep stable at small mu */
                    dci >>= 8;
                    dcq >>= 8;
                    int32_t nci = (int32_t)st->c_i[k] + dci;
                    int32_t ncq = (int32_t)st->c_q[k] + dcq;
                    if (nci > maxc) {
                        nci = maxc;
                    }
                    if (nci < -maxc) {
                        nci = -maxc;
                    }
                    if (ncq > maxc) {
                        ncq = maxc;
                    }
                    if (ncq < -maxc) {
                        ncq = -maxc;
                    }
                    st->c_i[k] = (int16_t)nci;
                    st->c_q[k] = (int16_t)ncq;
                    j--;
                }
            }
        }
    }
}
