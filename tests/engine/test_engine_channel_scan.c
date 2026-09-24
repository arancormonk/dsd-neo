// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void dsd_key_set_test_alloc_fail_after(long count);

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
    /* The production note_retune opens the per-visit window for the cap (issue #507) as well
     * as for the voice gate, so the rows below can see that the commit anchored a visit. */
    state->scan_visit_since_m = now;
}

static void
test_option_commit_boundary(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->aggressive_framesync = 1;
    opts->scan_voice_hold_ms = 2000;
    state->samplesPerSymbol = 10;
    state->R = 999;
    state->lcn_freq_count = 2;
    *dsd_state_trunk_lcn_slot(state, 0) = *dsd_state_trunk_lcn_slot(state, 1) = 150000000;
    assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
    assert(profile);
    profile->values.present = DSD_SCAN_OPT_FORCE | DSD_SCAN_OPT_CRC | DSD_SCAN_OPT_HOLD;
    profile->values.force = 0x21;
    profile->values.hold_ms = 4000;
    assert(dsd_channel_profile_set(state, 0, profile) == 0);
    dsd_key_set keys = {0};
    keys.present = 1;
    keys.scalars.R = keys.scalars.RR = 0x123456789ULL;
    assert(dsd_state_trunk_lcn_keys_set(state, 0, &keys) == 0);
    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(state->R == 999 && state->M == 0 && opts->scan_voice_hold_ms == 2000);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(!dsd_engine_channel_scan_pending(opts, state));
    assert(state->R == 999 && state->M == 0 && opts->aggressive_framesync == 1);
    state->lcn_freq_roll = 0;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(!dsd_engine_channel_scan_pending(opts, state));
    assert(state->R == 0x123456789ULL && state->RR == state->R && state->M == 0x21);
    assert(opts->scan_voice_hold_ms == 4000 && !opts->aggressive_framesync && opts->dmr_crc_relaxed_default);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_step_manual(opts, state) == 1);
    assert(state->R == 999 && state->M == 0 && opts->scan_voice_hold_ms == 2000 && opts->aggressive_framesync == 1);
    dsd_engine_channel_scan_leave(opts, state);
    dsd_scan_keys_leave(state);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* Rows that carry options but declare no mode still belong to the typed scanner: the legacy
 * scanner applies keys but never row options. Edits to the row-scoped policy (mutes, voice
 * gate) beneath a staged tune take effect without restaging it, whereas a keyring change
 * during the tune window restages so the commit re-prepares against the current globals. */
