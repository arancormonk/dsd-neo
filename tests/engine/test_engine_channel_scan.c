// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/audio.h>
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
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/posix_compat.h>
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

/* --- Issue #526: scanner sinks and DSP rate, from engine/trunk_tuning.c and core/audio, which this fixture does not
 * link. The stubs record which sink each row commit asked for. --- */
static int g_ensure_analog_calls;
static int g_ensure_digital_calls;
static int g_scan_dsp_rate_hz;

int
dsd_audio_ensure_analog_output(dsd_opts* opts) {
    (void)opts;
    g_ensure_analog_calls++;
    return 0;
}

int
dsd_audio_ensure_digital_output(dsd_opts* opts) {
    (void)opts;
    g_ensure_digital_calls++;
    return 0;
}

int
dsd_engine_scan_dsp_rate_hz(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return g_scan_dsp_rate_hz;
}

/* The live receive-family requests the RTL front end has accepted (trunk_tuning.c). A case standing in for a command
 * that makes one (the width command, a config apply) moves it. */
static uint32_t g_family_requests;

uint32_t
dsd_engine_scan_family_requests(const dsd_opts* opts) {
    (void)opts;
    return g_family_requests;
}

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

/* What the options said at the tune (issue #526): the row's family and width reach the front end through them. */
static int tuned_analog_only = -1;
static int tuned_nfm_width_hz = -1;

dsd_trunk_tune_result
dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t* out) {
    assert(freq == 150000000);
    assert(opts->frame_nxdn48 == expected_nxdn);
    tuned_analog_only = opts->analog_only;
    tuned_nfm_width_hz = opts->analog_nfm_bandwidth_hz;
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
    /* The real reset also forgets the received tone (issue #522; pinned by
       FRAME_SYNC_INTERNAL_HELPERS). Mirrored here so a row commit or a leave that stopped
       calling it, or that seeded the tone again afterwards, shows up in their assertions. */
    const uint32_t generation = state->analog_rx.generation + 1U;
    DSD_MEMSET(&state->analog_rx, 0, sizeof(state->analog_rx));
    state->analog_rx.generation = generation;
}

/* A tone the previous row left on the publication, as the analog tap would. */
static void
seed_received_tone(dsd_state* state) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1000;
}

static int
received_tone_cleared(const dsd_state* state) {
    return state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_LOCKED && state->analog_rx.ctcss_tenths_hz == 0
           && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.carrier_open == 0;
}

/* How many times the leave dropped the decoder's part-collected analog monitor block. */
static int analog_block_resets;

