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
   OP25 uses alpha=0.04, beta=0.125*alpha^2=0.0002, fmax=2400Hz.
   These are applied directly rather than computed from loop_bw/damping. */

/** @brief Default Costas loop alpha (phase gain) - OP25 default. */
static inline float
dsd_neo_costas_default_alpha(void) {
    return 0.04f;
}

/** @brief Default Costas loop beta (frequency gain) - OP25: 0.125 * alpha^2. */
static inline float
dsd_neo_costas_default_beta(void) {
    return 0.125f * 0.04f * 0.04f; /* 0.0002 */
}

/** @brief Default max frequency (rad/sample) - OP25: 2400 Hz @ 24 kHz. */
static inline float
dsd_neo_costas_default_max_freq(void) {
    return 6.28318530717958647692f * 2400.0f / 24000.0f; /* ~0.628 rad/sample */
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
 * @brief Combined Costas NCO + differential decode + loop update with per-sample feedback.
 *
 * OP25-style: Performs NCO rotation, differential decoding, phase error detection,
 * and loop update in a single per-sample loop. This maintains proper PLL feedback
 * where each sample's correction is applied before processing the next sample.
 *
 * Expects raw IQ samples in lowpassed buffer. Output is differential phasors
 * ready for phase extraction.
 *
 * @param d Demodulator state (modifies lowpassed in-place to diff phasors).
 */
void cqpsk_costas_diff_and_update(struct demod_state* d);

#ifdef __cplusplus
}
#endif
