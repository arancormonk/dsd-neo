// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static const dsd_opts* g_latest_opts;
static const dsd_state* g_latest_state;

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
    opts->terminal_compact = 1;
    opts->terminal_history = 0;
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
    assert(status.terminal_compact == 1);
    assert(status.terminal_history == 0);
    assert(status.audio_in_type == AUDIO_IN_RTL);
    assert(status.audio_out == 1);
    assert(strcmp(status.audio_in_dev, "rtl:0:851.0125M") == 0);
    assert(strcmp(status.config_autosave_path, "/tmp/dsd-neo.ini") == 0);
    assert(status.rtlsdr_center_freq == 851012500U);
    assert(status.rtlsdr_ppm_error == -3);
    assert(status.rtl_auto_ppm == 1);
    assert(status.p2_wacn == 0xbee00U);
    assert(status.tg_hold == 1001U);
    assert(status.lasttgR == 1003U);
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
    return 23.5;
}

static void
test_metrics_fallback_and_runtime_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_frontend_metrics metrics;
    fill_frontend_inputs(&opts, &state);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    assert(dsd_app_frontend_get_metrics(&opts, NULL, &metrics) == 0);
    assert(metrics.output_rate_hz == 0U);
    assert(metrics.snr_c4fm_db == -100.0);
    assert(metrics.snr_gfsk_eye_db == -100.0);
    assert(metrics.requested_ppm == -3);
    assert(metrics.tuner_gain_is_auto == 1);
    assert(metrics.spectrum_size == 0);
    assert(dsd_app_frontend_constellation_get(NULL, 0) == 0);
    assert(dsd_app_frontend_spectrum_get_size() == 0);
    assert(dsd_app_frontend_spectrum_set_size(256) == -1);
    assert(dsd_app_frontend_auto_ppm_enabled(NULL, 1) == 1);
    assert(dsd_app_frontend_tuner_autogain_enabled(NULL, 0) == 0);

    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_kind = hook_output_kind;
    hooks.output_rate_hz = hook_output_rate;
    hooks.symbol_profile = hook_symbol_profile;
    hooks.cqpsk_status = hook_cqpsk_status;
    hooks.snr_c4fm_db = hook_snr_c4fm;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    assert(dsd_app_frontend_get_metrics(&opts, &state, &metrics) == 0);
    assert(metrics.output_kind == DSD_FRONTEND_RTL_OUTPUT_SYMBOL_CQPSK);
    assert(metrics.output_rate_hz == 48000U);
    assert(metrics.symbol_rate_hz == 4800);
    assert(metrics.symbol_levels == 4);
    assert(metrics.channel_profile == 5);
    assert(metrics.cqpsk_enable == 1);
    assert(metrics.cqpsk_timing_active == 1);
    assert(metrics.snr_c4fm_db == 23.5);
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

int
main(void) {
    test_status_copies_opts_and_state();
    test_status_reads_latest_snapshots();
    test_metrics_fallback_and_runtime_hooks();
    printf("UI_FRONTEND_STATUS_METRICS: OK\n");
    return 0;
}
