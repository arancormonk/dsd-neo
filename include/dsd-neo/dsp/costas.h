// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief QPSK Costas loop (carrier recovery) interface.
 *
 * Ports the GNU Radio `costas_loop_cc` control loop and phase detectors.
 * State is tracked in `dsd_costas_loop_state_t` and updated in-place on each block.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct demod_state; /* fwd decl */

typedef struct {
    float phase;
    float freq;
    float max_freq;
    float min_freq;
    float damping;
    float loop_bw;
    float alpha;
    float beta;
    float error;
    int initialized;
} dsd_costas_loop_state_t;

/* OP25-compatible defaults for CQPSK carrier recovery.
   OP25 uses loop_bw=0.008, damping=sqrt(2)/2, computed alpha/beta:
     denom = 1.0 + 2.0 * damping * loop_bw + loop_bw * loop_bw
     alpha = (4 * damping * loop_bw) / denom  ≈ 0.0221
     beta  = (4 * loop_bw * loop_bw) / denom  ≈ 0.000253
   Frequency limits: ±1.0 rad/sample
   Phase limits: ±π/2 (clamped, not wrapped) */

/** @brief OP25 Costas loop bandwidth. */
static inline float
dsd_neo_costas_default_loop_bw_op25(void) {
    return 0.008f;
}

/** @brief Default Costas loop alpha (phase gain) - computed from OP25 loop_bw/damping. */
static inline float
dsd_neo_costas_default_alpha(void) {
    /* loop_bw=0.008, damping=sqrt(2)/2
       denom = 1.0 + 2.0 * 0.7071 * 0.008 + 0.008^2 ≈ 1.01137
       alpha = (4 * 0.7071 * 0.008) / 1.01137 ≈ 0.0221 */
    return 0.0221f;
}

/** @brief Default Costas loop beta (frequency gain) - computed from OP25 loop_bw/damping. */
static inline float
dsd_neo_costas_default_beta(void) {
    /* beta = (4 * 0.008^2) / 1.01137 ≈ 0.000253 */
    return 0.000253f;
}

/** @brief Default max frequency (rad/sample) - OP25: ±1.0 rad/sample. */
static inline float
dsd_neo_costas_default_max_freq(void) {
    return 1.0f; /* OP25: COSTAS_MAX_FREQ = 1.0 rad/sample */
}

/** @brief Default Costas loop bandwidth (legacy, for non-CQPSK modes). */
static inline float
dsd_neo_costas_default_loop_bw(void) {
    return 6.28318530717958647692f / 100.0f;
}

/** @brief Default Costas loop damping factor (sqrt(2)/2). */
static inline float
dsd_neo_costas_default_damping(void) {
    return 0.70710678118654752440f; /* sqrt(2)/2 */
}

/**
 * @brief Reset Costas loop state for fresh carrier acquisition.
 *
 * Call this on channel retunes to clear stale phase/frequency estimates
 * from the previous channel. Without reset, the loop starts with the old
 * frequency offset and must slew to the new carrier, delaying sync acquisition.
 *
 * @param c Costas loop state to reset.
 */
void dsd_costas_reset(dsd_costas_loop_state_t* c);

/**
 * @brief OP25-compatible Gardner + Costas combined block.
 *
 * Direct port of OP25's gardner_costas_cc_impl::general_work().
 * Signal flow:
 *   1. Per input sample: NCO rotation, push to delay line
 *   2. Per output symbol: MMSE interpolation, Gardner TED, Costas phase tracking
 *   3. Output: RAW NCO-corrected symbols (NOT differential)
 *
 * The Gardner TED and Costas loop operate together in a single processing block,
 * exactly matching OP25's combined implementation. The NCO correction is applied
 * BEFORE the delay line, and phase error is computed from the internal diffdec.
 *
 * @param d Demodulator state. Input: lowpassed (sample-rate IQ after AGC).
 *          Output: lowpassed (symbol-rate NCO-corrected samples).
 */
void op25_gardner_costas_cc(struct demod_state* d);

/**
 * @brief External differential phasor decoder (matches GNU Radio diff_phasor_cc).
 *
 * Computes y[n] = x[n] * conj(x[n-1]) to produce differential phase output.
 * This is applied AFTER op25_gardner_costas_cc, matching OP25's Python flow:
 *   clock -> diffdec -> to_float -> rescale -> slicer
 *
 * @param d Demodulator state. Modifies lowpassed in-place to differential phasors.
 */
void op25_diff_phasor_cc(struct demod_state* d);

/**
 * @brief Legacy wrapper: calls op25_gardner_costas_cc then op25_diff_phasor_cc.
 *
 * Kept for API compatibility. New code should call the individual functions.
 *
 * @param d Demodulator state.
 */
void cqpsk_costas_diff_and_update(struct demod_state* d);

#ifdef __cplusplus
}
#endif