void
dsd_symbol_analog_block_reset(dsd_state* state) {
    (void)state;
    analog_block_resets++;
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

static int g_analog_warnings;
static char g_analog_warning_rows[8][384];

static void
count_squelch_warnings(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_WARN && text
        && (strstr(text, "analog channel's squelch") || strstr(text, "--nfm-bandwidth-hz")
            || strstr(text, "skipped at every visit"))) {
        if (g_analog_warnings < 8) {
            DSD_SNPRINTF(g_analog_warning_rows[g_analog_warnings], sizeof g_analog_warning_rows[0], "%s", text);
        }
        g_analog_warnings++;
        return;
    }
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

/*
 * The decoder's part-collected analog monitor block holds samples of the front end's output family. A leave that
 * switches the front end between the analog and digital families drops it, so the first block the other family
 * completes does not start with them; a leave that keeps the family keeps it, and off RTL there is no front end family
 * to switch. A leave that switches the family also forgets the received tone (issue #522), which described the old
 * family's reception.
 */
static void
test_leave_family_switch_drops_partial_analog_block(void) {
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

    /* -fA with a typed DMR row, which runs on the analog family's monitor output: the leave keeps the family. */
    reset_frontend_records();
    analog_block_resets = 0;
    fake_analog_family = 1;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 1 && analog_restore_family == DSD_RX_FAMILY_ANALOG);
    assert(analog_block_resets == 0);

    /* Analog configured while the front end runs the digital family (Analog picked under a digital session's row). */
    reset_frontend_records();
    analog_block_resets = 0;
    fake_analog_family = 0;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    seed_received_tone(state);
    uint32_t generation = state->analog_rx.generation;
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 1 && analog_restore_family == DSD_RX_FAMILY_ANALOG);
    assert(analog_block_resets == 1);
    assert(received_tone_cleared(state) && state->analog_rx.generation != generation);

    /* A digital mode configured while the front end still runs the analog family. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    reset_frontend_records();
    analog_block_resets = 0;
    fake_analog_family = 1;
    fake_digital_rate = 24000U;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48) == 0);
    seed_received_tone(state);
    generation = state->analog_rx.generation;
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 1 && analog_restore_family == DSD_RX_FAMILY_DIGITAL);
    assert(analog_block_resets == 1);
    assert(received_tone_cleared(state) && state->analog_rx.generation != generation);

    /* Digital configured on a digital front end. */
    reset_frontend_records();
    analog_block_resets = 0;
    fake_analog_family = 0;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NXDN48) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 1 && analog_restore_family == DSD_RX_FAMILY_DIGITAL);
    assert(analog_block_resets == 0);

    /* Off RTL. */
    opts->audio_in_type = AUDIO_IN_WAV;
    reset_frontend_records();
    analog_block_resets = 0;
    fake_analog_family = 1;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    dsd_engine_channel_scan_leave(opts, state);
    assert(analog_restore_calls == 0 && analog_block_resets == 0);

    fake_analog_family = 0;
    fake_digital_rate = 0U;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

/* ---- Received tone (issue #522) ----------------------------------------------------------- */

/* A row the scanner commits to starts with no received tone, whether its tune resolved later
   (a pending request the sync service commits) or at once (a step whose tune completed): the
   new row cannot inherit the outgoing row's. */
