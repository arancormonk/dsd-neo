// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdlib.h>

static dsd_trunk_tune_result tune_result;
static uint64_t request;
static int tunes;
static int reset_count;
static int last_forget_modulation;
static int expected_nxdn;
static int change_rate_on_tune;
static uint32_t reported_rate = 48000;

static uint32_t
output_rate(void) {
    return reported_rate;
}

static int restored_frontend;
static int expected_frontend_rate = 4800;
static int expected_frontend_levels = 2;
static int expected_frontend_sps = 10;

static int
restore_frontend(int cqpsk, int rate, int levels, int filter, int sps) {
    assert(cqpsk == 0 && rate == expected_frontend_rate && levels == expected_frontend_levels
           && sps == expected_frontend_sps);
    assert(filter == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);
    restored_frontend++;
    return 0;
}

dsd_trunk_tune_result
dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t* out) {
    assert(freq == 150000000);
    assert(opts->frame_nxdn48 == expected_nxdn);
    assert(sps == (change_rate_on_tune ? (int)reported_rate / 4800 : (expected_nxdn ? 20 : 10)));
    if (change_rate_on_tune) {
        assert(tunes < 100);
        reported_rate = reported_rate == 48000 ? 96000 : 48000;
    }
    (void)state;
    tunes++;
    request = dsd_trunk_tuning_request_begin();
    *out = request;
    if (tune_result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        dsd_trunk_tuning_request_mark_ready(request);
    } else {
        dsd_trunk_tuning_request_complete(request, tune_result);
    }
    return tune_result;
}

void
dsd_engine_reset_no_carrier_state(dsd_opts* opts, dsd_state* state) {
    assert(opts->scanner_mode == 1);
    (void)state;
    reset_count++;
}

void
dsd_frame_sync_reset_acquisition(const dsd_opts* opts, dsd_state* state, int forget) {
    (void)opts;
    last_forget_modulation = forget;
    state->synctype = DSD_SYNC_NONE;
    state->profile_proof_valid = 0;
    state->symbol_history_count = 0;
    state->sps_hunt_counter = 0;
}

