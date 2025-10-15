// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Timing Error Detector (TED) interface: Gardner TED and fractional
 * delay timing correction for symbol synchronization in digital demodulation modes.
 */

#ifndef DSP_TED_H
#define DSP_TED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TED Configuration structure */
typedef struct {
    int enabled;
    int force;    /* allow forcing TED even for FM/C4FM paths */
    int gain_q20; /* small gain (Q20) for stability */
    int sps;      /* nominal samples per symbol (e.g., 10 for 4800 sym/s at 48k) */
} ted_config_t;

/* TED State structure - minimal fields needed for TED operations */
typedef struct {
    int mu_q20; /* fractional phase [0,1) in Q20 */
    /* Smoothed Gardner error residual (EMA, coarse units). Sign indicates
       persistent early/late bias; magnitude is relative and not normalized. */
    int e_ema;
} ted_state_t;

/**
 * @brief Initialize TED state with default values.
 *
 * @param state TED state to initialize.
 */
void ted_init_state(ted_state_t* state);

/**
 * @brief Lightweight Gardner timing correction.
 *
 * Uses a cubic Farrow (cubic convolution) fractional-delay interpolator
 * around the nominal samples-per-symbol to reduce timing error; intended
 * for digital modes when enabled.
 *
 * @param config TED configuration.
 * @param state  TED state (updates mu_q20).
 * @param x      Input/output I/Q buffer (modified in-place if timing adjustment applied).
 * @param N      Length of buffer (must be even; updated if timing adjustment applied).
 * @param y      Work buffer for timing-adjusted I/Q (must be at least size N).
 * @note Skips processing when samples-per-symbol is large unless forced.
 */
void gardner_timing_adjust(const ted_config_t* config, ted_state_t* state, int16_t* x, int* N, int16_t* y);

/**
 * @brief Return the current smoothed TED residual (EMA of Gardner error).
 *
 * Positive values indicate a persistent "sample early" bias (center → right),
 * negative values indicate "sample late" (center → left). Zero means no bias
 * or TED disabled.
 */
static inline int
ted_residual(const ted_state_t* s) {
    return s ? s->e_ema : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DSP_TED_H */
