// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
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
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint64_t request;
    uint64_t map_sequence;
    int row;
    int retry;
    int needs_commit;
    dsd_scan_mode mode;
    dsd_scan_settings configured;
    dsd_scan_key_change keys;
    uint64_t key_epoch;
    /* The map generation, and the DSP rate, whose rows were last checked against the input (issues #521, #526). */
    uint64_t rows_checked_map;
    int rows_checked_rate_hz;
    int rows_checked;
} channel_scan;

static void
channel_scan_free(void* ptr) {
    channel_scan* scan = (channel_scan*)ptr;
    if (!scan) {
        return;
    }
    dsd_scan_key_change_clear(&scan->keys);
    free(scan);
}

static channel_scan*
channel_scan_get(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(channel_scan, state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN);
}

int
dsd_engine_channel_scan_waiting(const dsd_state* state) {
    const channel_scan* scan = channel_scan_get(state);
    return scan && (scan->request != 0 || scan->retry);
}

static int channel_scan_start_row(dsd_opts* opts, dsd_state* state, int row);

static int
channel_scan_row_is_current(const dsd_opts* opts, const dsd_state* state, const channel_scan* scan) {
    return opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1 && scan->map_sequence == state->trunk_chan_map_seq;
}

static void
channel_scan_end_calls(dsd_opts* opts, dsd_state* state) {
    const double now = dsd_time_now_monotonic_s();
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end(state, (uint8_t)slot, now) > 0) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
    (void)dsd_recent_activity_clear_all(state);
    opts->trunk_is_tuned = 0;
}

static int
channel_scan_commit(dsd_opts* opts, dsd_state* state, channel_scan* scan) {
    /* Row commits run only in conventional scanner mode. Entry into that mode
     * is guarded (or precedes startup), and P25 recovery admission excludes it,
     * so policy installation cannot overlap an admitted watchdog tick (#554). */
    /* Hardware has moved. Even if a later retry rolls back, frames cannot use
     * the outgoing profile until a row is successfully committed. */
    scan->needs_commit = 1;
    dsd_scan_settings latest;
    dsd_scan_mode_configured(opts, state, &latest);
    if (!dsd_scan_settings_equal(&latest, &scan->configured, 1) || scan->key_epoch != state->enc_lockout_key_epoch) {
        /* A configured setting changed while tuning. Stage the new effective
         * profile in a fresh request before any frame can use it. */
        scan->request = 0;
        scan->retry = 1;
        return 0;
    }
    const int outgoing_force = state->M;
    /* End calls with the outgoing row's keys and label still installed. */
    channel_scan_end_calls(opts, state);
    dsd_engine_reset_no_carrier_state(opts, state);
    if (dsd_scan_mode_enter(opts, state, scan->mode) != 0) {
        scan->retry = 1;
        return -1;
    }
    state->lcn_freq_roll = scan->row + 1;
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)scan->row);
    (void)dsd_scan_mode_options(opts, state, profile ? &profile->values : NULL);
    dsd_engine_scan_ensure_output(opts);
    dsd_scan_groups_enter(state, profile);
    const int maps_changed = dsd_scan_maps_enter(state, profile);
    const int keys_changed = dsd_scan_key_change_commit(state, &scan->keys);
    if (keys_changed || maps_changed || outgoing_force != state->M) {
        dsd_enc_lockout_bump_key_epoch(state);
    }
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    state->nxdn_last_ran = -1;
    dsd_scan_voice_gate_note_retune(state, state->last_cc_sync_time_m);
    scan->request = 0;
    scan->needs_commit = 0;
    return 1;
}

int
dsd_engine_channel_scan_pending(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    channel_scan* scan = channel_scan_get(state);
    if (!scan) {
        return 0;
    }
    if (scan->retry) {
        if (channel_scan_row_is_current(opts, state, scan)) {
            (void)channel_scan_start_row(opts, state, scan->row);
        } else {
            scan->retry = 0;
        }
        return dsd_engine_channel_scan_waiting(state);
    }
    if (!scan->request) {
        return 0;
    }
    const dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(scan->request, NULL);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return 1;
    }
    if (result == DSD_TRUNK_TUNE_RESULT_OK && channel_scan_row_is_current(opts, state, scan)) {
        scan->request = 0;
        (void)channel_scan_commit(opts, state, scan);
        return dsd_engine_channel_scan_waiting(state);
    }
    channel_scan_end_calls(opts, state);
    if (result == DSD_TRUNK_TUNE_RESULT_FAILED && scan->map_sequence == state->trunk_chan_map_seq) {
        state->lcn_freq_roll = scan->row + 1;
    }
    scan->request = 0;
    return 0;
}

