// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Adaptive equalizers for CQPSK/H-DQPSK symbol streams.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Keep the CQPSK equalizer small and deterministic: it runs once per recovered
 * symbol, so long filters directly add per-channel CPU cost and convergence
 * time.
 */
#define DSD_CQPSK_CMA_EQ_MAX_TAPS 15

typedef struct {
    float taps_r[DSD_CQPSK_CMA_EQ_MAX_TAPS];
    float taps_i[DSD_CQPSK_CMA_EQ_MAX_TAPS];
    float hist_r[DSD_CQPSK_CMA_EQ_MAX_TAPS];
    float hist_i[DSD_CQPSK_CMA_EQ_MAX_TAPS];
    int taps;
    int filled;
    int initialized;
    unsigned int symbols;
    float err_ema;
    float mag2_ema;
} dsd_cqpsk_cma_equalizer_state_t;

typedef struct {
    int initialized;
    int taps;
    unsigned int symbols;
    float err_ema;
    float mag2_ema;
    float tap_energy;
    float center_tap_mag;
    float max_side_tap_mag;
} dsd_cqpsk_cma_equalizer_metrics_t;

void dsd_cqpsk_cma_equalizer_init(dsd_cqpsk_cma_equalizer_state_t* st, int taps);
void dsd_cqpsk_cma_equalizer_reset(dsd_cqpsk_cma_equalizer_state_t* st, int taps);
void dsd_cqpsk_cma_equalizer_apply(dsd_cqpsk_cma_equalizer_state_t* st, float* iq, int len, int taps, float step,
                                   float modulus);
void dsd_cqpsk_cma_equalizer_get_metrics(const dsd_cqpsk_cma_equalizer_state_t* st,
                                         dsd_cqpsk_cma_equalizer_metrics_t* out);

#ifdef __cplusplus
}
#endif