static void
test_rx_tone_row_commit_and_step_clear(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->frame_dstar = 1;
    opts->scanner_mode = 1;
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 2;
    for (int row = 0; row < 2; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    expected_nxdn = 0;

    /* The outgoing row's tone is still on the publication while the tune is pending ... */
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(state->lcn_freq_roll == 0 && dsd_engine_channel_scan_pending(opts, state) == 1);
    seed_received_tone(state);
    uint32_t generation = state->analog_rx.generation;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    state->synctype = DSD_SYNC_P25P1_POS;
    assert(!dsd_engine_channel_scan_service_sync(opts, state));
    assert(state->lcn_freq_roll == 1 && reset_count == 1);
    /* ... and the row commit takes it away. */
    assert(received_tone_cleared(state) && state->analog_rx.generation != generation);

    /* A step whose tune completes at once commits the next row the same way. */
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    seed_received_tone(state);
    generation = state->analog_rx.generation;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 2);
    assert(received_tone_cleared(state) && state->analog_rx.generation != generation);

    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* --- Issue #526: nfm rows --- */

static dsd_scan_row_profile*
nfm_row_profile(dsd_state* state, int row, uint32_t present, int width_hz, int squelch_db) {
    dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
    assert(profile);
    profile->values.present = present;
    profile->values.channel_bw_hz = width_hz;
    profile->values.squelch_db = squelch_db;
    assert(dsd_channel_profile_set(state, (size_t)row, profile) == 0);
    return profile;
}

/* A map mixing nfm and digital rows: every row tunes with its own family and width in force, the
 * nfm row commits the analog monitor at its width (the configured one without its own), the next
 * digital row puts the digital decoder and the configured width back, each commit opens the sink
 * its row plays through, and leave restores the configured session. A manual step and a failed
 * row move through nfm rows the way they move through digital ones. */
static void
test_nfm_rows_switch_family_width_and_sink(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    opts->analog_nfm_bandwidth_hz = 20000;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-60.0);
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 4;
    for (int row = 0; row < 4; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_channel_mode_set(state, 2, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_channel_mode_set(state, 3, DSD_SCAN_MODE_DMR) == 0);
    (void)nfm_row_profile(state, 1, DSD_SCAN_OPT_BANDWIDTH, 12500, 0);
    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_ensure_analog_calls = g_ensure_digital_calls = 0;

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 1 && opts->frame_dmr && !opts->analog_only && tuned_analog_only == 0);
    assert(g_ensure_digital_calls == 1 && g_ensure_analog_calls == 0);

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(tuned_analog_only == 1 && tuned_nfm_width_hz == 12500);
    assert(state->lcn_freq_roll == 2 && opts->analog_only == 1 && opts->monitor_input_audio == 1 && !opts->frame_dmr);
    assert(opts->analog_nfm_bandwidth_hz == 12500 && dsd_scan_mode_active(state) == DSD_SCAN_MODE_NFM);
    assert(dsd_scan_mode_configured_view(state)->analog_nfm_bandwidth_hz == 20000);
    assert(g_ensure_analog_calls == 1);

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(tuned_analog_only == 1 && tuned_nfm_width_hz == 20000 && opts->analog_nfm_bandwidth_hz == 20000);
    assert(g_ensure_analog_calls == 2);

    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(tuned_analog_only == 0 && tuned_nfm_width_hz == 20000);
    assert(state->lcn_freq_roll == 4 && opts->frame_dmr && !opts->analog_only && !opts->monitor_input_audio);
    assert(g_ensure_digital_calls == 2);

    /* A manual step wraps onto the nfm row with its width. */
    assert(dsd_engine_channel_scan_step_manual(opts, state) == 1);
    assert(state->lcn_freq_roll == 1 && !opts->analog_only);
    assert(dsd_engine_channel_scan_step_manual(opts, state) == 1);
    assert(state->lcn_freq_roll == 2 && opts->analog_only && opts->analog_nfm_bandwidth_hz == 12500);
    /* A row that cannot be tuned is skipped with the nfm row left in force. */
    tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    assert(dsd_engine_channel_scan_step(opts, state) == -1);
    assert(state->lcn_freq_roll == 3 && opts->analog_only && opts->analog_nfm_bandwidth_hz == 12500);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;

    dsd_engine_channel_scan_leave(opts, state);
    assert(!opts->analog_only && opts->frame_dmr && opts->analog_nfm_bandwidth_hz == 20000);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* The configured NFM width is what an nfm row without its own width tunes with, on a digital session too. The width
 * command edits it without suspending the scope, so an edit made while such a row's tune is outstanding restages the
 * tune, as any configured acquisition change does, rather than committing the row on a front end tuned for the old
 * width. A width edit under a digital row's outstanding tune is no acquisition change there, and commits. */
static void
test_nfm_row_restages_after_a_configured_width_edit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    opts->analog_nfm_bandwidth_hz = 20000;
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 2;
    for (int row = 0; row < 2; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_NFM) == 0);
    expected_nxdn = 0;

    /* The DMR row's tune is outstanding when the width changes: it commits as staged. */
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 16000) == 1);
    int before = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before && state->lcn_freq_roll == 1 && opts->frame_dmr && !opts->analog_only);
    assert(opts->analog_nfm_bandwidth_hz == 16000);

    /* The nfm row's tune carried 16 kHz; an edit to 11.25 kHz before it lands restages it at the new width. */
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(tuned_analog_only == 1 && tuned_nfm_width_hz == 16000);
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 11250) == 1);
    before = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1);
    assert(tunes == before && state->lcn_freq_roll == 1);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before + 1 && tuned_analog_only == 1 && tuned_nfm_width_hz == 11250);
    assert(state->lcn_freq_roll == 2 && opts->analog_only && opts->analog_nfm_bandwidth_hz == 11250);

    dsd_engine_channel_scan_leave(opts, state);
    assert(!opts->analog_only && opts->frame_dmr && opts->analog_nfm_bandwidth_hz == 11250);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* A command acting for the row still on air while the next row's tune is outstanding asks the RTL front end for a
 * receive family live: the width command or a config apply republishing an nfm row's monitor, or a config apply
 * republishing a DMR row's symbol profile on the digital family. The front end takes it as the newer word on the family
 * and lands the staged retune with neither the family nor the symbol profile it carries, so the incoming row would
 * commit on the outgoing row's family: DMR decoded from the analog monitor, or an nfm row played from the digital
 * discriminator. The commit restages the row instead, and its retry lands the row's own family. A request made before
 * the row's tune was queued is older than its retune, which lands over it, and the row commits as staged. */