int
dsd_engine_channel_scan_service_sync(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    const channel_scan* scan = channel_scan_get(state);
    if (!scan || (!scan->request && !scan->retry && !scan->needs_commit)) {
        return 1;
    }
    (void)dsd_engine_channel_scan_pending(opts, state);
    /* Even a completed tune invalidates sync collected before the row commit. */
    state->synctype = DSD_SYNC_NONE;
    return 0;
}

static int
channel_scan_next_row(const dsd_state* state) {
    int row = state->lcn_freq_roll;
    if (row < 0 || row >= state->lcn_freq_count) {
        row = 0;
    }
    if (state->lcn_avoid_count) {
        row = dsd_state_trunk_lcn_next_unavoided(state, row);
    }
    return row;
}

/* A row squelch gates the RTL demodulator. Any other input has no demodulator for it to gate, so
 * it cannot gate digital acquisition; the threshold in dsd_opts only reaches the analog input
 * monitor (-8, with audio output on) and the carrier activity that monitor stamps. */
static void
channel_scan_warn_row_squelch(const dsd_opts* opts, int row, long freq, const dsd_scan_option_values* values) {
    if (opts->audio_in_type == AUDIO_IN_RTL || !values || !(values->present & DSD_SCAN_OPT_SQUELCH)) {
        return;
    }
    LOG_WARN("WARNING: Scan channel %d (%.6lf MHz): --squelch-db %d cannot gate digital acquisition "
             "without a radio input; here it gates only the analog input monitor (-8) and the carrier "
             "activity it stamps.\n",
             row + 1, (double)freq / 1000000.0, values->squelch_db);
}

/* What the rows owe the operator, said once per row when a scan (or a newly imported map) starts, and again when the
 * DSP rate an analog row width must fit changes. An RTL stream that has published no rate yet leaves the check to the
 * next row start: a width would go unchecked, and the rows are skipped quietly at every visit it refuses
 * (dsd_engine_scan_tune_to_freq()). */
static void
channel_scan_check_rows(const dsd_opts* opts, const dsd_state* state, channel_scan* scan) {
    const int dsp_rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    if (opts->audio_in_type == AUDIO_IN_RTL && dsp_rate_hz <= 0) {
        return;
    }
    if (scan->rows_checked && scan->rows_checked_map == state->trunk_chan_map_seq
        && scan->rows_checked_rate_hz == dsp_rate_hz) {
        return;
    }
    scan->rows_checked = 1;
    scan->rows_checked_map = state->trunk_chan_map_seq;
    scan->rows_checked_rate_hz = dsp_rate_hz;
    for (int row = 0; row < state->lcn_freq_count; row++) {
        const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
        const dsd_scan_option_values* values = profile ? &profile->values : NULL;
        const long freq = *dsd_state_trunk_lcn_slot_const(state, row);
        /* An analog row's squelch gates exactly what it is for on any input: its monitor and carrier. */
        if (!dsd_scan_mode_is_analog(dsd_channel_mode_get(state, (size_t)row))) {
            channel_scan_warn_row_squelch(opts, row, freq, values);
            continue;
        }
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "Scan channel %d (%.6lf MHz)", row + 1, (double)freq / 1000000.0);
        (void)dsd_engine_scan_warn_analog_row(opts, state, values, dsp_rate_hz, label);
    }
}

/* The squelch an analog row runs with: its own, else the configured one. Off, or at -100 dB and below, holds the row
 * on noise until -t or the visit cap moves on. The -100 dB comparison allows for the rounding of the level's power. */
static int
channel_scan_analog_squelch_open(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    double level = configured ? configured->rtl_squelch_level : opts->rtl_squelch_level;
    if (row && (row->present & DSD_SCAN_OPT_SQUELCH)) {
        level = dsd_squelch_level_from_sql((double)row->squelch_db);
    }
    const double floor_level = dsd_squelch_level_from_sql(-100.0);
    return dsd_squelch_is_off(level) || level <= floor_level * (1.0 + 1e-9);
}

