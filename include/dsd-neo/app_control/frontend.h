// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-facing app-control status and metrics API.
 *
 * This boundary exposes copied status and plain metric values to frontends
 * without leaking live dsd_opts/dsd_state pointers or IO-layer RTL types.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_

#include <dsd-neo/app_control/frontend_types.h>
#include <dsd-neo/core/input_level.h>
#include <stdint.h>
#include "dsd-neo/platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_frontend_status {
    dsd_frontend_kind frontend_kind;
    dsd_frontend_common_display_opts display;
    dsd_frontend_terminal_display_opts terminal_display;

    int audio_in_type;
    int audio_out_type;
    int audio_out;
    char audio_in_dev[2048];
    char audio_out_dev[1024];
    char pa_input_idx[100];
    char pa_output_idx[100];
    char tcp_hostname[1024];
    int tcp_portno;
    char udp_hostname[1024];
    int udp_portno;
    char udp_in_bindaddr[256];
    int udp_in_portno;
    char rigctlhostname[1024];
    int rigctlportno;
    int use_rigctl;

    int config_autosave_enabled;
    char config_autosave_path[2048];

    int payload_logging;
    int event_log_enabled;
    char event_log_path[1024];
    int per_call_wav_enabled;
    int per_call_wav_active;
    int static_wav_enabled;
    int static_wav_active;
    char wav_out_dir[512];
    char wav_out_file[1024];
    char wav_out_file_raw[1024];
    int symbol_capture_active;
    int symbol_playback_active;
    char symbol_out_file[1024];
    int tcp_audio_connected;
    int udp_input_active;
    int rigctl_connected;
    int rtl_input_active;
    int trunk_use_allow_list;
    int trunk_tune_group_calls;
    int trunk_tune_private_calls;
    int trunk_tune_data_calls;
    int trunk_tune_enc_calls;
    int p25_lcw_retune;
    int p25_prefer_candidates;
    int call_alert;
    uint8_t call_alert_events;

    uint64_t p2_wacn;
    uint64_t p2_sysid;
    uint64_t p2_cc;
    uint32_t tg_hold;
    uint32_t lasttg;
    uint32_t lasttgR;
    int slot_preference;
    int slot1_on;
    int slot2_on;
    double trunk_hangtime;
    int p25_trunk;
    int trunk_enable;
    int scanner_mode;

    int rtl_dev_index;
    uint32_t rtlsdr_center_freq;
    int rtl_gain_value;
    int rtlsdr_ppm_error;
    int rtl_dsp_bw_khz;
    double rtl_squelch_level;
    int rtl_volume_multiplier;
    int rtl_bias_tee;
    int rtltcp_autotune;
    int rtl_auto_ppm;
    double input_warn_db;
    int input_volume_multiplier;
} dsd_frontend_status;

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

int dsd_app_frontend_get_status(dsd_frontend_status* out);
int dsd_app_frontend_get_metrics(dsd_frontend_metrics* out);
int dsd_app_frontend_get_metrics_with_snr_fallbacks(dsd_frontend_metrics* out, unsigned int snr_fallbacks);

int dsd_app_frontend_constellation_get(float* out_xy, int max_points);
int dsd_app_frontend_eye_get(float* out, int max_samples, int* out_sps);
int dsd_app_frontend_spectrum_get(float* out_db, int max_bins, int* out_rate);
int dsd_app_frontend_spectrum_get_size(void);
int dsd_app_frontend_spectrum_set_size(int n);

int dsd_app_frontend_requested_ppm(void);
float dsd_app_frontend_ted_gain(void);
int dsd_app_frontend_auto_ppm_enabled(int configured);
int dsd_app_frontend_tuner_autogain_enabled(int configured);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_ */
