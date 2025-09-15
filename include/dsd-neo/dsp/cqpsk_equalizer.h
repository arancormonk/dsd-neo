// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

/*
 * Minimal, lightweight decision-directed CQPSK equalizer interface.
 *
 * This interface purposely keeps state opaque and small. The initial
 * implementation acts as a pass-through while scaffolding is validated;
 * subsequent iterations can add LMS updates and taps without changing the
 * public API.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cqpsk_eq_state_s {
    /* Two complex taps (I/Q pairs) for a short FIR equalizer.
       Fixed-point Q14 coefficients; initial pass-through uses {1, 0}. */
    int16_t c0_i, c0_q; /* center tap (complex) */
    int16_t c1_i, c1_q; /* side tap (complex), initially 0 */
    /* Simple clamp for LMS updates (magnitude limit) */
    int16_t max_abs_q14;
    /* Last input sample for side-tap (I/Q) */
    int16_t x1_i, x1_q;
    /* LMS parameters */
    int lms_enable;    /* 0 off (default), 1 on (experimental) */
    int16_t mu_q15;    /* small step size (e.g. 1..8) */
    int update_stride; /* apply update every N complex samples (e.g., 4) */
    int update_count;  /* internal counter */
    /* Reserved for future history/bias tracking */
    int16_t _rsvd[8];
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