void
dsd_engine_scan_ensure_output(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    if (dsd_opts_is_analog_family(opts)) {
        (void)dsd_audio_ensure_analog_output(opts);
    } else {
        (void)dsd_audio_ensure_digital_output(opts);
    }
}

int
dsd_engine_scan_warn_analog_row(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                int dsp_rate_hz, const char* label) {
    if (!opts || !state || !label) {
        return 0;
    }
    int warnings = 0;
    if (channel_scan_analog_squelch_open(opts, state, row)) {
        LOG_WARN("WARNING: %s: the nfm row's squelch is off or at -100 dB or below, so noise holds it until -t or "
                 "--scan-max-visit-ms moves on; set --squelch-db on the row or a squelch level.\n",
                 label);
        warnings++;
    }
    if (!row || !(row->present & DSD_SCAN_OPT_BANDWIDTH)) {
        return warnings;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        LOG_WARN("WARNING: %s: --nfm-bandwidth-hz %d has no effect on audio input, which arrives demodulated; set "
                 "the demodulator's own passband (-B for a rigctl peer).\n",
                 label, row->channel_bw_hz);
        return warnings + 1;
    }
    char why[DSD_ANALOG_ERROR_TEXT_MAX];
    if (dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, row, dsp_rate_hz, why, sizeof why) != 0) {
        LOG_WARN("WARNING: %s: %s; the row is skipped at every visit.\n", label, why);
        warnings++;
    }
    return warnings;
}

static int
channel_scan_start_row(dsd_opts* opts, dsd_state* state, int row) {
    channel_scan* scan = channel_scan_get(state);
    if (row < 0 || row >= state->lcn_freq_count) {
        return 0;
    }
    const long freq = *dsd_state_trunk_lcn_slot(state, row);
    if (!freq) {
        state->lcn_freq_roll = row + 1;
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
        dsd_scan_voice_gate_note_retune(state, state->last_cc_sync_time_m);
        return 0;
    }
    if (!scan) {
        scan = (channel_scan*)calloc(1, sizeof(*scan));
        if (!scan) {
            return -1;
        }
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, scan, channel_scan_free);
    }
    channel_scan_check_rows(opts, state, scan);
    scan->row = row;
    scan->mode = dsd_channel_mode_get(state, (size_t)row);
    scan->map_sequence = state->trunk_chan_map_seq;
    dsd_scan_settings before;
    dsd_scan_settings next;
    dsd_scan_settings_capture(opts, state, &before);
    /* The row's own acquisition options (its analog channel width) are part of what the tune queues. */
    const dsd_scan_row_profile* row_profile = dsd_channel_profile_get(state, (size_t)row);
    if (dsd_scan_mode_prepare(opts, state, scan->mode, row_profile ? &row_profile->values : NULL, &next) != 0) {
        return -1;
    }
    dsd_scan_mode_configured(opts, state, &scan->configured);
    if (dsd_scan_groups_begin(state)
        || dsd_scan_key_change_prepare(state, dsd_state_trunk_lcn_keys_get(state, (size_t)row), &scan->keys)) {
        return -1;
    }
    scan->key_epoch = state->enc_lockout_key_epoch;
    /* A completed tune may already have moved the receiver. Keep its retry gate
     * closed through preparation failures; the new request takes over below. */
    scan->retry = 0;
    dsd_scan_settings_restore(&next, opts, state);
    const dsd_trunk_tune_result result =
        dsd_engine_scan_tune_to_freq(opts, state, freq, state->samplesPerSymbol, &scan->request);
    dsd_scan_settings_restore(&before, opts, state);
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        return channel_scan_commit(opts, state, scan);
    }
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return 0;
    }
    /* A failed second backend may already have moved the first one. Keep the tune
     * ledger's frame gate closed until a subsequent successful request recovers. */
    channel_scan_end_calls(opts, state);
    if (result == DSD_TRUNK_TUNE_RESULT_FAILED) {
        /* A bad row must not prevent a later successful row from recovering
         * the receiver and retiring the failed tune's frame gate. */
        state->lcn_freq_roll = row + 1;
    }
    scan->request = 0;
    return -1;
}

