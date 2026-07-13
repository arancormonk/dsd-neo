// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "snapshot_internal.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#endif

enum { FRONTEND_SNR_INVALID_DB = -50 };

static void
frontend_metrics_defaults(dsd_frontend_metrics* out, const dsd_opts* opts) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->snr_c4fm_db = -100.0;
    out->snr_c4fm_eye_db = -100.0;
    out->snr_cqpsk_db = -100.0;
    out->snr_gfsk_db = -100.0;
    out->snr_gfsk_eye_db = -100.0;
    out->snr_qpsk_const_db = -100.0;
    out->requested_ppm = opts ? opts->rtlsdr_ppm_error : 0;
    out->tuner_gain_is_auto = 1;
}

static int
frontend_snr_value_is_valid(double snr_db) {
    return snr_db > (double)FRONTEND_SNR_INVALID_DB;
}

static void
frontend_metrics_from_runtime_hooks(dsd_frontend_metrics* out) {
    out->output_rate_hz = dsd_rtl_stream_metrics_hook_output_rate_hz();
    out->output_kind = dsd_rtl_stream_metrics_hook_output_kind();
    (void)dsd_rtl_stream_metrics_hook_symbol_profile(&out->symbol_rate_hz, &out->symbol_levels, &out->channel_profile);
    out->stream_generation = dsd_rtl_stream_metrics_hook_stream_generation();
    out->stream_active = dsd_rtl_stream_metrics_hook_stream_active();
    (void)dsd_rtl_stream_metrics_hook_input_level(&out->input_level);
    (void)dsd_rtl_stream_metrics_hook_cqpsk_status(&out->cqpsk_enable, &out->cqpsk_timing_active);
    out->cqpsk_timing_bias = dsd_rtl_stream_metrics_hook_cqpsk_timing_bias();
    out->snr_bias_evm = dsd_rtl_stream_metrics_hook_snr_bias_evm();
    out->snr_c4fm_db = dsd_rtl_stream_metrics_hook_snr_c4fm_db();
    out->snr_cqpsk_db = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
    out->snr_gfsk_db = dsd_rtl_stream_metrics_hook_snr_gfsk_db();
}

static void
frontend_metrics_add_snr_fallbacks(dsd_frontend_metrics* out, unsigned int snr_fallbacks) {
    if ((snr_fallbacks & DSD_FRONTEND_SNR_FALLBACK_C4FM_EYE) != 0u && !frontend_snr_value_is_valid(out->snr_c4fm_db)) {
        out->snr_c4fm_eye_db = dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db();
    }
    if ((snr_fallbacks & DSD_FRONTEND_SNR_FALLBACK_GFSK_EYE) != 0u && !frontend_snr_value_is_valid(out->snr_gfsk_db)) {
        out->snr_gfsk_eye_db = dsd_rtl_stream_metrics_hook_snr_gfsk_eye_db();
    }
    if ((snr_fallbacks & DSD_FRONTEND_SNR_FALLBACK_QPSK_CONST) != 0u
        && !frontend_snr_value_is_valid(out->snr_cqpsk_db)) {
        out->snr_qpsk_const_db = dsd_rtl_stream_metrics_hook_snr_qpsk_const_db();
    }
}

