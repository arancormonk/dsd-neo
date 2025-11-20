// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * QPSK Costas loop (carrier recovery) interface.
 *
 * This module ports the GNU Radio costas_loop_cc control loop and phase
 * detectors. State is tracked in dsd_costas_loop_state_t and updated in-place
 * on each block.
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
static inline float
dsd_neo_costas_default_loop_bw(void) {
    return 6.28318530717958647692f / 800.0f; /* ~2*pi/800 */
}

static inline float
dsd_neo_costas_default_damping(void) {
    return 0.70710678118654752440f; /* sqrt(2)/2 */
}

/* Run Costas loop rotation + loop update on interleaved I/Q in-place. */
void cqpsk_costas_mix_and_update(struct demod_state* d);

#ifdef __cplusplus
}
#endif