static void
test_option_only_rows_and_policy_edits(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
    opts->scan_voice_hold_ms = 2000;
    opts->scan_voice_qualify_ms = 1000;
    opts->scan_max_visit_ms = 15000;
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 1;
    *dsd_state_trunk_lcn_slot(state, 0) = 150000000;
    assert(!dsd_channel_modes_present(state));
    dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
    assert(profile);
    profile->values.present = DSD_SCAN_OPT_HOLD | DSD_SCAN_OPT_MUTE_DMR | DSD_SCAN_OPT_MAX_VISIT;
    profile->values.hold_ms = 4000;
    profile->values.mute_dmr = 0;
    profile->values.max_visit_ms = 45000;
    assert(dsd_channel_profile_set(state, 0, profile) == 0);
    assert(dsd_channel_modes_present(state) && dsd_channel_mode_get(state, 0) == DSD_SCAN_MODE_INHERIT);

    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    state->scan_visit_since_m = -1.0;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(opts->scan_voice_hold_ms == 4000 && opts->dmr_mute_encL == 0 && opts->dmr_mute_encR == 0);
    assert(opts->frame_dstar == 1);
    /* The commit is what parks the receiver, so the commit is what opens the per-visit window the
     * cap measures (issue #507) -- with the row's own cap in force, not the global. */
    assert(opts->scan_max_visit_ms == 45000 && state->scan_visit_since_m >= 0.0);

    /* A policy edit during an outstanding request: the commit proceeds without a retry. */
    state->lcn_freq_roll = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(dsd_scan_mode_suspend(opts, state));
    assert(opts->scan_voice_hold_ms == 2000 && opts->dmr_mute_encL == 1 && opts->scan_max_visit_ms == 15000);
    opts->scan_voice_qualify_ms = 1500;
    opts->scan_max_visit_ms = 20000;
    opts->unmute_encrypted_p25 = 1;
    assert(dsd_scan_mode_resume(opts, state) == 0);
    assert(opts->scan_voice_hold_ms == 4000 && opts->dmr_mute_encL == 0 && opts->scan_voice_qualify_ms == 1500);
    /* An edited global beneath a staged tune does not dislodge the row's cap. */
    assert(opts->scan_max_visit_ms == 45000);
    const int before_policy_edit = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before_policy_edit && opts->scan_voice_qualify_ms == 1500 && opts->unmute_encrypted_p25 == 1);
    /* And a row cap is policy, not acquisition: the cycle above restaged nothing. */
    assert(opts->scan_max_visit_ms == 45000);

    /* A keyring change during an outstanding request restages, and the eventual leave hands
     * back the globals as edited rather than the copy prepared before the edit. */
    dsd_key_set keys = {0};
    keys.present = 1;
    keys.scalars.R = keys.scalars.RR = 0x55;
    assert(dsd_state_trunk_lcn_keys_set(state, 0, &keys) == 0);
    state->lcn_freq_roll = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    dsd_scan_keys_suspend(state);
    state->rkey_array[3] = 777;
    state->rkey_array_loaded[3] = 1;
    dsd_scan_keys_resume(state);
    dsd_enc_lockout_bump_key_epoch(state);
    const int before_key_edit = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1);
    assert(tunes == before_key_edit && state->R == 0);
    /* The receiver moved, but its outgoing profile must remain gated if restaging
     * runs out of memory. Repeated sync service passes cannot bypass that gate. */
    dsd_key_set_test_alloc_fail_after(0);
    for (int i = 0; i < 3; i++) {
        assert(dsd_engine_channel_scan_pending(opts, state) == 1);
        state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
        assert(dsd_engine_channel_scan_service_sync(opts, state) == 0);
        assert(state->synctype == DSD_SYNC_NONE);
        assert(dsd_engine_channel_scan_waiting(state));
        assert(tunes == before_key_edit && state->R == 0);
    }
    dsd_key_set_test_alloc_fail_after(-1);
    /* A deferred or rejected retry can retire its tune-ledger gate, but the
     * receiver is still on an uncommitted row. Keep decoding closed and allow
     * ordinary scanning to recover instead of trapping it on the failed row. */
    tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(!dsd_engine_channel_scan_waiting(state));
    assert(dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1));
    assert(!dsd_engine_channel_scan_service_sync(opts, state));
    tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    assert(dsd_engine_channel_scan_step(opts, state) == -1);
    assert(!dsd_engine_channel_scan_waiting(state));
    assert(!dsd_engine_channel_scan_service_sync(opts, state));
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(dsd_engine_channel_scan_service_sync(opts, state) == 1);
    assert(tunes == before_key_edit + 3 && state->R == 0x55 && state->scan_keys_active_set);
    assert(state->rkey_array[3] == 0 && !state->rkey_array_loaded[3]);
    dsd_engine_channel_scan_leave(opts, state);
    dsd_scan_keys_leave(state);
    assert(state->R == 0 && state->rkey_array[3] == 777 && state->rkey_array_loaded[3]);
    assert(opts->scan_voice_hold_ms == 2000 && opts->dmr_mute_encL == 1 && opts->scan_voice_qualify_ms == 1500);
    assert(opts->scan_max_visit_ms == 20000);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* The per-visit cap is a row-scoped option like the voice-gate windows (issue #507): a row that
 * carries one wins while it is on air, a row that carries none inherits the global, a row `0`
 * switches the cap off for that row alone, and leaving the scan hands the configured global back
 * rather than saving whichever row happened to be parked. */
static void
test_row_max_visit_override_and_inherit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->scan_max_visit_ms = 15000;
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 3;
    for (int row = 0; row < 3; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    dsd_scan_row_profile* row_override = (dsd_scan_row_profile*)calloc(1, sizeof(*row_override));
    assert(row_override);
    row_override->values.present = DSD_SCAN_OPT_MAX_VISIT;
    row_override->values.max_visit_ms = 45000;
    assert(dsd_channel_profile_set(state, 0, row_override) == 0);
    dsd_scan_row_profile* row_off = (dsd_scan_row_profile*)calloc(1, sizeof(*row_off));
    assert(row_off);
    row_off->values.present = DSD_SCAN_OPT_MAX_VISIT;
    row_off->values.max_visit_ms = 0;
    assert(dsd_channel_profile_set(state, 2, row_off) == 0);

    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    state->scan_visit_since_m = -1.0;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(opts->scan_max_visit_ms == 45000 && state->lcn_freq_roll == 1);
    assert(state->scan_visit_since_m >= 0.0);
    /* Row 1 carries no override, so the global applies again. */
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(opts->scan_max_visit_ms == 15000 && state->lcn_freq_roll == 2);
    /* Row 2 asks for 0: off for that row even though the global is set. */
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(opts->scan_max_visit_ms == 0 && state->lcn_freq_roll == 3);
    dsd_engine_channel_scan_leave(opts, state);
    assert(opts->scan_max_visit_ms == 15000);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* --- Issue #521: per-row squelch overrides --- */

static int g_squelch_pushes;
static double g_squelch_pushed = -1.0;
static int g_squelch_warnings;
static char g_squelch_warning_rows[8][96];

static void
record_squelch_push(double mean_power) {
    g_squelch_pushes++;
    g_squelch_pushed = mean_power;
}

static void
count_squelch_warnings(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_WARN && text && strstr(text, "--squelch-db")) {
        if (g_squelch_warnings < 8) {
            DSD_SNPRINTF(g_squelch_warning_rows[g_squelch_warnings], sizeof g_squelch_warning_rows[0], "%s", text);
        }
        g_squelch_warnings++;
    }
}

static int
squelch_level_is(double level, double db) {
    const double expected = dsd_squelch_level_from_sql(db);
    return fabs(level - expected) <= 1e-9 * fmax(fabs(level), fabs(expected));
}

/* Rows in succession -- a threshold, the same threshold again, an explicit off and one that
 * inherits. The RTL demod hears each row's level through the runtime hook, once per change and
 * never the configured default in between; dsd_opts carries the same level for the gates that
 * read it. The DECODE_IQ_SCAN_NXDN48_SQUELCH_* replays put a row's level through the real demod
 * gate, and ENGINE_SCAN_SQUELCH_GATE the frame-sync gate through trunk-scan targets. A manual
 * step, an operator edit beneath a row and the scan teardown each leave the configured default
 * where the operator put it. */
static void
test_row_squelch_threshold_off_inherit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 4;
    for (int row = 0; row < 4; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    const int row_db[] = {-60, -60, 0};
    for (int row = 0; row < 3; row++) {
        dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
        assert(profile);
        profile->values.present = DSD_SCAN_OPT_SQUELCH;
        profile->values.squelch_db = row_db[row];
        assert(dsd_channel_profile_set(state, (size_t)row, profile) == 0);
    }
    const dsd_rtl_stream_metrics_hooks hooks = {.output_rate_hz = output_rate,
                                                .set_channel_squelch = record_squelch_push};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_squelch_pushes = 0;
    g_squelch_warnings = 0;
    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 1 && squelch_level_is(opts->rtl_squelch_level, -60.0));
    assert(g_squelch_pushes == 1 && squelch_level_is(g_squelch_pushed, -60.0));

    /* The same threshold on the next row: nothing for the demod to hear. */
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 2 && squelch_level_is(opts->rtl_squelch_level, -60.0));
    assert(g_squelch_pushes == 1);

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 3 && dsd_squelch_is_off(opts->rtl_squelch_level));
    assert(g_squelch_pushes == 2 && dsd_squelch_is_off(g_squelch_pushed));

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 4 && squelch_level_is(opts->rtl_squelch_level, -80.0));
    assert(g_squelch_pushes == 3 && squelch_level_is(g_squelch_pushed, -80.0));

    /* A manual step wraps to the threshold row. */
    assert(dsd_engine_channel_scan_step_manual(opts, state) == 1);
    assert(state->lcn_freq_roll == 1 && squelch_level_is(opts->rtl_squelch_level, -60.0));
    assert(g_squelch_pushes == 4 && squelch_level_is(g_squelch_pushed, -60.0));

    /* The operator edits the default beneath the row: the row keeps its threshold, and the
     * demod is re-told the row's level after the edit, not left on the new default. */
    const int before_edit = g_squelch_pushes;
    assert(dsd_scan_mode_suspend(opts, state));
    assert(squelch_level_is(opts->rtl_squelch_level, -80.0));
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-75.0);
    assert(dsd_scan_mode_resume(opts, state) == 0);
    assert(squelch_level_is(opts->rtl_squelch_level, -60.0));
    assert(g_squelch_pushes == before_edit + 1 && squelch_level_is(g_squelch_pushed, -60.0));
    assert(squelch_level_is(dsd_scan_mode_configured_view(state)->rtl_squelch_level, -75.0));

    /* Teardown hands back the edited default, pushed once. */
    dsd_engine_channel_scan_leave(opts, state);
    assert(squelch_level_is(opts->rtl_squelch_level, -75.0));
    assert(g_squelch_pushes == before_edit + 2 && squelch_level_is(g_squelch_pushed, -75.0));
    /* An RTL input gates acquisition itself, so nothing needed saying. */
    assert(g_squelch_warnings == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* On a PCM input the row threshold still lands in dsd_opts, where the analog input monitor (-8)
 * and its carrier stamp read it against rtl_pwr, but there is no demodulator to gate digital
 * acquisition: the scan says so once per affected row when it starts (and again for a newly
 * imported map), never per visit. */
static void
test_row_squelch_warns_once_per_row_on_pcm_input(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 3;
    for (int row = 0; row < 3; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    for (int row = 0; row < 3; row += 2) {
        dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
        assert(profile);
        profile->values.present = DSD_SCAN_OPT_SQUELCH;
        profile->values.squelch_db = -60;
        assert(dsd_channel_profile_set(state, (size_t)row, profile) == 0);
    }
    const dsd_rtl_stream_metrics_hooks hooks = {.set_channel_squelch = record_squelch_push};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_squelch_pushes = 0;
    g_squelch_warnings = 0;
    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    for (int visit = 0; visit < 6; visit++) {
        assert(dsd_engine_channel_scan_step(opts, state) == 1);
        /* The row's level is in force even though nothing is pushed. */
        const int row = (state->lcn_freq_roll + 2) % 3;
        assert(squelch_level_is(opts->rtl_squelch_level, row == 1 ? -80.0 : -60.0));
    }
    assert(g_squelch_warnings == 2 && g_squelch_pushes == 0);
    assert(strstr(g_squelch_warning_rows[0], "Scan channel 1 ") != NULL);
    assert(strstr(g_squelch_warning_rows[1], "Scan channel 3 ") != NULL);
    /* A new map is a new scan as far as the warning goes. */
    state->trunk_chan_map_seq++;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(g_squelch_warnings == 4);
    dsd_engine_channel_scan_leave(opts, state);
    assert(squelch_level_is(opts->rtl_squelch_level, -80.0) && g_squelch_pushes == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* ---- #518 analog receive core: the leave path restores the configured receive family ---- */

static int frontend_sequence;
static int analog_restore_calls;
static int analog_restore_family;
static int analog_restore_kind;
static int analog_restore_width_hz;
static int analog_restore_order;
static int digital_restore_calls;
static int digital_restore_order;
static int digital_restore_sps;
/* What the front end reports: whether it runs the analog family, and the output rate the digital family lands on. */
static int fake_analog_family;
static unsigned int fake_digital_rate;
static int rate_for_family_calls;
static int rate_for_family_family;
static int rate_for_family_cqpsk;
static int rate_for_family_symbol_rate;

static int
record_analog_restore(int family, int kind, int width_hz) {
    analog_restore_calls++;
    analog_restore_family = family;
    analog_restore_kind = kind;
    analog_restore_width_hz = width_hz;
    analog_restore_order = ++frontend_sequence;
    return 0;
}

static int
record_digital_restore(int cqpsk, int rate, int levels, int filter, int sps) {
    (void)cqpsk;
    (void)rate;
    (void)levels;
    (void)filter;
    digital_restore_calls++;
    digital_restore_order = ++frontend_sequence;
    digital_restore_sps = sps;
    return 0;
}

static int
report_analog_family(void) {
    return fake_analog_family;
}

static unsigned int
report_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz) {
    rate_for_family_calls++;
    rate_for_family_family = family;
    rate_for_family_cqpsk = cqpsk_enable;
    rate_for_family_symbol_rate = symbol_rate_hz;
    return family == DSD_RX_FAMILY_DIGITAL ? fake_digital_rate : 48000U;
}

static void
reset_frontend_records(void) {
    frontend_sequence = 0;
    analog_restore_calls = analog_restore_family = analog_restore_kind = analog_restore_width_hz = 0;
    analog_restore_order = digital_restore_calls = digital_restore_order = digital_restore_sps = 0;
    rate_for_family_calls = rate_for_family_family = rate_for_family_cqpsk = rate_for_family_symbol_rate = 0;
}

/*
 * Leaving a typed row under -fA puts the front end back on the configured analog
 * profile -- family, demodulator and channel width -- rather than a hard-coded
 * WIDE digital profile that leaves the RTL stream on the row's digital family.
 * A digital session gets the digital family first, then its symbol profile.
 */
static void
test_leave_restores_configured_receive_family(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    state->samplesPerSymbol = 10;
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    opts->analog_nfm_bandwidth_hz = 12500;
    const dsd_rtl_stream_metrics_hooks hooks = {.apply_demod_profile = record_digital_restore,
                                                .apply_analog_profile = record_analog_restore};
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    reset_frontend_records();
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(opts->analog_only == 0 && opts->frame_dmr == 1);
    dsd_engine_channel_scan_leave(opts, state);
    assert(opts->analog_only == 1 && dsd_opts_is_analog_family(opts));
    assert(analog_restore_calls == 1);
    assert(analog_restore_family == DSD_RX_FAMILY_ANALOG);
    assert(analog_restore_kind == DSD_ANALOG_DEMOD_FM);
    assert(analog_restore_width_hz == 12500);
    assert(digital_restore_calls == 0);

    /* The configured demodulator kind survives a typed row: the row's digital preset puts the kind back to FM, and
     * leaving the row restores the configured kind together with that kind's width. */
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->analog_am_bandwidth_hz = 9000;
    reset_frontend_records();
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(opts->analog_only == 0 && opts->analog_demod == DSD_ANALOG_DEMOD_FM);
    dsd_engine_channel_scan_leave(opts, state);
    assert(opts->analog_demod == DSD_ANALOG_DEMOD_AM);
    assert(analog_restore_calls == 1 && analog_restore_kind == DSD_ANALOG_DEMOD_AM);
    assert(analog_restore_width_hz == 9000);
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_am_bandwidth_hz = 0;
    opts->analog_nfm_bandwidth_hz = 12500;

    /* The unset default travels as 0, never as a resolved 16 kHz. */
    opts->analog_nfm_bandwidth_hz = 0;
    reset_frontend_records();
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 1 && analog_restore_width_hz == 0);

    /* A digital session: digital family first, then the symbol profile it runs on. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    reset_frontend_records();
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 1 && analog_restore_family == DSD_RX_FAMILY_DIGITAL);
    assert(digital_restore_calls == 1 && analog_restore_order < digital_restore_order);

    /* Off RTL nothing is asked of the front end. */
    opts->audio_in_type = AUDIO_IN_WAV;
    reset_frontend_records();
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 0 && digital_restore_calls == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

/*
 * A -fA session scanning a typed DMR row, whose configured mode the operator changes to P25 Phase 2 while the row
 * runs. The command times the saved configuration for the rate it reads then, the analog monitor's resampled 48 kHz
 * (8 samples per 6000 sym/s symbol), and leaves the front end alone until the row's constraint is gone. Leaving the
 * scan is what switches the front end to the digital family, which runs at the 24 kHz DSP rate: the leave times the
 * decoder, and the symbol profile it publishes, for the rate that family lands on (4 samples per symbol), as a mode
 * change outside a row does. A front end already on the digital family keeps the timing the configuration saved.
 */
static void
test_leave_retimes_the_digital_landing(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    state->samplesPerSymbol = 10;
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    const dsd_rtl_stream_metrics_hooks hooks = {.apply_demod_profile = record_digital_restore,
                                                .apply_analog_profile = record_analog_restore,
                                                .analog_family_active = report_analog_family,
                                                .output_rate_for_family = report_output_rate_for_family};
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_options(opts, state, NULL) == 0);
    assert(dsd_scan_mode_suspend(opts, state) == 1);
    /* What DSD_APP_CMD_DECODE_MODE_SET does while the row's constraint is suspended: the preset, the timing at the
     * 48 kHz the stream outputs now, and the mode's SPS hunt profile (svc_publish_symbol_profile()). */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P2, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    state->samplesPerSymbol = 8;
    state->symbolCenter = dsd_opts_symbol_center(8);
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    (void)dsd_scan_mode_resume(opts, state);
    assert(opts->frame_dmr == 1);

    reset_frontend_records();
    fake_analog_family = 1;
    fake_digital_rate = 24000U;
    dsd_engine_channel_scan_leave(opts, state);
    assert(opts->analog_only == 0 && opts->frame_p25p2 == 1);
    assert(analog_restore_calls == 1 && analog_restore_family == DSD_RX_FAMILY_DIGITAL);
    assert(digital_restore_calls == 1 && analog_restore_order < digital_restore_order);
    assert(rate_for_family_calls == 1 && rate_for_family_family == DSD_RX_FAMILY_DIGITAL);
    assert(rate_for_family_symbol_rate == 6000);
    assert(rate_for_family_cqpsk == (state->rf_mod == 1));
    assert(state->samplesPerSymbol == 4);
    assert(state->symbolCenter == dsd_opts_symbol_center(4));
    assert(digital_restore_sps == 4);

    /* A digital session's front end is already on the digital family: nothing to predict, the saved timing stands. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    state->samplesPerSymbol = 5;
    state->symbolCenter = dsd_opts_symbol_center(5);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48) == 0);
    assert(dsd_scan_mode_options(opts, state, NULL) == 0);
    reset_frontend_records();
    fake_analog_family = 0;
    dsd_engine_channel_scan_leave(opts, state);
    assert(opts->frame_dmr == 1);
    assert(rate_for_family_calls == 0);
    assert(state->samplesPerSymbol == 5 && digital_restore_sps == 5);

    fake_digital_rate = 0U;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

int
main(void) {
    dsd_neo_log_set_tap(count_squelch_warnings, NULL);
    test_leave_restores_configured_receive_family();
    test_leave_retimes_the_digital_landing();
    test_option_commit_boundary();
    test_option_only_rows_and_policy_edits();
    test_row_max_visit_override_and_inherit();
    test_row_squelch_threshold_off_inherit();
    test_row_squelch_warns_once_per_row_on_pcm_input();
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
    state->scan_visit_since_m = -1.0;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(tunes == before_zero && state->lcn_freq_roll == 3 && opts->frame_p25p1);
    /* A zero-frequency placeholder parks in place instead of retuning, and it is still a new
     * visit: the cap has to measure it from here (issue #507). */
    assert(state->scan_visit_since_m >= 0.0);
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
