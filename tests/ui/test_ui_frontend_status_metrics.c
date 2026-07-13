// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include "../../src/app_control/frontend_internal.h"
#include "../../src/app_control/snapshot_internal.h"

static const dsd_opts* g_latest_opts;
static const dsd_state* g_latest_state;
static int g_snr_c4fm_eye_calls;
static int g_snr_gfsk_eye_calls;
static int g_snr_qpsk_const_calls;
static double g_snr_c4fm = 23.5;
static double g_snr_cqpsk = 19.25;
static double g_snr_gfsk = 17.75;

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return g_latest_opts;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    return g_latest_state;
}

static void
fill_frontend_inputs(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->frontend_kind = DSD_FRONTEND_TERMINAL;
    opts->frontend_display.constellation = 1;
    opts->frontend_display.show_dsp_panel = 1;
    opts->frontend_terminal_display.terminal_compact = 1;
    opts->frontend_terminal_display.terminal_history = 0;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->audio_out_type = 0;
    opts->audio_out = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851.0125M");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
    DSD_SNPRINTF(opts->pa_input_idx, sizeof opts->pa_input_idx, "%s", "source");
    DSD_SNPRINTF(opts->pa_output_idx, sizeof opts->pa_output_idx, "%s", "sink");
    opts->rtl_dev_index = 2;
    opts->rtlsdr_center_freq = 851012500U;
    opts->rtl_gain_value = 24;
    opts->rtlsdr_ppm_error = -3;
    opts->rtl_dsp_bw_khz = 24;
    opts->rtl_auto_ppm = 1;
    opts->input_warn_db = -42.5;
    opts->payload = 1;
    DSD_SNPRINTF(opts->event_out_file, sizeof opts->event_out_file, "%s", "/tmp/events.log");
    opts->dmr_stereo_wav = 1;
    opts->wav_out_f = (SNDFILE*)0x1;
    opts->static_wav_file = 1;
    DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", "/tmp/wav");
    DSD_SNPRINTF(opts->wav_out_file, sizeof opts->wav_out_file, "%s", "/tmp/static.wav");
    DSD_SNPRINTF(opts->wav_out_file_raw, sizeof opts->wav_out_file_raw, "%s", "/tmp/raw.wav");
    opts->symbol_out_f = (FILE*)0x1;
    DSD_SNPRINTF(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s", "/tmp/symbols.bin");
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = (dsd_socket_t)7;
    opts->trunk_use_allow_list = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->p25_lcw_retune = 1;
    opts->p25_prefer_candidates = 1;
    opts->call_alert = 1;
    opts->call_alert_events = 3;

    state->config_autosave_enabled = 1;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", "/tmp/dsd-neo.ini");
    state->p2_wacn = 0xbee00U;
    state->p2_sysid = 0x123U;
    state->p2_cc = 0x456U;
    state->tg_hold = 1001;
    state->lasttg = 1002;
    state->lasttgR = 1003;
}

static void
test_status_copies_opts_and_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_status status;
    fill_frontend_inputs(&opts, &state);

    dsd_app_frontend_status_from_opts_state(&opts, &state, &status);
    opts.audio_in_dev[0] = '\0';
    state.config_autosave_path[0] = '\0';

    assert(status.frontend_kind == DSD_FRONTEND_TERMINAL);
    assert(status.display.constellation == 1);
    assert(status.display.show_dsp_panel == 1);
    assert(status.terminal_display.terminal_compact == 1);
    assert(status.terminal_display.terminal_history == 0);
    assert(status.audio_in_type == AUDIO_IN_RTL);
    assert(status.audio_out == 1);
    assert(strcmp(status.audio_in_dev, "rtl:0:851.0125M") == 0);
    assert(strcmp(status.config_autosave_path, "/tmp/dsd-neo.ini") == 0);
    assert(status.rtlsdr_center_freq == 851012500U);
    assert(status.rtlsdr_ppm_error == -3);
    assert(status.rtl_auto_ppm == 1);
    assert(status.payload_logging == 1);
    assert(status.event_log_enabled == 1);
    assert(strcmp(status.event_log_path, "/tmp/events.log") == 0);
    assert(status.per_call_wav_enabled == 1);
    assert(status.per_call_wav_active == 1);
    assert(status.static_wav_enabled == 1);
    assert(status.static_wav_active == 1);
    assert(strcmp(status.wav_out_dir, "/tmp/wav") == 0);
    assert(strcmp(status.wav_out_file, "/tmp/static.wav") == 0);
    assert(strcmp(status.wav_out_file_raw, "/tmp/raw.wav") == 0);
    assert(status.symbol_capture_active == 1);
    assert(strcmp(status.symbol_out_file, "/tmp/symbols.bin") == 0);
    assert(status.rigctl_connected == 1);
    assert(status.trunk_use_allow_list == 1);
    assert(status.trunk_tune_group_calls == 1);
    assert(status.trunk_tune_private_calls == 1);
    assert(status.trunk_tune_data_calls == 1);
    assert(status.p25_lcw_retune == 1);
    assert(status.p25_prefer_candidates == 1);
    assert(status.call_alert == 1);
    assert(status.call_alert_events == 3);
    assert(status.p2_wacn == 0xbee00U);
    assert(status.tg_hold == 1001U);
    assert(status.lasttgR == 1003U);
}

