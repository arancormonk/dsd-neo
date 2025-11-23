// SPDX-License-Identifier: GPL-3.0-or-later
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

#define DSP_FLL_BE_MAX_TAPS 129 /* 2*sps+1 with sps<=64 -> 129 */

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
    /* Band-edge FLL (CQPSK) state */
    float be_phase; /* radians */
    float be_freq;  /* radians/sample */
    float be_alpha;
    float be_beta;
    float be_max_freq;
    float be_min_freq;
    float be_loop_bw;
    float be_sps;
    float be_rolloff;
    int be_taps_len;
    int be_buf_idx;
    float be_taps_lower_r[DSP_FLL_BE_MAX_TAPS];
    float be_taps_lower_i[DSP_FLL_BE_MAX_TAPS];
    float be_taps_upper_r[DSP_FLL_BE_MAX_TAPS];
    float be_taps_upper_i[DSP_FLL_BE_MAX_TAPS];
    float be_buf_r[DSP_FLL_BE_MAX_TAPS * 2];
    float be_buf_i[DSP_FLL_BE_MAX_TAPS * 2];
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
 * @brief Band-edge FLL identical to GNU Radio's `fll_band_edge_cc`.
 *
 * Rotates the block in-place, runs the band-edge filters, and updates the
 * internal control loop using OP25's default parameters:
 *   - rolloff = 0.2
 *   - filter_size = 2*sps+1
 *   - loop bandwidth = 2*pi/sps/250
 *   - freq limits = +/-2*pi*(2/sps)
 *
 * @param config FLL configuration (gains, deadband, slew limit).
 * @param state  FLL state (updates freq_q15 and may advance phase_q15).
 * @param x      Input interleaved I/Q buffer.
 * @param N      Length of buffer in elements (must be even).
 * @param sps    Nominal samples-per-symbol (complex samples per symbol).
 */
void fll_update_error_qpsk(const fll_config_t* config, fll_state_t* state, int16_t* x, int N, int sps);

#ifdef __cplusplus
}
#endif

#endif /* DSP_FLL_H */
