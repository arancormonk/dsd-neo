// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdint.h>

static inline int64_t
i64abs(int64_t v) {
    return (v < 0) ? -v : v;
}

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
    st->eps_q15 = 4;    /* small epsilon for NLMS normalization */
    st->sym_stride = 4; /* default approximate SPS; refined by path init */
    st->sym_count = 0;
    /* DFE defaults */
    st->dfe_enable = 0;
    st->dfe_taps = 0;
    for (int i = 0; i < 4; i++) {
        st->b_i[i] = 0;
        st->b_q[i] = 0;
        st->d_i[i] = 0;
        st->d_q[i] = 0;
    }
    /* Widely-linear defaults */
    st->wl_enable = 0;
    for (int k = 0; k < CQPSK_EQ_MAX_TAPS; k++) {
        st->cw_i[k] = 0;
        st->cw_q[k] = 0;
    }
    /* CMA warmup */
    st->cma_warmup = 0; /* disabled by default */
    st->cma_mu_q15 = 1; /* tiny default */
    /* DQPSK decision defaults */
    st->dqpsk_decision = 0;
    st->have_last_sym = 0;
    st->last_y_i_q14 = 0;
    st->last_y_q_q14 = 0;
}

void
cqpsk_eq_reset_dfe(cqpsk_eq_state_t* st) {
    if (!st) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        st->b_i[i] = 0;
        st->b_q[i] = 0;
        st->d_i[i] = 0;
        st->d_q[i] = 0;
    }
}

void
cqpsk_eq_reset_runtime(cqpsk_eq_state_t* st) {
    if (!st) {
        return;
    }
    /* Clear circular history and decision buffers, counters */
    for (int k = 0; k < CQPSK_EQ_MAX_TAPS; k++) {
        st->x_i[k] = 0;
        st->x_q[k] = 0;
    }
    for (int i = 0; i < 4; i++) {
        st->d_i[i] = 0;
        st->d_q[i] = 0;
    }
    st->head = -1;
    st->update_count = 0;
    st->sym_count = 0;
    st->cma_warmup = 0;
}

void
cqpsk_eq_reset_wl(cqpsk_eq_state_t* st) {
    if (!st) {
        return;
    }
    for (int k = 0; k < CQPSK_EQ_MAX_TAPS; k++) {
        st->cw_i[k] = 0;
        st->cw_q[k] = 0;
    }
}

