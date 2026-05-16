// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Symbol-rate FSK modem for complex RTL-family baseband.
 *
 * The modem consumes filtered complex baseband and emits one normalized float
 * per recovered symbol. It performs FM phase-difference detection internally;
 * callers never see or route a discriminator PCM stream for digital decode.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_fsk_modem_config {
    int sample_rate_hz;
    int symbol_rate_hz;
    int levels;          /* 2 or 4 */
    int channel_profile; /* DSD_CH_LPF_PROFILE_* value for diagnostics/config */
} dsd_fsk_modem_config;

#define DSD_FSK_MODEM_ACQ_MAX_SAMPLES        2048
#define DSD_FSK_MODEM_PENDING_INLINE_SYMBOLS 2048

typedef struct dsd_fsk_modem_state {
    dsd_fsk_modem_config cfg;
    float prev_i;
    float prev_q;
    int have_prev;
    float symbol_clock;
    float symbol_phase;
    float symbol_accum;
    int symbol_count;
    float dc_est;
    float abs_est;
    float last_symbol;
    uint64_t symbols_emitted;
    int timing_acquired;
    int acq_len;
    float acq_freq[DSD_FSK_MODEM_ACQ_MAX_SAMPLES];
    int pending_pos;
    int pending_len;
    int pending_cap;
    float* pending_heap;
    float pending_symbols[DSD_FSK_MODEM_PENDING_INLINE_SYMBOLS];
} dsd_fsk_modem_state;

void dsd_fsk_modem_init(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg);
void dsd_fsk_modem_reset(dsd_fsk_modem_state* st);
void dsd_fsk_modem_configure(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg);
void dsd_fsk_modem_release(dsd_fsk_modem_state* st);
int dsd_fsk_modem_process(dsd_fsk_modem_state* st, const float* iq_interleaved, int len_interleaved, float* out_symbols,
                          int max_symbols);
int dsd_fsk_modem_zero_symbols(dsd_fsk_modem_state* st, int input_complex_samples, float* out_symbols, int max_symbols);

#ifdef __cplusplus
}
#endif
