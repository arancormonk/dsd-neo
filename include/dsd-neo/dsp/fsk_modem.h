// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief FSK discriminator helper for complex RTL-family baseband.
 *
 * The helper consumes filtered complex baseband and emits centered PCM-like
 * discriminator samples. The core decoder's sample-domain symbolizer performs
 * all FSK symbol timing and slicing.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_FSK_MODEM_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_FSK_MODEM_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_fsk_modem_config {
    int sample_rate_hz;
    int symbol_rate_hz;
    int levels;          /* 2 or 4 */
    int channel_profile; /* DSD_CH_LPF_PROFILE_* value for diagnostics/config */
} dsd_fsk_modem_config;

typedef struct dsd_fsk_modem_state {
    dsd_fsk_modem_config cfg;
    float prev_i;
    float prev_q;
    int have_prev;
    float dc_est;
    float discriminator_peak_est;
} dsd_fsk_modem_state;

void dsd_fsk_modem_init(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg);
void dsd_fsk_modem_reset(dsd_fsk_modem_state* st);
void dsd_fsk_modem_configure(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg);
void dsd_fsk_modem_release(dsd_fsk_modem_state* st);
int dsd_fsk_modem_discriminator_process(dsd_fsk_modem_state* st, const float* iq_interleaved, int len_interleaved,
                                        float* out_samples, int max_samples);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_FSK_MODEM_H_ */