static void
test_row_restages_after_a_live_family_request(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 2;
    for (int row = 0; row < 2; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_DMR) == 0);
    const dsd_rtl_stream_metrics_hooks hooks = {.output_rate_hz = output_rate};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    expected_nxdn = 0;
    g_family_requests = 0U;

    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_step(opts, state) == 1);
    assert(state->lcn_freq_roll == 1 && opts->analog_only == 1 && dsd_scan_mode_active(state) == DSD_SCAN_MODE_NFM);

    /* The DMR row's tune is outstanding when the width command changes the configured NFM width the nfm row on air
     * runs, and republishes that row's monitor at it. */
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(tuned_analog_only == 0);
    assert(dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 12500) == 1);
    g_family_requests++;
    int before = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1);
    assert(tunes == before && state->lcn_freq_roll == 1 && opts->analog_only == 1 && !opts->frame_dmr);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before + 1 && tuned_analog_only == 0);
    assert(state->lcn_freq_roll == 2 && opts->frame_dmr && !opts->analog_only);

    /* The nfm row's tune is outstanding when a config apply republishes the DMR row's symbol profile, which asks for
     * the digital family. */
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    assert(tuned_analog_only == 1 && tuned_nfm_width_hz == 12500);
    g_family_requests++;
    before = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 1);
    assert(tunes == before && state->lcn_freq_roll == 2 && opts->frame_dmr && !opts->analog_only);
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before + 1 && tuned_analog_only == 1);
    assert(state->lcn_freq_roll == 1 && opts->analog_only == 1 && dsd_scan_mode_active(state) == DSD_SCAN_MODE_NFM);

    /* A request made before the DMR row's tune was queued: the row commits as staged. */
    g_family_requests++;
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_engine_channel_scan_step(opts, state) == 0);
    before = tunes;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_engine_channel_scan_pending(opts, state) == 0);
    assert(tunes == before && state->lcn_freq_roll == 2 && opts->frame_dmr && !opts->analog_only);

    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_family_requests = 0U;
    dsd_engine_channel_scan_leave(opts, state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* Four rows on one frequency: nfm with a 20 kHz width and its own -50 dB squelch, nfm with its squelch off, DMR with a
 * squelch, and nfm on the configured squelch. */
static void
nfm_warning_rows_setup(dsd_opts* opts, dsd_state* state, int audio_in_type, double configured_sql_db) {
    opts->scanner_mode = 1;
    opts->audio_in_type = audio_in_type;
    opts->wav_sample_rate = 48000;
    opts->frame_dmr = 1;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(configured_sql_db);
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 4;
    for (int row = 0; row < 4; row++) {
        *dsd_state_trunk_lcn_slot(state, row) = 150000000;
    }
    assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_channel_mode_set(state, 2, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_channel_mode_set(state, 3, DSD_SCAN_MODE_NFM) == 0);
    (void)nfm_row_profile(state, 0, DSD_SCAN_OPT_BANDWIDTH | DSD_SCAN_OPT_SQUELCH, 20000, -50);
    (void)nfm_row_profile(state, 1, DSD_SCAN_OPT_SQUELCH, 0, 0);
    (void)nfm_row_profile(state, 2, DSD_SCAN_OPT_SQUELCH, 0, 0);
    g_analog_warnings = 0;
    g_squelch_warnings = 0;
    expected_nxdn = 0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
}

static void
nfm_warning_rows_visit(dsd_opts* opts, dsd_state* state, int visits) {
    for (int visit = 0; visit < visits; visit++) {
        (void)dsd_engine_channel_scan_step(opts, state);
    }
}

/* What the nfm rows owe the operator: once per row per map when the scan starts, a squelch that holds on noise; and
 * once per map and DSP rate, a width on an input with no demodulator for it or a width the running DSP rate cannot
 * filter. Digital rows and well-set nfm rows say nothing. */
static const char* g_nfm_warning_input_dev = "";

static int
nfm_row_warnings(int audio_in_type, int dsp_rate_hz, double configured_sql_db) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    nfm_warning_rows_setup(opts, state, audio_in_type, configured_sql_db);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", g_nfm_warning_input_dev);
    g_scan_dsp_rate_hz = dsp_rate_hz;
    nfm_warning_rows_visit(opts, state, 8);
    dsd_engine_channel_scan_leave(opts, state);
    g_scan_dsp_rate_hz = 0;
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
    return g_analog_warnings;
}

