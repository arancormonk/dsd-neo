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
 *
 * The output is the instantaneous frequency off the carrier on a fixed scale:
 * DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ counts per hertz at any sample rate, so
 * DSD_FSK_DISCRIMINATOR_REFERENCE_HZ reads DSD_FSK_DISCRIMINATOR_REFERENCE_COUNTS
 * and the PCM range (+/-32767) holds +/-6553 Hz; it is not clipped there. No
 * gain follows the signal: a sync word reads at the level of the frame behind
 * it however the channel sounded before the carrier came up.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_FSK_MODEM_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_FSK_MODEM_H_

#ifdef __cplusplus
extern "C" {
#endif

/** The deviation the scale is set by, which the PCM range (6553 Hz) clears: every FSK protocol on this path peaks below
    5 kHz with its pulse's overshoot (M17's root raised cosine and YSF's 2.7 kHz the widest of the digital ones, EDACS
    voice at most 5 kHz on 25 kHz channels), which leaves room for a carrier offset the centering has not yet taken
    out. */
#define DSD_FSK_DISCRIMINATOR_REFERENCE_HZ     6000.0f
#define DSD_FSK_DISCRIMINATOR_REFERENCE_COUNTS 30000.0f
#define DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ                                                                            \
    (DSD_FSK_DISCRIMINATOR_REFERENCE_COUNTS / DSD_FSK_DISCRIMINATOR_REFERENCE_HZ)

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
    float scale; /* counts per radian per sample: DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ at cfg.sample_rate_hz */
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
