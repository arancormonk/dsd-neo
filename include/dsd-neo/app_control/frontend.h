// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-facing app-control metrics API.
 *
 * This boundary exposes plain metric values without leaking live
 * dsd_opts/dsd_state pointers or IO-layer RTL types.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_

#include <dsd-neo/core/input_level.h>
#include <stdint.h>
#include "dsd-neo/platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_frontend_decode_health {
    int valid;
    uint32_t generation;
    unsigned int p25p1_fec_ok;
    unsigned int p25p1_fec_err;
    unsigned int p25p2_facch_ok;
    unsigned int p25p2_facch_err;
    unsigned int p25p2_sacch_ok;
    unsigned int p25p2_sacch_err;
    unsigned int p25p2_voice_err;
} dsd_frontend_decode_health;

typedef struct dsd_frontend_costas_metrics {
    int err_smooth_avg_q14;
    int err_raw_avg_q14;
    int confidence_avg_q14;
    int zero_conf_pct;
} dsd_frontend_costas_metrics;

typedef enum DSD_ATTR_PACKED {
    DSD_FRONTEND_RTL_OUTPUT_AUDIO_MONITOR = 0,
    DSD_FRONTEND_RTL_OUTPUT_FSK_DISCRIMINATOR = 1,
    DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK = 2
} dsd_frontend_rtl_output_kind;

typedef struct dsd_frontend_metrics {
    unsigned int output_rate_hz;
    int output_kind;
    int symbol_rate_hz;
    int symbol_levels;
    int channel_profile;
    uint32_t stream_generation;
    int stream_active;
    dsd_input_level_snapshot input_level;

    int cqpsk_enable;
    int cqpsk_timing_active;
    int cqpsk_timing_bias;
    double snr_bias_evm;
    double snr_bias_c4fm;
    double snr_c4fm_db;
    double snr_c4fm_eye_db;
    double snr_cqpsk_db;
    double snr_gfsk_db;
    double snr_gfsk_eye_db;
    double snr_qpsk_const_db;

    int iq_balance;
    int iq_dc_enabled;
    int iq_dc_shift_k;
    int ted_sps;
    float ted_gain;
    double cfo_hz;
    int carrier_lock;
    int costas_err_q14;
    int nco_q15;
    int demod_rate_hz;
    double fll_band_edge_freq_hz;
    dsd_frontend_costas_metrics costas;

    int spectrum_size;
    int requested_ppm;
    int tuner_gain_tenth_db;
    int tuner_gain_is_auto;
    int tuner_gain_valid;
    int auto_ppm_enabled;
    int auto_ppm_locked;
    int auto_ppm_locked_ppm;
    int auto_ppm_step_dir;
    double auto_ppm_snr_db;
    double auto_ppm_df_hz;
    int tuner_autogain;
    dsd_frontend_decode_health decode_health;
} dsd_frontend_metrics;

enum {
    DSD_FRONTEND_SNR_FALLBACK_C4FM_EYE = 1u << 0,
    DSD_FRONTEND_SNR_FALLBACK_GFSK_EYE = 1u << 1,
    DSD_FRONTEND_SNR_FALLBACK_QPSK_CONST = 1u << 2,
    DSD_FRONTEND_SNR_FALLBACK_ALL =
        DSD_FRONTEND_SNR_FALLBACK_C4FM_EYE | DSD_FRONTEND_SNR_FALLBACK_GFSK_EYE | DSD_FRONTEND_SNR_FALLBACK_QPSK_CONST
};

int dsd_app_frontend_get_metrics(dsd_frontend_metrics* out);
int dsd_app_frontend_get_metrics_with_snr_fallbacks(dsd_frontend_metrics* out, unsigned int snr_fallbacks);

int dsd_app_frontend_constellation_get(float* out_xy, int max_points);
int dsd_app_frontend_eye_get(float* out, int max_samples, int* out_sps);
int dsd_app_frontend_spectrum_get(float* out_db, int max_bins, int* out_rate);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_ */
