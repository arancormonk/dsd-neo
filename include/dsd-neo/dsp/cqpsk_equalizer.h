// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

/*
 * Decision-directed CQPSK equalizer interface (fractionally-spaced, NLMS).
 *
 * Lightweight, fixed-point implementation intended to mitigate moderate ISI
 * on P25 CQPSK paths. Uses a short feed-forward FIR with complex taps
 * (Q14) and normalized LMS updates every N samples. Defaults to 5 taps and
 * tiny step size for stability; configurable via runtime/env.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Symbol ring buffer size for recent equalized outputs */
#ifndef CQPSK_EQ_SYM_MAX
#define CQPSK_EQ_SYM_MAX 2048
#endif

/* Max taps for the short equalizer. Must be odd, up to 11. */
#ifndef CQPSK_EQ_MAX_TAPS
#define CQPSK_EQ_MAX_TAPS 11
#endif

typedef struct cqpsk_eq_state_s {
    /* Complex FIR taps (Q14), 0..num_taps-1 */
    int16_t c_i[CQPSK_EQ_MAX_TAPS];
    int16_t c_q[CQPSK_EQ_MAX_TAPS];
    int num_taps;        /* odd number of taps in use (1..CQPSK_EQ_MAX_TAPS) */
    int16_t max_abs_q14; /* clamp for coefficient magnitude */
    /* Circular buffer of recent complex input samples */
    int16_t x_i[CQPSK_EQ_MAX_TAPS];
    int16_t x_q[CQPSK_EQ_MAX_TAPS];
    int head; /* index of most recent sample in circular buffer */
    /* NLMS parameters */
    int lms_enable;    /* 0 off (default), 1 on */
    int16_t mu_q15;    /* small step size (e.g., 1..128) */
    int update_stride; /* apply update every N complex samples (e.g., 4) */
    int update_count;  /* internal counter */
    int err_ema_q14;   /* EMA of |e| magnitude in Q14 (diagnostic) */
    int16_t eps_q15;   /* NLMS epsilon in Q15 to avoid div-by-zero (~1..8) */
    /* Symbol gating for DFE decisions/updates (approximate SPS) */
    int sym_stride; /* advance decision history every sym_stride samples */
    int sym_count;  /* internal counter for symbol gating */
    /* Decision-Feedback Equalizer (DFE) small branch (feedback taps) */
    int dfe_enable; /* enable feedback branch */
    int dfe_taps;   /* number of feedback taps (0..4) */
    int16_t b_i[4]; /* feedback taps (Q14) real */
    int16_t b_q[4]; /* feedback taps (Q14) imag */
    /* Short history of past decisions (sliced symbols) for feedback */
    int32_t d_i[4];
    int32_t d_q[4];
    /* Widely-linear augmentation (conjugate branch) */
    int wl_enable;                   /* include conj(x) taps when set */
    int16_t cw_i[CQPSK_EQ_MAX_TAPS]; /* conj taps real */
    int16_t cw_q[CQPSK_EQ_MAX_TAPS]; /* conj taps imag */
    /* CMA warmup (blind pre-training) */
    int cma_warmup;     /* number of samples to run CMA updates before DD */
    int16_t cma_mu_q15; /* CMA step (tiny) */

    /* Optional DQPSK-aware decision mode */
    int dqpsk_decision;   /* 0=axis-aligned (default), 1=DQPSK decision */
    int have_last_sym;    /* whether previous symbol output is valid */
    int32_t last_y_i_q14; /* previous symbol output (Q14) */
    int32_t last_y_q_q14; /* previous symbol output (Q14) */

    /* WL stability helpers */
    int wl_leak_shift;       /* leakage shift for WL taps (e.g., 12 => ~1/4096 per update) */
    int wl_gate_thr_q15;     /* impropriety gate threshold in Q15 for |E[x^2]|/E[|x|^2] (e.g., 0.02->~655) */
    int wl_mu_q15;           /* WL step size (Q15), separate from FFE mu */
    int wl_improp_ema_q15;   /* EMA of impropriety ratio in Q15 */
    int wl_improp_alpha_q15; /* EMA alpha in Q15 (e.g., 8192 ~ 0.25) */
    /* Running statistics for impropriety, decoupled from tap/window length */
    int wl_x2_re_ema;      /* EMA of Re{E[x^2]} in Q0 */
    int wl_x2_im_ema;      /* EMA of Im{E[x^2]} in Q0 */
    int wl_p2_ema;         /* EMA of E[|x|^2] in Q0 */
    int wl_stat_alpha_q15; /* EMA alpha for running stats (Q15) */
    /* Phase decoupling between FFE and WL */
    int adapt_mode;     /* 0 = FFE adapting, 1 = WL adapting */
    int adapt_hold;     /* countdown ticks before a mode switch is allowed */
    int adapt_min_hold; /* min ticks to hold a mode once switched */
    int wl_thr_off_q15; /* WL off threshold (hysteresis), Q15 */

    /* Recent equalized symbols (Q0), captured at symbol ticks */
    int16_t sym_xy[CQPSK_EQ_SYM_MAX * 2];
    int sym_head; /* ring head in pairs [0..CQPSK_EQ_SYM_MAX-1]; next write index */
    int sym_len;  /* number of valid pairs currently stored (0..CQPSK_EQ_SYM_MAX) */
} cqpsk_eq_state_t;

/* Initialize equalizer state with identity response. */
void cqpsk_eq_init(cqpsk_eq_state_t* st);

/* Reset only runtime history/counters; keep taps/flags. */
void cqpsk_eq_reset_runtime(cqpsk_eq_state_t* st);

/* Reset DFE branch taps and decision history to zero (safe enable). */
void cqpsk_eq_reset_dfe(cqpsk_eq_state_t* st);

/* Reset WL (conjugate) branch taps to zero. */
void cqpsk_eq_reset_wl(cqpsk_eq_state_t* st);

/* Full reset: taps to identity, WL/DFE cleared, histories/counters cleared. */
void cqpsk_eq_reset_all(cqpsk_eq_state_t* st);

/*
 * Apply equalizer to interleaved I/Q samples in-place.
 * in_out: pointer to interleaved I/Q samples
 * len:    number of elements in interleaved array (must be even)
 */
void cqpsk_eq_process_block(cqpsk_eq_state_t* st, int16_t* in_out, int len);

/*
 * Retrieve recent equalized symbol outputs captured at symbol ticks.
 * Copies up to max_pairs complex samples (interleaved I,Q in Q0 int16) into out_xy.
 * Returns the number of pairs copied (0 if unavailable).
 */
int cqpsk_eq_get_symbols(const cqpsk_eq_state_t* st, int16_t* out_xy, int max_pairs);

#ifdef __cplusplus
}
#endif
