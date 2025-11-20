// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Public API for Frequency-Locked Loop (FLL) utilities.
 *
 * Provides state and configuration structures and routines to perform
 * NCO-based mixing and frequency-error control suitable for FM demodulation.
 */

#ifndef DSP_FLL_H
#define DSP_FLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FLL Configuration structure */
typedef struct {
    int enabled;
    int alpha_q15;    /* proportional gain (Q15) */
    int beta_q15;     /* integral gain (Q15) */
    int deadband_q14; /* ignore small phase errors |err| <= deadband (Q14) */
    int slew_max_q15; /* max |delta freq| per update (Q15) */
} fll_config_t;

/* FLL State structure - minimal fields needed for FLL operations */
typedef struct {
    int freq_q15;  /* NCO frequency increment (Q15 radians/sample scaled) */
    int phase_q15; /* NCO phase accumulator (wrap at 2*pi -> 1<<15 scale) */
    int prev_r;
    int prev_j;
    int int_q15; /* PI integrator state (Q15), bounded for anti-windup */
    /* Small history of trailing complex samples for symbol-spaced updates. */
    int prev_hist_r[64];
    int prev_hist_j[64];
    int prev_hist_len; /* number of valid samples in prev_hist_* (0..64) */
} fll_state_t;

/**
 * @brief Initialize FLL state with default values.
 *
 * @param state FLL state to initialize.
 */
void fll_init_state(fll_state_t* state);

/**
 * @brief Mix I/Q by an NCO and advance phase by freq_q15 per sample.
 *
 * Phase and frequency are Q15 where a full turn (2*pi) maps to 1<<15.
 * Uses high-quality sin/cos from the math library for rotation.
 *
 * @param config FLL configuration.
 * @param state  FLL state (updates phase_q15).
 * @param x      Input/output interleaved I/Q buffer (modified in-place).
 * @param N      Length of buffer in samples (must be even).
 */
void fll_mix_and_update(const fll_config_t* config, fll_state_t* state, int16_t* x, int N);

/**
 * @brief Estimate frequency error and update FLL control (PI in Q15).
 *
 * Uses a phase-difference discriminator to compute average error.
 * Applies proportional and integral actions to adjust the NCO frequency.
 *
 * @param config FLL configuration.
 * @param state  FLL state (updates freq_q15 and may advance phase_q15).
 * @param x      Input interleaved I/Q buffer.
 * @param N      Length of buffer in samples (must be even).
 */
void fll_update_error(const fll_config_t* config, fll_state_t* state, const int16_t* x, int N);

/**
 * @brief Estimate frequency error for QPSK carriers using symbol-spaced phase
 *        differences, then update the FLL control (PI in Q15).
 *
 * Computes the angle of s[k] * conj(s[k - sps]) across the block and averages
 * the result to form a frequency error estimate that is robust to QPSK symbol
 * transitions. The integrator and proportional terms are applied to the NCO
 * frequency increment with slew limiting.
 *
 * @param config FLL configuration (gains, deadband, slew limit).
 * @param state  FLL state (updates freq_q15 and may advance phase_q15).
 * @param x      Input interleaved I/Q buffer.
 * @param N      Length of buffer in elements (must be even).
 * @param sps    Nominal samples-per-symbol (complex samples per symbol).
 */
void fll_update_error_qpsk(const fll_config_t* config, fll_state_t* state, const int16_t* x, int N, int sps);

#ifdef __cplusplus
}
#endif

#endif /* DSP_FLL_H */