int
dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->trunk_scan_enabled == 1 || state->lcn_freq_count <= 0) {
        return 0;
    }
    if (dsd_engine_channel_scan_waiting(state)) {
        (void)dsd_engine_channel_scan_pending(opts, state);
        return 0;
    }
    return channel_scan_start_row(opts, state, channel_scan_next_row(state));
}

int
dsd_engine_channel_scan_step_manual(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->trunk_scan_enabled == 1 || state->lcn_freq_count <= 0) {
        return 0;
    }
    const channel_scan* scan = channel_scan_get(state);
    if (scan && (scan->request || scan->retry)) {
        return dsd_engine_channel_scan_step(opts, state);
    }
    int row = state->lcn_freq_roll;
    if (row < 0 || row >= state->lcn_freq_count) {
        row = 0;
    }
    for (int examined = 0; examined < state->lcn_freq_count; examined++) {
        const long freq = *dsd_state_trunk_lcn_slot(state, row);
        if (freq != 0 && !dsd_state_trunk_lcn_avoid_get(state, (size_t)row)) {
            const int result = channel_scan_start_row(opts, state, row);
            if (result >= 0) {
                LOG_INFO("Channel Cycle: tuning to %.06lf MHz\n", (double)freq / 1000000);
            }
            return result;
        }
        row = (row + 1) % state->lcn_freq_count;
    }
    return 0;
}

/* Put the RTL front end back on the configured receive family. Under -fA that is the configured analog profile
 * (demodulator and channel width, 0 meaning the default); otherwise the digital family and the symbol profile the
 * restored decoder runs on, in that order, so the demod thread switches family before it applies the profile.
 *
 * A front end still on the analog family (the -fA session whose configured mode was changed to a digital one while a
 * row ran) switches to digital only here, after the configured timing was saved for the analog family's output rate:
 * the monitor's resampled audio, or the rate a typed row's profile ran at. The decoder, and the profile it publishes,
 * are timed for the rate the digital family lands on instead, as svc_publish_symbol_profile() times a mode change
 * outside a row.
 *
 * A leave that switches the front end's family also drops the analog monitor block the decoder has part-collected
 * from the old family's output, as a decode-mode change between the families does. */
static void
channel_scan_restore_frontend(const dsd_opts* opts, dsd_state* state) {
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return;
    }
    const int analog_family_active = dsd_rtl_stream_metrics_hook_analog_family_active();
    if (dsd_opts_is_analog_family(opts)) {
        if (!analog_family_active) {
            dsd_symbol_analog_block_reset(state);
        }
        (void)dsd_rtl_stream_metrics_hook_apply_analog_profile(DSD_RX_FAMILY_ANALOG, opts->analog_demod,
                                                               dsd_opts_analog_width_hz(opts));
        return;
    }
    const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
    if (analog_family_active) {
        dsd_symbol_analog_block_reset(state);
        const unsigned int rate_hz = dsd_rtl_stream_metrics_hook_output_rate_for_family(
            DSD_RX_FAMILY_DIGITAL, state->rf_mod == 1, profile.symbol_rate_hz);
        if (rate_hz > 0U) {
            state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, (int)rate_hz);
            state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
        }
    }
    const int filter = opts->analog_only || !dsd_opts_has_digital_decode_mode(opts)
                           ? DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE
                           : dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, state->rf_mod);
    (void)dsd_rtl_stream_metrics_hook_apply_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0);
    (void)dsd_rtl_stream_metrics_hook_apply_demod_profile(state->rf_mod == 1, profile.symbol_rate_hz, profile.levels,
                                                          filter, state->samplesPerSymbol);
}

void
dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    const int active = channel_scan_get(state) != NULL || dsd_scan_mode_configured_view(state) != NULL
                       || dsd_scan_mode_updating(state);
    if (active) {
        channel_scan_end_calls(opts, state);
    }
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, NULL, NULL);
    dsd_scan_groups_leave(state);
    dsd_scan_maps_leave(state);
    dsd_scan_mode_leave(opts, state);
    if (active) {
        dsd_frame_sync_reset_acquisition(opts, state, opts->trunk_scan_enabled != 1);
        channel_scan_restore_frontend(opts, state);
    }
}