static void
test_status_treats_invalid_rigctl_socket_as_disconnected(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_status status;
    fill_frontend_inputs(&opts, &state);
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;

    dsd_app_frontend_status_from_opts_state(&opts, &state, &status);

    assert(status.use_rigctl == 1);
    assert(status.rigctl_connected == 0);
}

static void
test_status_separates_static_and_per_call_wav_activity(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_status status;
    fill_frontend_inputs(&opts, &state);
    opts.dmr_stereo_wav = 0;
    opts.static_wav_file = 1;
    opts.wav_out_f = (SNDFILE*)0x1;
    opts.wav_out_fR = NULL;

    dsd_app_frontend_status_from_opts_state(&opts, &state, &status);

    assert(status.per_call_wav_enabled == 0);
    assert(status.per_call_wav_active == 0);
    assert(status.static_wav_enabled == 1);
    assert(status.static_wav_active == 1);
}

static void
test_status_reads_latest_snapshots(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_status status;
    fill_frontend_inputs(&opts, &state);
    g_latest_opts = &opts;
    g_latest_state = &state;

    assert(dsd_app_frontend_get_status(&status) == 0);
    assert(status.frontend_kind == DSD_FRONTEND_TERMINAL);
    assert(status.rtl_dev_index == 2);
    assert(status.config_autosave_enabled == 1);
}

static int
hook_output_kind(void) {
    return DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK;
}

static unsigned int
hook_output_rate(void) {
    return 48000U;
}

static int
hook_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = 5;
    }
    return 0;
}

static int
hook_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 1;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 1;
    }
    return 0;
}

static double
hook_snr_c4fm(void) {
    return g_snr_c4fm;
}

static double
hook_snr_cqpsk(void) {
    return g_snr_cqpsk;
}

static double
hook_snr_gfsk(void) {
    return g_snr_gfsk;
}

static double
hook_snr_c4fm_eye(void) {
    ++g_snr_c4fm_eye_calls;
    return 7.25;
}

static double
hook_snr_gfsk_eye(void) {
    ++g_snr_gfsk_eye_calls;
    return 6.5;
}

static double
hook_snr_qpsk_const(void) {
    ++g_snr_qpsk_const_calls;
    return 9.75;
}

static void
reset_snr_hook_fakes(double c4fm, double cqpsk, double gfsk) {
    g_snr_c4fm = c4fm;
    g_snr_cqpsk = cqpsk;
    g_snr_gfsk = gfsk;
    g_snr_c4fm_eye_calls = 0;
    g_snr_gfsk_eye_calls = 0;
    g_snr_qpsk_const_calls = 0;
}