void
dsd_scan_voice_gate_note_retune(dsd_state* state, double now) {
    state->last_cc_sync_time_m = now;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->scanner_mode = 1;
    state->samplesPerSymbol = 10;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->lcn_freq_count = 4;
    for (int i = 0; i < 4; i++) {
        *dsd_state_trunk_lcn_slot(state, i) = i == 2 ? 0 : 150000000;
    }
    assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NXDN48) == 0);
    assert(dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_P25) == 0);
    assert(dsd_channel_mode_set(state, 2, DSD_SCAN_MODE_DMR) == 0);
    expected_nxdn = 1;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(state->lcn_freq_roll == 0 && opts->frame_dstar == 1);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1);
    state->synctype = DSD_SYNC_P25P1_POS;
    assert(!dsd_engine_channel_scan_service_sync(opts, state));
    assert(state->synctype == DSD_SYNC_NONE);
    const uint64_t old_generation = dsd_trunk_tuning_generation();
    assert(!dsd_trunk_tuning_frame_is_dispatchable(old_generation, 1));
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    state->synctype = DSD_SYNC_P25P1_POS;
    assert(!dsd_engine_channel_scan_service_sync(opts, state));
    assert(state->lcn_freq_roll == 1 && opts->frame_nxdn48 == 1 && reset_count == 1);
    assert(!dsd_trunk_tuning_frame_is_dispatchable(old_generation, 1));
    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    assert(dsd_engine_channel_scan_step(opts, state) == -1);
    assert(state->lcn_freq_roll == 2 && opts->frame_nxdn48 == 1);
    /* A temporary deferral retries its own row; a hard failure skips it. */
    state->lcn_freq_roll = 1;
    tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    assert(dsd_engine_channel_scan_step(opts, state) == -1);
    assert(state->lcn_freq_roll == 1);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 2 && opts->frame_p25p1 && opts->frame_p25p2 && !opts->frame_dmr);
    const int before_zero = tunes;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(tunes == before_zero && state->lcn_freq_roll == 3 && opts->frame_p25p1);
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 4 && opts->frame_dstar == 1 && !opts->frame_p25p1);
    state->lcn_freq_roll = 2;
    opts->trunk_is_tuned = 1;
    assert(dsd_recent_activity_publish(state, 0, NULL, "Outgoing row data", 1) == 1);
    tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    assert(dsd_engine_channel_scan_step_manual(opts, state) == -1);
    assert(state->lcn_freq_roll == 4);
    state->lcn_freq_roll = 2;
    opts->trunk_is_tuned = 1;
    assert(dsd_recent_activity_publish(state, 0, NULL, "Outgoing row data", 1) == 1);
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step_manual(opts, state) == 0);
    assert(state->lcn_freq_roll == 2);
    assert(dsd_scan_mode_suspend(opts, state));
    opts->inverted_dmr = !opts->inverted_dmr;
    (void)dsd_scan_mode_resume(opts, state);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1 && state->lcn_freq_roll == 2);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(state->lcn_freq_roll == 4);
    dsd_recent_activity_snapshot recent;
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].notice[0] == '\0' && opts->trunk_is_tuned == 0);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    state->lcn_freq_roll = 2;
    assert(dsd_engine_channel_scan_step_manual(opts, state) == 1);
    assert(state->lcn_freq_roll == 4 && opts->frame_dstar);
    expected_nxdn = 1;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(opts->frame_dstar && state->lcn_freq_roll == 1);
    state->lcn_freq_roll = 0;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    state->trunk_chan_map_seq++;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 0 && opts->frame_dstar);
    /* A tune changes live output timing; it is not a configuration edit and
     * must not recursively retune the same row. */
    reported_rate = 48000;
    opts->audio_in_type = AUDIO_IN_RTL;
    change_rate_on_tune = 1;
    expected_nxdn = 0;
    state->lcn_freq_roll = 1;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    const dsd_rtl_stream_metrics_hooks rate_hooks = {.output_rate_hz = output_rate};
    dsd_rtl_stream_metrics_hooks_set(&rate_hooks);
    const int before_rate_change = tunes;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(tunes == before_rate_change + 1 && state->samplesPerSymbol == 20);
    change_rate_on_tune = 0;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    opts->audio_in_type = AUDIO_IN_WAV;
    /* A real configuration edit during an outstanding request retries on the
     * next service pass, without dispatching samples from the old request. */
    state->lcn_freq_roll = 1;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(dsd_scan_mode_suspend(opts, state));
    opts->inverted_dmr = !opts->inverted_dmr;
    (void)dsd_scan_mode_resume(opts, state);
    const int before_config_retry = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1);
    assert(tunes == before_config_retry && state->lcn_freq_roll == 1);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before_config_retry + 1 && state->lcn_freq_roll == 2);
    state->lcn_freq_roll = 0;
    expected_nxdn = 1;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    const dsd_rtl_stream_metrics_hooks hooks = {.apply_demod_profile = restore_frontend};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    opts->audio_in_type = AUDIO_IN_RTL;
    dsd_engine_channel_scan_leave(opts, state);
    assert(restored_frontend == 1 && last_forget_modulation == 1);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 0 && opts->frame_dstar);
    /* Releasing a scope captured halfway through AUTO hunting restores its
     * frontend with the same 2400/4 clock as the saved 20-SPS decoder. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state->samplesPerSymbol = 20;
    state->symbolCenter = 9;
    state->rf_mod = 2;
    expected_frontend_rate = 2400;
    expected_frontend_levels = 4;
    expected_frontend_sps = 20;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    dsd_engine_channel_scan_leave(NULL, state);
    assert(dsd_scan_mode_active(state) == DSD_SCAN_MODE_P25);
    assert(!dsd_engine_channel_scan_pending(NULL, state));
    assert(!dsd_engine_channel_scan_pending(opts, NULL));
    dsd_engine_channel_scan_leave(opts, NULL);
    dsd_engine_channel_scan_leave(opts, state);
    assert(restored_frontend == 2 && state->samplesPerSymbol == 20);
    opts->trunk_scan_enabled = 1;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(last_forget_modulation == 0);
    opts->trunk_scan_enabled = 0;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return 0;
}

int
csvKeyImportHexPath(const char* path, int show, dsd_state* state, dsd_csv_validation* stats) {
    (void)stats;
    (void)show;
    (void)path;
    (void)state;
    return -1;
}

int
csvKeyImportDecPath(const char* path, int show, dsd_state* state, dsd_csv_validation* stats) {
    (void)stats;
    (void)show;
    (void)path;
    (void)state;
    return -1;
}