#ifdef USE_RADIO
static void
frontend_metrics_from_radio(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out) {
    int iq_dc_k = 0;
    out->iq_balance = rtl_stream_get_iq_balance();
    out->iq_dc_enabled = rtl_stream_get_iq_dc(&iq_dc_k);
    out->iq_dc_shift_k = iq_dc_k;
    out->ted_sps = rtl_stream_get_ted_sps();
    out->ted_gain = rtl_stream_get_ted_gain();
    out->cfo_hz = rtl_stream_get_cfo_hz();
    out->carrier_lock = rtl_stream_get_carrier_lock();
    out->costas_err_q14 = rtl_stream_get_costas_err_q14();
    out->nco_q15 = rtl_stream_get_nco_q15();
    out->demod_rate_hz = rtl_stream_get_demod_rate_hz();
    out->fll_band_edge_freq_hz = rtl_stream_get_fll_band_edge_freq_hz();
    out->spectrum_size = rtl_stream_spectrum_get_size();
    out->snr_bias_c4fm = rtl_stream_get_snr_bias_c4fm();
    out->requested_ppm = rtl_stream_get_requested_ppm(opts);
    out->tuner_gain_valid = (rtl_stream_get_gain(&out->tuner_gain_tenth_db, &out->tuner_gain_is_auto) == 0) ? 1 : 0;
    out->auto_ppm_enabled = rtl_stream_get_auto_ppm();
    out->tuner_autogain = rtl_stream_get_tuner_autogain();

    rtl_stream_costas_metrics cm;
    DSD_MEMSET(&cm, 0, sizeof(cm));
    if (rtl_stream_get_costas_metrics(&cm) == 0) {
        out->costas.err_smooth_avg_q14 = cm.err_smooth_avg_q14;
        out->costas.err_raw_avg_q14 = cm.err_raw_avg_q14;
        out->costas.confidence_avg_q14 = cm.confidence_avg_q14;
        out->costas.zero_conf_pct = cm.zero_conf_pct;
        out->costas_err_q14 = cm.err_smooth_avg_q14;
    }

    int ap_locked = 0;
    (void)rtl_stream_auto_ppm_get_status(&out->auto_ppm_enabled, &out->auto_ppm_snr_db, &out->auto_ppm_df_hz, NULL,
                                         &out->auto_ppm_step_dir, NULL, &ap_locked);
    out->auto_ppm_locked = ap_locked;
    if (ap_locked) {
        (void)rtl_stream_auto_ppm_get_lock(&out->auto_ppm_locked_ppm, NULL, NULL);
    }

    rtl_stream_decode_health health;
    DSD_MEMSET(&health, 0, sizeof(health));
    if (rtl_stream_get_decode_health(&health) == 0) {
        out->decode_health.valid = health.valid;
        out->decode_health.generation = health.generation;
        out->decode_health.p25p1_fec_ok = health.p25p1_fec_ok;
        out->decode_health.p25p1_fec_err = health.p25p1_fec_err;
        out->decode_health.p25p2_facch_ok = health.p25p2_facch_ok;
        out->decode_health.p25p2_facch_err = health.p25p2_facch_err;
        out->decode_health.p25p2_sacch_ok = health.p25p2_sacch_ok;
        out->decode_health.p25p2_sacch_err = health.p25p2_sacch_err;
        out->decode_health.p25p2_voice_err = health.p25p2_voice_err;
    }
    if (state && state->rtl_ctx) {
        out->output_rate_hz = rtl_stream_output_rate(state->rtl_ctx);
    }
}
#endif

static int
frontend_get_metrics(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out) {
    if (!out) {
        return -1;
    }
    frontend_metrics_defaults(out, opts);
    frontend_metrics_from_runtime_hooks(out);
#ifdef USE_RADIO
    frontend_metrics_from_radio(opts, state, out);
#else
    (void)state;
#endif
    return 0;
}

static int
frontend_get_metrics_with_snr_fallbacks(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out,
                                        unsigned int snr_fallbacks) {
    int rc = frontend_get_metrics(opts, state, out);
    if (rc != 0) {
        return rc;
    }
    frontend_metrics_add_snr_fallbacks(out, snr_fallbacks);
    return 0;
}

int
dsd_app_frontend_get_metrics(dsd_frontend_metrics* out) {
    return frontend_get_metrics(dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot(), out);
}

int
dsd_app_frontend_get_metrics_with_snr_fallbacks(dsd_frontend_metrics* out, unsigned int snr_fallbacks) {
    return frontend_get_metrics_with_snr_fallbacks(dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot(),
                                                   out, snr_fallbacks);
}

int
dsd_app_frontend_constellation_get(float* out_xy, int max_points) {
#ifdef USE_RADIO
    return rtl_stream_constellation_get(out_xy, max_points);
#else
    (void)out_xy;
    (void)max_points;
    return 0;
#endif
}

int
dsd_app_frontend_eye_get(float* out, int max_samples, int* out_sps) {
#ifdef USE_RADIO
    return rtl_stream_eye_get(out, max_samples, out_sps);
#else
    (void)out;
    (void)max_samples;
    if (out_sps) {
        *out_sps = 0;
    }
    return 0;
#endif
}

int
dsd_app_frontend_spectrum_get(float* out_db, int max_bins, int* out_rate) {
#ifdef USE_RADIO
    return rtl_stream_spectrum_get(out_db, max_bins, out_rate);
#else
    (void)out_db;
    (void)max_bins;
    if (out_rate) {
        *out_rate = 0;
    }
    return 0;
#endif
}
