// SPDX-License-Identifier: GPL-3.0-or-later
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

/* TED Configuration structure (GNU Radio-style native float) */
typedef struct {
    int enabled;
    int force;  /* allow forcing TED even for FM/C4FM paths */
    float gain; /* loop gain, typically 0.01..0.1 for stability */
    int sps;    /* nominal samples per symbol (e.g., 10 for 4800 sym/s at 48k) */
} ted_config_t;

/* TED State structure (native float for precision) */
typedef struct {
    float mu; /* fractional phase [0.0, 1.0) */
    /* Smoothed Gardner error residual (EMA). Sign indicates persistent
       early/late bias; magnitude is relative (normalized by power). */
    float e_ema;
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
void gardner_timing_adjust(const ted_config_t* config, ted_state_t* state, float* x, int* N, float* y);

/**
 * @brief Return the current smoothed TED residual (EMA of Gardner error).
 *
 * Positive values indicate a persistent "sample early" bias (center → right),
 * negative values indicate "sample late" (center → left). Zero means no bias
 * or TED disabled. Returns float for full precision.
 */
static inline float
ted_residual(const ted_state_t* s) {
    return s ? s->e_ema : 0.0f;
}

/**
 * @brief Return the current smoothed TED residual as integer (legacy compat).
 *
 * Scaled to roughly match old Q15 range for diagnostic displays.
 */
static inline int
ted_residual_int(const ted_state_t* s) {
    return s ? (int)(s->e_ema * 32768.0f) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DSP_TED_H */
