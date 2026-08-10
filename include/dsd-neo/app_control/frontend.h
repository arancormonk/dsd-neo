// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-facing app-control metrics API.
 *
 * This boundary exposes plain metric values without leaking live
 * dsd_opts/dsd_state pointers or IO-layer RTL types. The one accessor that does
 * take those pointers reads published snapshots, never the live objects — see
 * dsd_app_frontend_get_metrics_for_snapshot().
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
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
    /* Full width in Hz of the channel the demodulator is filtering — twice the
     * protected edge of channel_profile. 0 when the input is not a radio. The
     * channel, not the filter: see dsd_channel_lpf_protected_edge_hz(). */
    int channel_bandwidth_hz;
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

/**
 * @brief Metrics read from snapshots the caller already holds.
 *
 * The accessors above take a fresh snapshot each time. A frontend that reads both
 * metrics and snapshot fields to build one frame would therefore consume twice, and
 * a publish landing in between leaves the two halves of that frame describing
 * different generations. Consume once per frame (see app_control/snapshot.h) and
 * pass the result here.
 *
 * @param opts          Options snapshot from dsd_app_get_latest_opts_snapshot().
 * @param state         State snapshot from dsd_app_get_latest_snapshot().
 * @param out           Receives the metrics.
 * @param snr_fallbacks DSD_FRONTEND_SNR_FALLBACK_* mask.
 * @return 0 on success, negative on failure.
 */
int dsd_app_frontend_get_metrics_for_snapshot(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out,
                                              unsigned int snr_fallbacks);

/** @brief One SNR reading, and whether an estimator actually produced it. */
typedef struct {
    double snr_db; /**< Reading in dB. Only meaningful while @c valid is nonzero. */
    int valid;     /**< Nonzero when an estimator reported; zero when none has yet. */
} dsd_frontend_snr_readout;

/**
 * @brief Pick the SNR estimator that matches @p rf_mod, applying the fallbacks.
 *
 * There is one estimator per modulation and they do not stand in for one another: a
 * GFSK stream reads nothing on the C4FM estimator, and a session with no demodulator
 * at all (UDP, file, rtl_tcp) reads nothing anywhere. Each estimator reports a large
 * negative sentinel until it has a measurement, so a frontend that publishes the
 * value unconditionally prints that sentinel as though it were a reading.
 *
 * Every frontend selects the same way through this: by modulation, then the eye or
 * constellation fallback for that modulation, and only then reports invalid.
 *
 * @param metrics Metrics filled with DSD_FRONTEND_SNR_FALLBACK_ALL, or NULL.
 * @param rf_mod  Modulation from dsd_state::rf_mod (0 C4FM, 1 QPSK, 2 GFSK).
 */
dsd_frontend_snr_readout dsd_app_frontend_snr_for_mod(const dsd_frontend_metrics* metrics, int rf_mod);

int dsd_app_frontend_constellation_get(float* out_xy, int max_points);
int dsd_app_frontend_eye_get(float* out, int max_samples, int* out_sps);
int dsd_app_frontend_spectrum_get(float* out_db, int max_bins, int* out_rate);

/**
 * @brief Copy the wideband spectrum covering the whole SDR capture span.
 *
 * Unlike dsd_app_frontend_spectrum_get(), which reports the narrow
 * post-decimation span used for tuner diagnostics, this covers the full
 * capture bandwidth so a frontend can draw a panorama around the tuned
 * frequency. Bins are DC-centered and the center/span are published together
 * with them, so the axis always matches the data.
 *
 * Production is off by default: a frontend must call
 * dsd_app_frontend_wideband_spectrum_set_enabled(1) while its view is open and
 * 0 when it closes, so the FFT costs nothing the rest of the time.
 *
 * @param out_db Destination buffer, at least DSD_WIDEBAND_SPECTRUM_BINS floats
 *               (from <dsd-neo/core/wideband_spectrum.h>).
 * @param max_bins Capacity of @p out_db in floats. A shorter buffer is refused
 *                 rather than filled with a prefix of the span.
 * @param out_center_freq_hz Optional; receives the tuned center in Hz.
 * @param out_span_hz Optional; receives the covered span in Hz.
 * @param out_frame_serial Optional; receives the frame's serial number, which
 *                         changes only when a new frame is published. A
 *                         frontend polling on its own timer needs it to tell a
 *                         fresh frame from a re-read of the one it already drew.
 * @return Number of bins written; 0 when disabled, unavailable, too large for
 *         @p out_db, or on a build with no radio backend.
 */
int dsd_app_frontend_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz,
                                           uint32_t* out_span_hz, uint32_t* out_frame_serial);
/** @brief Enable or disable wideband spectrum production (off = zero DSP cost). */
void dsd_app_frontend_wideband_spectrum_set_enabled(int on);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_FRONTEND_H_ */