static void
test_metrics_fallback_and_runtime_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_metrics metrics;
    fill_frontend_inputs(&opts, &state);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    g_latest_opts = &opts;
    g_latest_state = NULL;
    assert(dsd_app_frontend_get_metrics(&metrics) == 0);
    assert(metrics.output_rate_hz == 0U);
    assert(metrics.snr_c4fm_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.requested_ppm == -3);
    assert(metrics.tuner_gain_is_auto == 1);
    assert(metrics.spectrum_size == 0);
    assert(dsd_app_frontend_constellation_get(NULL, 0) == 0);
    assert(dsd_app_frontend_spectrum_get_size() == 0);
    assert(dsd_app_frontend_spectrum_set_size(256) == -1);
    assert(dsd_app_frontend_auto_ppm_enabled(1) == 1);
    assert(dsd_app_frontend_tuner_autogain_enabled(0) == 0);

    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_kind = hook_output_kind;
    hooks.output_rate_hz = hook_output_rate;
    hooks.symbol_profile = hook_symbol_profile;
    hooks.cqpsk_status = hook_cqpsk_status;
    hooks.snr_c4fm_db = hook_snr_c4fm;
    hooks.snr_c4fm_eye_db = hook_snr_c4fm_eye;
    hooks.snr_cqpsk_db = hook_snr_cqpsk;
    hooks.snr_gfsk_db = hook_snr_gfsk;
    hooks.snr_gfsk_eye_db = hook_snr_gfsk_eye;
    hooks.snr_qpsk_const_db = hook_snr_qpsk_const;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_snr_hook_fakes(23.5, 19.25, 17.75);
    g_latest_opts = &opts;
    g_latest_state = &state;
    assert(dsd_app_frontend_get_metrics(&metrics) == 0);
    assert(metrics.output_kind == DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK);
    assert(metrics.output_rate_hz == 48000U);
    assert(metrics.symbol_rate_hz == 4800);
    assert(metrics.symbol_levels == 4);
    assert(metrics.channel_profile == 5);
    assert(metrics.cqpsk_enable == 1);
    assert(metrics.cqpsk_timing_active == 1);
    assert(metrics.snr_c4fm_db == 23.5);
    assert(metrics.snr_cqpsk_db == 19.25);
    assert(metrics.snr_gfsk_db == 17.75);
    assert(metrics.snr_c4fm_eye_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.snr_qpsk_const_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 0);
    assert(g_snr_gfsk_eye_calls == 0);
    assert(g_snr_qpsk_const_calls == 0);

    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_ALL) == 0);
    assert(metrics.snr_c4fm_db == 23.5);
    assert(metrics.snr_cqpsk_db == 19.25);
    assert(metrics.snr_gfsk_db == 17.75);
    assert(metrics.snr_c4fm_eye_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.snr_qpsk_const_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 0);
    assert(g_snr_gfsk_eye_calls == 0);
    assert(g_snr_qpsk_const_calls == 0);

    reset_snr_hook_fakes(-100.0, -100.0, -100.0);
    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_C4FM_EYE
                                                                         | DSD_FRONTEND_SNR_FALLBACK_QPSK_CONST)
           == 0);
    assert(metrics.snr_c4fm_eye_db == 7.25);
    assert(metrics.snr_qpsk_const_db == 9.75);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 1);
    assert(g_snr_gfsk_eye_calls == 0);
    assert(g_snr_qpsk_const_calls == 1);

    reset_snr_hook_fakes(-100.0, -100.0, -100.0);
    assert(dsd_app_frontend_get_metrics_with_snr_fallbacks(&metrics, DSD_FRONTEND_SNR_FALLBACK_GFSK_EYE) == 0);
    assert(metrics.snr_c4fm_eye_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == 6.5);
    assert(metrics.snr_qpsk_const_db == -100.0);
    assert(g_snr_c4fm_eye_calls == 0);
    assert(g_snr_gfsk_eye_calls == 1);
    assert(g_snr_qpsk_const_calls == 0);
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

int
main(void) {
    test_status_copies_opts_and_state();
    test_status_treats_invalid_rigctl_socket_as_disconnected();
    test_status_separates_static_and_per_call_wav_activity();
    test_status_reads_latest_snapshots();
    test_metrics_fallback_and_runtime_hooks();
    printf("UI_FRONTEND_STATUS_METRICS: OK\n");
    return 0;
}