void
cqpsk_eq_reset_all(cqpsk_eq_state_t* st) {
    if (!st) {
        return;
    }
    /* Keep current num_taps/max_abs, but reset taps to identity */
    for (int k = 0; k < CQPSK_EQ_MAX_TAPS; k++) {
        st->c_i[k] = 0;
        st->c_q[k] = 0;
        st->cw_i[k] = 0;
        st->cw_q[k] = 0;
    }
    st->c_i[0] = (int16_t)(1 << 14);
    st->c_q[0] = 0;
    cqpsk_eq_reset_dfe(st);
    cqpsk_eq_reset_runtime(st);
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

        /* y = sum_k c[k] * x[n-k]; with optional conj branch and DFE */
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
            if (st->wl_enable) {
                /* conj branch: cw * conj(x) */
                int16_t cwI = st->cw_i[k];
                int16_t cwQ = st->cw_q[k];
                accI_q14 += (int32_t)xkI * cwI + (int32_t)xkQ * cwQ;
                accQ_q14 += -(int32_t)xkI * cwQ + (int32_t)xkQ * cwI;
            }
            idx--;
        }
        /* Decision feedback subtraction (align scales: Q14*Q14 -> Q28, shift to Q14) */
        if (st->dfe_enable && st->dfe_taps > 0) {
            for (int k = 0; k < st->dfe_taps && k < 4; k++) {
                int16_t br = st->b_i[k];
                int16_t bj = st->b_q[k];
                int16_t dr = st->d_i[k];
                int16_t dj = st->d_q[k];
                int32_t ri = ((int32_t)dr * br - (int32_t)dj * bj) >> 14; /* Q14 */
                int32_t rq = ((int32_t)dr * bj + (int32_t)dj * br) >> 14; /* Q14 */
                accI_q14 -= ri;
                accQ_q14 -= rq;
            }
        }
        /* Convert Q14 to Q0 for output with rounding */
        int32_t yI = (int32_t)((accI_q14 + (1 << 13)) >> 14);
        int32_t yQ = (int32_t)((accQ_q14 + (1 << 13)) >> 14);
        in_out[i] = sat16_i32(yI);
        in_out[i + 1] = sat16_i32(yQ);

        /* Determine approximate symbol tick for DFE decisions/updates */
        int sym_stride = (st->sym_stride > 0) ? st->sym_stride : st->update_stride;
        if (sym_stride <= 0) {
            sym_stride = 4;
        }
        st->sym_count++;
        int sym_tick = 0;
        if (st->sym_count >= sym_stride) {
            st->sym_count = 0;
            sym_tick = 1;
        }

        /* Build desired symbol decision 'd' (use sym_tick for DQPSK mode) */
        int32_t di, dq;
        {
            int32_t rad_q14 = (int32_t)i64abs(accI_q14);
            int32_t tmp_q14 = (int32_t)i64abs(accQ_q14);
            if (tmp_q14 > rad_q14) {
                rad_q14 = tmp_q14;
            }
            if (rad_q14 < (1 << 12)) {
                rad_q14 = (1 << 12);
            }
            if (rad_q14 > (1 << 20)) {
                rad_q14 = (1 << 20);
            }

            if (st->dqpsk_decision && st->have_last_sym && sym_tick) {
                /* Differential decision: z = y * conj(y_prev) in Q14 */
                int32_t lyi = st->last_y_i_q14;
                int32_t lyq = st->last_y_q_q14;
                int32_t zr_q14 = (int32_t)(((int64_t)accI_q14 * lyi + (int64_t)accQ_q14 * lyq) >> 14);
                int32_t zq_q14 = (int32_t)(((int64_t)accQ_q14 * lyi - (int64_t)accI_q14 * lyq) >> 14);
                int32_t radz_q14 = (zr_q14 >= 0 ? zr_q14 : -zr_q14);
                int32_t tmp2 = (zq_q14 >= 0 ? zq_q14 : -zq_q14);
                if (tmp2 > radz_q14) {
                    radz_q14 = tmp2;
                }
                if (radz_q14 < (1 << 12)) {
                    radz_q14 = (1 << 12);
                }
                if (radz_q14 > (1 << 20)) {
                    radz_q14 = (1 << 20);
                }
                int32_t dzr = (zr_q14 >= 0) ? radz_q14 : -radz_q14;
                int32_t dzq = (zq_q14 >= 0) ? radz_q14 : -radz_q14;
                /* Rotate back: d = dz * y_prev (conj^-1)
                   (dzr + j dzq) * (lyi + j lyq) */
                int64_t dri = (int64_t)dzr * lyi - (int64_t)dzq * lyq;
                int64_t drq = (int64_t)dzr * lyq + (int64_t)dzq * lyi;
                di = (int32_t)(dri >> 14);
                dq = (int32_t)(drq >> 14);
            } else {
                di = (accI_q14 >= 0) ? rad_q14 : -rad_q14;
                dq = (accQ_q14 >= 0) ? rad_q14 : -rad_q14;
            }
        }

        /* Shift-in decision history for DFE only at symbol ticks */
        if (st->dfe_enable && st->dfe_taps > 0 && sym_tick) {
            for (int k = st->dfe_taps - 1; k > 0; k--) {
                st->d_i[k] = st->d_i[k - 1];
                st->d_q[k] = st->d_q[k - 1];
            }
            st->d_i[0] = (int16_t)di;
            st->d_q[0] = (int16_t)dq;
        }

        /* CMA warmup pre-training */
        if (st->cma_warmup > 0) {
            int64_t yI_q14 = accI_q14;
            int64_t yQ_q14 = accQ_q14;
            int64_t mag2_q28 = yI_q14 * yI_q14 + yQ_q14 * yQ_q14; /* Q28 */
            int64_t R_q28 = (int64_t)1 << 28;                     /* target modulus^2 */
            int64_t diff_q28 = mag2_q28 - R_q28;
            /* Update FFE taps using CMA gradient */
            for (int k = 0, j = h; k < T; k++) {
                if (j < 0) {
                    j += T;
                }
                int32_t xr = st->x_i[j];
                int32_t xj2 = st->x_q[j];
                /* grad ~ diff * y * conj(x) */
                int64_t gi = ((diff_q28 >> 10) * ((yI_q14 * xr + yQ_q14 * xj2) >> 14)) >> 15;
                int64_t gq = ((diff_q28 >> 10) * ((yQ_q14 * xr - yI_q14 * xj2) >> 14)) >> 15;
                int32_t dci = (int32_t)((gi * st->cma_mu_q15) >> 20);
                int32_t dcq = (int32_t)((gq * st->cma_mu_q15) >> 20);
                int32_t nci = (int32_t)st->c_i[k] - dci;
                int32_t ncq = (int32_t)st->c_q[k] - dcq;
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
            st->cma_warmup--;
            continue; /* skip DD update this sample */
        }

        /* Decision-directed NLMS update (optional). Gate to symbol ticks for DD stability. */
        if (st->lms_enable) {
            st->update_count++;
            if ((st->update_count % update_stride) == 0 && sym_tick) {
                /* Error e = d - y (Q14), with d scaled to current output radius */
                int32_t ei = di - (int32_t)accI_q14; /* Q14 */
                int32_t eq = dq - (int32_t)accQ_q14; /* Q14 */

                /* Input power over window (Q0), add small bias to avoid stall;
                   also guard very low-energy segments to prevent runaway updates. */
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
                if (pwr < (uint64_t)T * 64) {
                    /* Too little energy, skip update */
                    continue;
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
                    if (st->wl_enable) {
                        /* conj-branch update: e * x_k */
                        int64_t giw_q14 = (int64_t)ei * xr - (int64_t)eq * xj2;
                        int64_t gqw_q14 = (int64_t)eq * xr + (int64_t)ei * xj2;
                        int64_t giw_q29 = giw_q14 * (int64_t)mu;
                        int64_t gqw_q29 = gqw_q14 * (int64_t)mu;
                        int32_t dwi = (int32_t)(giw_q29 / (int64_t)denom_q15);
                        int32_t dwq = (int32_t)(gqw_q29 / (int64_t)denom_q15);
                        dwi >>= 8;
                        dwq >>= 8;
                        int32_t nwi = (int32_t)st->cw_i[k] + dwi;
                        int32_t nwq = (int32_t)st->cw_q[k] + dwq;
                        if (nwi > maxc) {
                            nwi = maxc;
                        }
                        if (nwi < -maxc) {
                            nwi = -maxc;
                        }
                        if (nwq > maxc) {
                            nwq = maxc;
                        }
                        if (nwq < -maxc) {
                            nwq = -maxc;
                        }
                        st->cw_i[k] = (int16_t)nwi;
                        st->cw_q[k] = (int16_t)nwq;
                    }
                    j--;
                }
                /* DFE update against decision history */
                if (st->dfe_enable && st->dfe_taps > 0 && sym_tick) {
                    /*
             * DFE branch uses y = y_ff - b^H d_past. With this sign convention,
             * the LMS update for b must be the negative of the FFE rule:
             *    b <- b - mu * e * conj(d_past)
             * Using the wrong sign causes positive feedback and gain blow-up.
                     */
                    for (int k = 0; k < st->dfe_taps && k < 4; k++) {
                        int32_t dr = st->d_i[k];
                        int32_t dj = st->d_q[k];
                        int64_t gbi_q14 = (int64_t)ei * dr + (int64_t)eq * dj; /* Re{e * conj(d)} */
                        int64_t gbq_q14 = (int64_t)eq * dr - (int64_t)ei * dj; /* Im{e * conj(d)} */
                        int64_t gbi_q29 = gbi_q14 * (int64_t)mu;
                        int64_t gbq_q29 = gbq_q14 * (int64_t)mu;
                        int32_t dbi = (int32_t)(gbi_q29 / (int64_t)denom_q15);
                        int32_t dbq = (int32_t)(gbq_q29 / (int64_t)denom_q15);
                        dbi >>= 9;
                        dbq >>= 9;
                        /* Correct sign for DFE: subtract the gradient */
                        int32_t nbi = (int32_t)st->b_i[k] - dbi;
                        int32_t nbq = (int32_t)st->b_q[k] - dbq;
                        if (nbi > maxc) {
                            nbi = maxc;
                        }
                        if (nbi < -maxc) {
                            nbi = -maxc;
                        }
                        if (nbq > maxc) {
                            nbq = maxc;
                        }
                        if (nbq < -maxc) {
                            nbq = -maxc;
                        }
                        st->b_i[k] = (int16_t)nbi;
                        st->b_q[k] = (int16_t)nbq;
                    }
                }

                /* Gentle center-tap bias toward unity to stabilize overall gain.
                   This acts like a tiny leakage pulling c[0] -> 1 + j0 over time. */
                {
                    const int bias_shift = 9; /* ~1/512 per update at symbol ticks */
                    int32_t target_i = (1 << 14);
                    int32_t target_q = 0;
                    int32_t c0i = st->c_i[0];
                    int32_t c0q = st->c_q[0];
                    int32_t di0 = (target_i - c0i) >> bias_shift;
                    int32_t dq0 = (target_q - c0q) >> bias_shift;
                    int32_t n0i = c0i + di0;
                    int32_t n0q = c0q + dq0;
                    if (n0i > maxc) {
                        n0i = maxc;
                    }
                    if (n0i < -maxc) {
                        n0i = -maxc;
                    }
                    if (n0q > maxc) {
                        n0q = maxc;
                    }
                    if (n0q < -maxc) {
                        n0q = -maxc;
                    }
                    st->c_i[0] = (int16_t)n0i;
                    st->c_q[0] = (int16_t)n0q;
                }
            }
        }

        /* Update last symbol output at symbol ticks */
        if (sym_tick) {
            st->last_y_i_q14 = (int16_t)accI_q14;
            st->last_y_q_q14 = (int16_t)accQ_q14;
            st->have_last_sym = 1;
        }
    }
}