static void
test_nfm_row_warnings_once_per_row(void) {
    /* RTL input at a 48 kHz DSP rate with a -60 dB default: only row 2 (squelch off) holds on noise, and the text
       says what does move on from it: the visit cap or the operator, never -t, which its carrier keeps re-arming. */
    assert(nfm_row_warnings(AUDIO_IN_RTL, 48000, -60.0) == 1);
    assert(strstr(g_analog_warning_rows[0], "Scan channel 2 (150.000000 MHz): the analog channel's squelch is off"));
    assert(strstr(g_analog_warning_rows[0], "until --scan-max-visit-ms or a manual advance or avoid moves on"));
    assert(!strstr(g_analog_warning_rows[0], "-t "));
    /* The default is open as well (-110 dB, the unset level): row 4 inherits it. */
    assert(nfm_row_warnings(AUDIO_IN_RTL, 48000, -110.0) == 2);
    assert(strstr(g_analog_warning_rows[1], "Scan channel 4 "));
    /* A 16 kHz DSP rate cannot filter row 1's 20 kHz: named with the validator's text, after the squelch. */
    assert(nfm_row_warnings(AUDIO_IN_RTL, 16000, -60.0) == 2);
    assert(strstr(g_analog_warning_rows[1], "Scan channel 1 (150.000000 MHz): NFM bandwidth 20 kHz does not fit the "
                                            "16 kHz DSP rate"));
    assert(strstr(g_analog_warning_rows[1], "until then it is skipped at every visit"));
    /* The fix is the one the input allows (issue #525's wording): an RTL-SDR's DSP bandwidth is its rate, a SoapySDR or
       Airspy device's capture rate only follows the DSP bandwidth, and an I/Q replay's is fixed. */
    assert(strstr(g_analog_warning_rows[1], "set the RTL DSP bandwidth to 24 or 48 kHz"));
    g_nfm_warning_input_dev = "soapy:driver=airspy";
    assert(nfm_row_warnings(AUDIO_IN_RTL, 16000, -60.0) == 2);
    assert(strstr(g_analog_warning_rows[1], "raise the DSP bandwidth or narrow the NFM width"));
    assert(!strstr(g_analog_warning_rows[1], "RTL DSP bandwidth"));
    g_nfm_warning_input_dev = "iqreplay:capture.iq.json";
    assert(nfm_row_warnings(AUDIO_IN_RTL, 16000, -60.0) == 2);
    assert(strstr(g_analog_warning_rows[1], "narrow the NFM width"));
    assert(!strstr(g_analog_warning_rows[1], "DSP bandwidth"));
    g_nfm_warning_input_dev = "";
    /* Audio input: the width has nothing to act on, while an nfm row's squelch still gates its monitor and
       carrier there, so only the digital row's squelch draws the #521 warning. */
    assert(nfm_row_warnings(AUDIO_IN_WAV, 0, -60.0) == 2);
    assert(strstr(g_analog_warning_rows[1], "Scan channel 1 (150.000000 MHz): --nfm-bandwidth-hz 20000 has no effect"));
    assert(g_squelch_warnings == 1 && strstr(g_squelch_warning_rows[0], "Scan channel 3 "));
}

