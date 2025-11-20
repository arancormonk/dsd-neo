// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstddef>
#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

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

/* Ring buffer of recent equalized symbol outputs is per-instance (st->sym_xy). */
static const int kSymMax = CQPSK_EQ_SYM_MAX;

static inline void
sym_ring_append_q0(cqpsk_eq_state_t* st, int32_t yI_q0, int32_t yQ_q0) {
    int h = st->sym_head;
    st->sym_xy[(size_t)(h << 1) + 0] = sat16_i32(yI_q0);
    st->sym_xy[(size_t)(h << 1) + 1] = sat16_i32(yQ_q0);
    h++;
    if (h >= kSymMax) {
        h = 0;
    }
    st->sym_head = h;
    /* Increase valid length up to capacity */
    if (st->sym_len < kSymMax) {
        st->sym_len++;
    }
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
    st->err_ema_q14 = 0;
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

    /* WL stability defaults */
    st->wl_leak_shift = 12;         /* ~1/4096 per update */
    st->wl_gate_thr_q15 = 655;      /* ~0.02 impropriety ratio */
    st->wl_mu_q15 = 1;              /* smaller than FFE by default */
    st->wl_improp_ema_q15 = 0;      /* start at 0 */
    st->wl_improp_alpha_q15 = 8192; /* ~0.25 */
    st->wl_x2_re_ema = 0;
    st->wl_x2_im_ema = 0;
    st->wl_p2_ema = 0;
    st->wl_stat_alpha_q15 = 8192; /* default to ~0.25 */
    /* Decoupled adaptation defaults */
    st->adapt_mode = 0; /* start adapting FFE */
    st->adapt_hold = 0;
    st->adapt_min_hold = 64;                            /* hold ~64 update ticks by default */
    st->wl_thr_off_q15 = (st->wl_gate_thr_q15 * 3) / 4; /* 25% hysteresis */
    /* symbol ring */
    st->sym_head = 0;
    st->sym_len = 0;
    for (int i = 0; i < kSymMax * 2; i++) {
        st->sym_xy[i] = 0;
    }

    /* Optional environment overrides for WL gating */
    const char* env;
    env = getenv("DSD_NEO_WL_GATE_THR_Q15");
    if (env) {
        int v = (int)strtol(env, NULL, 0);
        if (v < 0) {
            v = 0;
        }
        if (v > 32767) {
            v = 32767;
        }
        st->wl_gate_thr_q15 = v;
    }
    env = getenv("DSD_NEO_WL_THR_OFF_Q15");
    if (env) {
        int v = (int)strtol(env, NULL, 0);
        if (v < 0) {
            v = 0;
        }
        if (v > 32767) {
            v = 32767;
        }
        st->wl_thr_off_q15 = v;
    }
    env = getenv("DSD_NEO_WL_IMPROP_ALPHA_Q15");
    if (env) {
        int v = (int)strtol(env, NULL, 0);
        if (v < 0) {
            v = 0;
        }
        if (v > 32767) {
            v = 32767;
        }
        st->wl_improp_alpha_q15 = v;
    }
    env = getenv("DSD_NEO_WL_STAT_ALPHA_Q15");
    if (env) {
        int v = (int)strtol(env, NULL, 0);
        if (v < 0) {
            v = 0;
        }
        if (v > 32767) {
            v = 32767;
        }
        st->wl_stat_alpha_q15 = v;
    }
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
    st->err_ema_q14 = 0;
    st->sym_count = 0;
    /* Preserve CMA warmup budget; do not force to zero so a reset does not skip pre-training.
       Only clamp negatives to zero to avoid underflow. */
    if (st->cma_warmup < 0) {
        st->cma_warmup = 0;
    }
    /* Clear symbol ring validity; next appends will repopulate */
    st->sym_len = 0;
    st->wl_x2_re_ema = 0;
    st->wl_x2_im_ema = 0;
    st->wl_p2_ema = 0;
    st->wl_improp_ema_q15 = 0;
    st->adapt_mode = 0;
    st->adapt_hold = 0;
    st->have_last_sym = 0;
    st->last_y_i_q14 = 0;
    st->last_y_q_q14 = 0;
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
                /* Proper WL branch: y += W * conj(x), with W = cwI + j*cwQ */
                int16_t cwI = st->cw_i[k];
                int16_t cwQ = st->cw_q[k];
                /* Re += a*xr + b*xq;  Im += b*xr - a*xq */
                accI_q14 += (int32_t)xkI * cwI + (int32_t)xkQ * cwQ;
                accQ_q14 += (int32_t)xkI * cwQ - (int32_t)xkQ * cwI;
            }
            idx--;
        }

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

        /* Decision feedback subtraction (align scales: Q14*Q14 -> Q28, shift to Q14).
           Apply continuously; decisions are only refreshed on symbol ticks. */
        if (st->dfe_enable && st->dfe_taps > 0) {
            for (int k = 0; k < st->dfe_taps && k < 4; k++) {
                int16_t br = st->b_i[k];
                int16_t bj = st->b_q[k];
                int32_t dr = st->d_i[k];
                int32_t dj = st->d_q[k];
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

        /* Build desired symbol decision 'd' (use sym_tick for DQPSK mode).
           Use constant-amplitude slicer target to stabilize DD updates. */
        int32_t di, dq;
        {
            const int32_t A = (st->max_abs_q14 > 0) ? st->max_abs_q14 : (1 << 14);

            if (st->dqpsk_decision && st->have_last_sym && sym_tick) {
                /* Differential decision: z = y * conj(y_prev) in Q14 */
                int32_t lyi = st->last_y_i_q14;
                int32_t lyq = st->last_y_q_q14;
                int32_t zr_q14 = (int32_t)(((int64_t)accI_q14 * lyi + (int64_t)accQ_q14 * lyq) >> 14);
                int32_t zq_q14 = (int32_t)(((int64_t)accQ_q14 * lyi - (int64_t)accI_q14 * lyq) >> 14);
                /* Constant-amplitude decision in differential domain */
                int32_t dzr = (zr_q14 >= 0) ? A : -A;
                int32_t dzq = (zq_q14 >= 0) ? A : -A;
                /* Normalize previous symbol to constant amplitude to avoid amplitude blow-up */
                int32_t rprev = (lyi >= 0 ? lyi : -lyi);
                int32_t tmp2 = (lyq >= 0 ? lyq : -lyq);
                if (tmp2 > rprev) {
                    rprev = tmp2;
                }
                if (rprev < (1 << 8)) {
                    rprev = (1 << 8);
                }
                int32_t nlyi = (int32_t)(((int64_t)lyi * A) / (int64_t)rprev);
                int32_t nlyq = (int32_t)(((int64_t)lyq * A) / (int64_t)rprev);
                /* Rotate back: d = dz * (normalized y_prev) */
                int64_t dri = (int64_t)dzr * nlyi - (int64_t)dzq * nlyq;
                int64_t drq = (int64_t)dzr * nlyq + (int64_t)dzq * nlyi;
                di = (int32_t)(dri >> 14);
                dq = (int32_t)(drq >> 14);
            } else {
                /* Axis-aligned constant-amplitude slicer */
                di = (accI_q14 >= 0) ? A : -A;
                dq = (accQ_q14 >= 0) ? A : -A;
            }
        }

        /* Shift-in decision history for DFE only at symbol ticks */
        if (st->dfe_enable && st->dfe_taps > 0 && sym_tick) {
            for (int k = st->dfe_taps - 1; k > 0; k--) {
                st->d_i[k] = st->d_i[k - 1];
                st->d_q[k] = st->d_q[k - 1];
            }
            st->d_i[0] = di;
            st->d_q[0] = dq;
        }

        /* CMA warmup pre-training */
        if (st->cma_warmup > 0) {
            int64_t yI_q14 = accI_q14;
            int64_t yQ_q14 = accQ_q14;
            /* Avoid overflow when squaring large outputs: scale down until |y| fits a safe bound,
               and scale the CMA target accordingly so the update remains proportional. */
            int scale = 0;
            const int kMaxMagBits = 30; /* keep |y| < 2^30 before squaring (>> 8 gives plenty of headroom) */
            while (scale < 8) {
                int64_t ayI = (yI_q14 >= 0) ? yI_q14 : -yI_q14;
                int64_t ayQ = (yQ_q14 >= 0) ? yQ_q14 : -yQ_q14;
                if (ayI <= (1LL << kMaxMagBits) && ayQ <= (1LL << kMaxMagBits)) {
                    break;
                }
                yI_q14 >>= 1;
                yQ_q14 >>= 1;
                scale++;
            }
            int64_t mag2_q28 = yI_q14 * yI_q14 + yQ_q14 * yQ_q14; /* Q(28 - 2*scale) */
            int64_t R_q28 = (int64_t)1 << 28;                     /* target modulus^2 */
            R_q28 >>= (scale * 2);
            if (R_q28 < 1) {
                R_q28 = 1;
            }
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
            /* During CMA warmup, freeze WL adaptation but gently leak WL toward zero at symbol ticks */
            if (st->wl_enable && sym_tick) {
                int leak = (st->wl_leak_shift > 0) ? st->wl_leak_shift : 12;
                for (int k = 0; k < T; k++) {
                    int32_t wi = st->cw_i[k];
                    int32_t wq = st->cw_q[k];
                    wi -= (wi >> leak);
                    wq -= (wq >> leak);
                    int maxc_loc = st->max_abs_q14;
                    if (wi > maxc_loc) {
                        wi = maxc_loc;
                    }
                    if (wi < -maxc_loc) {
                        wi = -maxc_loc;
                    }
                    if (wq > maxc_loc) {
                        wq = maxc_loc;
                    }
                    if (wq < -maxc_loc) {
                        wq = -maxc_loc;
                    }
                    st->cw_i[k] = (int16_t)wi;
                    st->cw_q[k] = (int16_t)wq;
                }
            }
            /* Force adaptation mode to FFE during CMA */
            st->adapt_mode = 0;
            st->adapt_hold = (st->adapt_min_hold > 0) ? st->adapt_min_hold : 32;
            continue; /* skip DD update this sample */
        }

        /* Decision-directed NLMS update (optional). Gate to symbol ticks for DD stability. */
        if (st->lms_enable) {
            st->update_count++;
            if ((st->update_count % update_stride) == 0 && sym_tick) {
                /* Error e = d - y (Q14), with d scaled to current output radius */
                int32_t ei = di - (int32_t)accI_q14; /* Q14 */
                int32_t eq = dq - (int32_t)accQ_q14; /* Q14 */

                /* Update diagnostic EMA of |e| (cheap L1 approx): err_ema += alpha*(|e|-ema) */
                {
                    int ai = (ei >= 0) ? ei : -ei;
                    int aq = (eq >= 0) ? eq : -eq;
                    int mag = ai + aq; /* simple proxy for |e| */
                    int ema = st->err_ema_q14;
                    /* alpha ~ 1/32 via >>5 */
                    int delta = mag - ema;
                    ema += (delta >> 5);
                    if (ema < 0) {
                        ema = 0;
                    }
                    st->err_ema_q14 = ema;
                }

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

                /* WL impropriety gate: EMA(|E[x^2]|/E[|x|^2]) vs threshold to avoid rapid toggling */
                int allow_wl_update = 0;
                if (st->wl_enable) {
                    /* Running impropriety stats decoupled from tap/window length */
                    int32_t xr0 = st->x_i[h];
                    int32_t xj0 = st->x_q[h];
                    int64_t x2r = (int64_t)xr0 * xr0 - (int64_t)xj0 * xj0; /* Re{x^2} */
                    int64_t x2i = (int64_t)2 * xr0 * xj0;                  /* Im{x^2} */
                    int64_t p2 = (int64_t)xr0 * xr0 + (int64_t)xj0 * xj0;  /* |x|^2 */
                    int stat_alpha = (st->wl_stat_alpha_q15 > 0) ? st->wl_stat_alpha_q15 : st->wl_improp_alpha_q15;
                    /* EMA update: s <- s + alpha * (v - s) >> 15 */
                    int64_t dre = x2r - (int64_t)st->wl_x2_re_ema;
                    int64_t dim = x2i - (int64_t)st->wl_x2_im_ema;
                    int64_t dp2 = p2 - (int64_t)st->wl_p2_ema;
                    int64_t are = (dre * stat_alpha) >> 15;
                    int64_t aim = (dim * stat_alpha) >> 15;
                    int64_t ap2 = (dp2 * stat_alpha) >> 15;
                    int64_t new_re = (int64_t)st->wl_x2_re_ema + are;
                    int64_t new_im = (int64_t)st->wl_x2_im_ema + aim;
                    int64_t new_p2 = (int64_t)st->wl_p2_ema + ap2;
                    if (new_re > INT32_MAX) {
                        new_re = INT32_MAX;
                    }
                    if (new_re < INT32_MIN) {
                        new_re = INT32_MIN;
                    }
                    if (new_im > INT32_MAX) {
                        new_im = INT32_MAX;
                    }
                    if (new_im < INT32_MIN) {
                        new_im = INT32_MIN;
                    }
                    if (new_p2 > INT32_MAX) {
                        new_p2 = INT32_MAX;
                    }
                    if (new_p2 < 1) {
                        new_p2 = 1;
                    }
                    st->wl_x2_re_ema = (int)new_re;
                    st->wl_x2_im_ema = (int)new_im;
                    st->wl_p2_ema = (int)new_p2;

                    double sx2_re = (double)st->wl_x2_re_ema;
                    double sx2_im = (double)st->wl_x2_im_ema;
                    double sp2 = (double)st->wl_p2_ema;
                    if (sp2 < 1.0) {
                        sp2 = 1.0;
                    }
                    double improp = sqrt(sx2_re * sx2_re + sx2_im * sx2_im) / sp2;
                    int meas_q15 = (int)(improp * 32768.0 + 0.5);
                    if (meas_q15 < 0) {
                        meas_q15 = 0;
                    }
                    if (meas_q15 > 32767) {
                        meas_q15 = 32767;
                    }
                    int ema = st->wl_improp_ema_q15;
                    int alpha = st->wl_improp_alpha_q15;
                    int delta = meas_q15 - ema;
                    int adj = (int)(((int64_t)alpha * (int64_t)delta) >> 15);
                    ema += adj;
                    if (ema < 0) {
                        ema = 0;
                    }
                    if (ema > 32767) {
                        ema = 32767;
                    }
                    st->wl_improp_ema_q15 = ema;
                    /* Mode switching with hysteresis and minimum hold time */
                    if (st->adapt_hold > 0) {
                        st->adapt_hold--;
                    }
                    int thr_on = st->wl_gate_thr_q15;
                    int thr_off = (st->wl_thr_off_q15 > 0) ? st->wl_thr_off_q15 : ((st->wl_gate_thr_q15 * 3) / 4);
                    if (st->adapt_hold == 0) {
                        if (st->adapt_mode == 0) {
                            if (ema >= thr_on) {
                                st->adapt_mode = 1; /* switch to WL */
                                st->adapt_hold = (st->adapt_min_hold > 0) ? st->adapt_min_hold : 32;
                            }
                        } else {
                            if (ema < thr_off) {
                                st->adapt_mode = 0; /* switch to FFE */
                                st->adapt_hold = (st->adapt_min_hold > 0) ? st->adapt_min_hold : 32;
                            }
                        }
                    }
                    allow_wl_update = (st->adapt_mode == 1);
                }

                /* Update each tap: c_k += (mu * e * conj(x_k)) / denom */
                int j = h;
                for (int k = 0; k < T; k++) {
                    if (j < 0) {
                        j += T;
                    }
                    int32_t xr = st->x_i[j];
                    int32_t xj2 = st->x_q[j];
                    /* Real/imag grad: scale e (Q14) by x (Q0) and renormalize to Q14 */
                    int64_t gi_q14 = ((int64_t)ei * xr + (int64_t)eq * xj2) >> 14;
                    int64_t gq_q14 = ((int64_t)eq * xr - (int64_t)ei * xj2) >> 14;
                    /* Apply mu (Q15): -> Q29 */
                    int64_t gi_q29 = gi_q14 * (int64_t)mu;
                    int64_t gq_q29 = gq_q14 * (int64_t)mu;
                    /* Normalize by denom_q15 (Q15). Result is ~Q14. */
                    int32_t dci = (int32_t)(gi_q29 / (int64_t)denom_q15); /* ~Q14 after div */
                    int32_t dcq = (int32_t)(gq_q29 / (int64_t)denom_q15);
                    /* Additional guard scaling (conservative) */
                    int ffe_shift = 6;
                    dci >>= ffe_shift;
                    dcq >>= ffe_shift;
                    /* NLMS: w <- w + mu * e * conj(x) / (||x||^2 + eps) */
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
                    /* Apply FFE update only when FFE mode is active */
                    if (st->adapt_mode == 0) {
                        st->c_i[k] = (int16_t)nci;
                        st->c_q[k] = (int16_t)ncq;
                    }
                    if (st->wl_enable && allow_wl_update) {
                        /* conj-branch update: e * x_k, renormalized to Q14 */
                        int64_t giw_q14 = ((int64_t)ei * xr - (int64_t)eq * xj2) >> 14;
                        int64_t gqw_q14 = ((int64_t)eq * xr + (int64_t)ei * xj2) >> 14;
                        int wl_mu = (st->wl_mu_q15 > 0) ? st->wl_mu_q15 : 1;
                        int64_t giw_q29 = giw_q14 * (int64_t)wl_mu;
                        int64_t gqw_q29 = gqw_q14 * (int64_t)wl_mu;
                        int32_t dwi = (int32_t)(giw_q29 / (int64_t)denom_q15);
                        int32_t dwq = (int32_t)(gqw_q29 / (int64_t)denom_q15);
                        dwi >>= 3;
                        dwq >>= 3;
                        int32_t nwi = (int32_t)st->cw_i[k] + dwi;
                        int32_t nwq = (int32_t)st->cw_q[k] + dwq;
                        /* Stronger cap for WL taps: keep within ~1/8 of max */
                        int wl_cap = maxc >> 3;
                        if (wl_cap < 1) {
                            wl_cap = 1;
                        }
                        if (nwi > wl_cap) {
                            nwi = wl_cap;
                        }
                        if (nwi < -wl_cap) {
                            nwi = -wl_cap;
                        }
                        if (nwq > wl_cap) {
                            nwq = wl_cap;
                        }
                        if (nwq < -wl_cap) {
                            nwq = -wl_cap;
                        }
                        st->cw_i[k] = (int16_t)nwi;
                        st->cw_q[k] = (int16_t)nwq;
                    } else if (st->wl_enable) {
                        /* Apply small leakage toward zero when WL is disabled by gate */
                        int leak = (st->wl_leak_shift > 0) ? st->wl_leak_shift : 12;
                        int32_t wi = st->cw_i[k];
                        int32_t wq = st->cw_q[k];
                        wi -= (wi >> leak);
                        wq -= (wq >> leak);
                        int wl_cap = maxc >> 3;
                        if (wl_cap < 1) {
                            wl_cap = 1;
                        }
                        if (wi > wl_cap) {
                            wi = wl_cap;
                        }
                        if (wi < -wl_cap) {
                            wi = -wl_cap;
                        }
                        if (wq > wl_cap) {
                            wq = wl_cap;
                        }
                        if (wq < -wl_cap) {
                            wq = -wl_cap;
                        }
                        st->cw_i[k] = (int16_t)wi;
                        st->cw_q[k] = (int16_t)wq;
                    }
                    j--;
                }
                /* DFE update against decision history */
                if (st->dfe_enable && st->dfe_taps > 0 && sym_tick && st->adapt_mode == 0) {
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
        } else if (st->wl_enable && sym_tick) {
            /* LMS disabled: still apply a tiny WL leakage at symbol ticks to keep WL near zero */
            int leak = (st->wl_leak_shift > 0) ? st->wl_leak_shift : 12;
            for (int k = 0; k < T; k++) {
                int32_t wi = st->cw_i[k];
                int32_t wq = st->cw_q[k];
                wi -= (wi >> leak);
                wq -= (wq >> leak);
                int maxc = st->max_abs_q14;
                if (wi > maxc) {
                    wi = maxc;
                }
                if (wi < -maxc) {
                    wi = -maxc;
                }
                if (wq > maxc) {
                    wq = maxc;
                }
                if (wq < -maxc) {
                    wq = -maxc;
                }
                st->cw_i[k] = (int16_t)wi;
                st->cw_q[k] = (int16_t)wq;
            }
        }

        /* Update last symbol output at symbol ticks */
        if (sym_tick) {
            st->last_y_i_q14 = (int32_t)accI_q14;
            st->last_y_q_q14 = (int32_t)accQ_q14;
            st->have_last_sym = 1;
            /* Append equalized symbol (Q0) to ring for EVM/SNR */
            sym_ring_append_q0(st, yI, yQ);
        }
    }
}

extern "C" int
cqpsk_eq_get_symbols(const cqpsk_eq_state_t* st, int16_t* out_xy, int max_pairs) {
    if (!out_xy || max_pairs <= 0) {
        return 0;
    }
    if (!st) {
        return 0;
    }
    /* Snapshot current head/len to avoid torn reads */
    int head = st->sym_head;
    int len = st->sym_len;
    if (len <= 0) {
        return 0;
    }
    if (len > kSymMax) {
        len = kSymMax;
    }
    int n = (max_pairs < len) ? max_pairs : len;
    /* Oldest element is (head - len) modulo kSymMax */
    int start = head - len;
    while (start < 0) {
        start += kSymMax;
    }
    for (int k = 0; k < n; k++) {
        int idx = start + k;
        if (idx >= kSymMax) {
            idx -= kSymMax;
        }
        out_xy[(size_t)(k << 1) + 0] = st->sym_xy[(size_t)(idx << 1) + 0];
        out_xy[(size_t)(k << 1) + 1] = st->sym_xy[(size_t)(idx << 1) + 1];
    }
    return n;
}
