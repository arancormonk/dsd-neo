// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief State shared by the OP25-compatible Gardner timing recovery.
 */

#ifndef DSP_TED_H
#define DSP_TED_H

#ifdef __cplusplus
extern "C" {
#endif

/* Delay line size for MMSE interpolation (matches OP25's NUM_COMPLEX) */
#define TED_DL_SIZE 100

/* TED State structure (native float for precision) - OP25 compatible */
typedef struct {
    float mu;        /* fractional sample phase [0.0, 1.0) */
    float omega;     /* current symbol period estimate (samples per symbol) */
    float omega_mid; /* nominal omega center */
    float omega_min; /* minimum omega (omega_mid * (1 - omega_rel)) */
    float omega_max; /* maximum omega (omega_mid * (1 + omega_rel)) */
    float omega_rel; /* relative omega limit for clipping (OP25: 0.002 = ±0.2%) */
    /* Last symbol sample for OP25 Gardner error computation */
    float last_r;
    float last_j;
    /* Smoothed Gardner error residual (EMA). Sign indicates persistent
       early/late bias; magnitude is relative (normalized by power). */
    float e_ema;
    /* Lock detector accumulator (Yair Linn method, like OP25) */
    float lock_accum;
    int lock_count;
    /* Event counter for lock statistics (reset on unmute transition, like OP25) */
    int event_count;
    /* Circular delay line for MMSE interpolation (OP25-style) */
    float dl[TED_DL_SIZE * 2 * 2]; /* interleaved I/Q, doubled for wrap-free access */
    int dl_index;                  /* current write position */
    int twice_sps;                 /* delay line wrap point: max(2*ceil(omega_max), ceil(omega_max/2)+9) */
    int sps;                       /* last initialized samples-per-symbol */
} ted_state_t;

/**
 * @brief Initialize TED state with default values.
 *
 * @param state TED state to initialize.
 */
void ted_init_state(ted_state_t* state);

/**
 * @brief Soft reset TED state, preserving mu and omega for phase continuity.
 *
 * Use this on frequency retunes where the transmitter symbol clock is consistent.
 * Avoids non-deterministic re-acquisition that occurs when mu resets to 0.
 *
 * @param state TED state to soft-reset.
 */
void ted_soft_reset(ted_state_t* state);

#ifdef __cplusplus
}
#endif

#endif /* DSP_TED_H */