/* The width checks follow the DSP rate a width must fit. An RTL stream that has published none yet leaves them to a
 * later row start rather than skip the width check for good, and a new rate names the widths again: a width that fit
 * the old rate may not fit the new one, and the scanner skips such a row quietly. The squelch does not depend on the
 * rate: it is named once, when the scan starts, whatever the rate does after. */
static void
test_nfm_row_warnings_follow_the_dsp_rate(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    nfm_warning_rows_setup(opts, state, AUDIO_IN_RTL, -60.0);
    g_scan_dsp_rate_hz = 0;
    nfm_warning_rows_visit(opts, state, 4);
    assert(g_analog_warnings == 1);
    assert(strstr(g_analog_warning_rows[0], "Scan channel 2 (150.000000 MHz): the analog channel's squelch is off"));
    assert(state->ui_msg[0] == '\0');
    /* The rate arrives: row 1's 20 kHz, once, in the log and on the status line every frontend shows. */
    g_scan_dsp_rate_hz = 16000;
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 2);
    assert(strstr(g_analog_warning_rows[1], "Scan channel 1 (150.000000 MHz): NFM bandwidth 20 kHz does not fit the "
                                            "16 kHz DSP rate"));
    assert(strcmp(state->ui_msg, "Skipped at every visit: Scan channel 1 (150.000000 MHz): NFM 20 kHz does not fit the "
                                 "16 kHz DSP rate")
           == 0);
    assert(state->ui_msg_expire > 0);
    /* A 24 kHz rate fits the 20 kHz width, and the squelch is not named again. */
    state->ui_msg[0] = '\0';
    g_scan_dsp_rate_hz = 24000;
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 2);
    assert(state->ui_msg[0] == '\0');
    /* Back to 16 kHz: the width is named again, the squelch still not. */
    g_scan_dsp_rate_hz = 16000;
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 3);
    assert(strstr(g_analog_warning_rows[2], "Scan channel 1 (150.000000 MHz): NFM bandwidth 20 kHz"));
    dsd_engine_channel_scan_leave(opts, state);
    g_scan_dsp_rate_hz = 0;
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* An nfm row that sets no width of its own runs the configured NFM width, which nothing holds to the DSP rate on a
 * digital session: a rate that cannot filter it skips those rows at every visit, so the scan start names them with
 * that width beside the rows whose own width does not fit, and a changed configured width names again only the rows
 * that run it. */
