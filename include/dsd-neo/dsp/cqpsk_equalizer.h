// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

/*
 * Decision-directed CQPSK equalizer interface (fractionally-spaced, NLMS).
 *
 * Lightweight, fixed-point implementation intended to mitigate moderate ISI
 * on P25 LSM/CQPSK paths. Uses a short feed-forward FIR with complex taps
 * (Q14) and normalized LMS updates every N samples. Defaults to 5 taps and
 * tiny step size for stability; configurable via runtime/env.
 */

#ifdef __cplusplus
extern "C" {
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
    int16_t eps_q15;   /* NLMS epsilon in Q15 to avoid div-by-zero (~1..8) */
    /* Reserved for future growth */
    int16_t _rsvd[4];
} cqpsk_eq_state_t;

/* Initialize equalizer state with identity response. */
void cqpsk_eq_init(cqpsk_eq_state_t* st);

/*
 * Apply equalizer to interleaved I/Q samples in-place.
 * in_out: pointer to interleaved I/Q samples
 * len:    number of elements in interleaved array (must be even)
 */
void cqpsk_eq_process_block(cqpsk_eq_state_t* st, int16_t* in_out, int len);

#ifdef __cplusplus
}
#endif
