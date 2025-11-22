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
    float noise;
    int order;
    int use_snr;
    int initialized;
} dsd_costas_loop_state_t;

/* Defaults tuned for lower-bandwidth DSP than upstream GNU Radio usage. */
/** @brief Default Costas loop bandwidth (~2*pi/800). */
static inline float
dsd_neo_costas_default_loop_bw(void) {
    return 6.28318530717958647692f / 800.0f; /* ~2*pi/800 */
}

/** @brief Default Costas loop damping factor (sqrt(2)/2). */
static inline float
dsd_neo_costas_default_damping(void) {
    return 0.70710678118654752440f; /* sqrt(2)/2 */
}

/**
 * @brief Run Costas loop rotation and loop update on interleaved I/Q in-place.
 *
 * Uses demod_state configuration to select detector order and toggles.
 */
void cqpsk_costas_mix_and_update(struct demod_state* d);

#ifdef __cplusplus
}
#endif