static void
test_nfm_row_warnings_for_the_configured_width(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    nfm_warning_rows_setup(opts, state, AUDIO_IN_RTL, -60.0);
    opts->analog_nfm_bandwidth_hz = 16000;
    g_scan_dsp_rate_hz = 16000;
    nfm_warning_rows_visit(opts, state, 8);
    /* Row 2's open squelch, row 1's own 20 kHz, and rows 2 and 4, which run the configured 16 kHz. */
    assert(g_analog_warnings == 4);
    assert(strstr(g_analog_warning_rows[1], "Scan channel 1 (150.000000 MHz): NFM bandwidth 20 kHz does not fit"));
    assert(strstr(g_analog_warning_rows[2], "Scan channel 2 (150.000000 MHz): it sets no NFM width of its own, and the "
                                            "configured NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate"));
    assert(strstr(g_analog_warning_rows[2], "set the RTL DSP bandwidth to 24 or 48 kHz; until then it is skipped at "
                                            "every visit"));
    assert(strstr(g_analog_warning_rows[3], "Scan channel 4 (150.000000 MHz): it sets no NFM width of its own"));
    assert(strcmp(state->ui_msg, "Skipped at every visit: Scan channel 1 (150.000000 MHz) and 2 more: NFM 20 kHz does "
                                 "not fit the 16 kHz DSP rate")
           == 0);
    /* A configured width the rate fits names nothing; one it does not names the two rows again, not row 1. */
    assert(dsd_scan_mode_configured_view(state) != NULL);
    (void)dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 12500);
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 4);
    (void)dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 20000);
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 6);
    assert(strstr(g_analog_warning_rows[4], "Scan channel 2 (150.000000 MHz): it sets no NFM width of its own, and the "
                                            "configured NFM bandwidth 20 kHz does not fit"));
    assert(strstr(g_analog_warning_rows[5], "Scan channel 4 "));
    /* The unset default runs at any rate (DSP-limited where it cannot filter), so it names nothing. */
    (void)dsd_scan_mode_set_configured_nfm_bandwidth(opts, state, 0);
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 6);
    dsd_engine_channel_scan_leave(opts, state);
    g_scan_dsp_rate_hz = 0;
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
}

/* DSD_NEO_CHANNEL_LPF=0 turns off the channel filter every explicit width needs, and the front end refuses such a
 * width at any rate, so a row with its own width is skipped at every visit even where the rate fits it: the scan start
 * names it once with that reason, in the log and on the status line, as it names a width the rate cannot fit. A row
 * on the unset default width still runs, and names nothing. */
static void
test_nfm_row_warnings_under_the_channel_lpf_override(void) {
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    nfm_warning_rows_setup(opts, state, AUDIO_IN_RTL, -60.0);
    g_scan_dsp_rate_hz = 48000;
    nfm_warning_rows_visit(opts, state, 8);
    /* Row 2's open squelch, then row 1's 20 kHz, which the 48 kHz rate fits. */
    assert(g_analog_warnings == 2);
    assert(strstr(g_analog_warning_rows[1], "Scan channel 1 (150.000000 MHz): NFM bandwidth 20 kHz needs the channel "
                                            "filter, but DSD_NEO_CHANNEL_LPF=0 turns it off"));
    assert(strstr(g_analog_warning_rows[1], "until then it is skipped at every visit"));
    assert(strcmp(state->ui_msg, "Skipped at every visit: Scan channel 1 (150.000000 MHz): NFM 20 kHz needs the "
                                 "filter DSD_NEO_CHANNEL_LPF=0 turns off")
           == 0);
    /* Said once: more rotations at the same rate name nothing more. */
    nfm_warning_rows_visit(opts, state, 8);
    assert(g_analog_warnings == 2);
    dsd_engine_channel_scan_leave(opts, state);
    g_scan_dsp_rate_hz = 0;
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    tunes = reset_count = 0;
    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
}

int
main(void) {
    dsd_neo_log_set_tap(count_squelch_warnings, NULL);
    test_leave_restores_configured_receive_family();
    test_leave_retimes_the_digital_landing();
    test_leave_family_switch_drops_partial_analog_block();
    test_option_commit_boundary();
    test_option_only_rows_and_policy_edits();
    test_row_max_visit_override_and_inherit();
    test_row_squelch_threshold_off_inherit();
    test_row_squelch_warns_once_per_row_on_pcm_input();
    test_rx_tone_row_commit_and_step_clear();
    test_nfm_rows_switch_family_width_and_sink();
    test_nfm_row_restages_after_a_configured_width_edit();
    test_row_restages_after_a_live_family_request();
    test_nfm_row_warnings_once_per_row();
    test_nfm_row_warnings_follow_the_dsp_rate();
    test_nfm_row_warnings_for_the_configured_width();
    test_nfm_row_warnings_under_the_channel_lpf_override();
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
