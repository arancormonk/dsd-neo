// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/key_set.h>
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
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint64_t request;
    uint64_t map_sequence;
    int row;
    int retry;
    dsd_scan_mode mode;
    dsd_scan_settings configured;
} channel_scan;

static channel_scan*
channel_scan_get(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(channel_scan, state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN);
}

static int
channel_scan_waiting(const dsd_state* state) {
    const channel_scan* scan = channel_scan_get(state);
    return scan && (scan->request != 0 || scan->retry);
}

static int channel_scan_start_row(dsd_opts* opts, dsd_state* state, int row);

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
    dsd_scan_settings latest;
    dsd_scan_mode_configured(opts, state, &latest);
    if (!dsd_scan_settings_equal(&latest, &scan->configured, 1)) {
        /* A configured setting changed while tuning. Stage the new effective
         * profile in a fresh request before any frame can use it. */
        scan->request = 0;
        scan->retry = 1;
        return 0;
    }
    /* End calls with the outgoing row's keys and label still installed. */
    channel_scan_end_calls(opts, state);
    dsd_engine_reset_no_carrier_state(opts, state);
    if (dsd_scan_mode_enter(opts, state, scan->mode) != 0) {
        return -1;
    }
    state->lcn_freq_roll = scan->row + 1;
    (void)dsd_scan_row_keys_apply(state, scan->row);
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    state->nxdn_last_ran = -1;
    dsd_scan_voice_gate_note_retune(state, state->last_cc_sync_time_m);
    scan->request = 0;
    return 1;
}

int
dsd_engine_channel_scan_pending(dsd_opts* opts, dsd_state* state) {
    channel_scan* scan = channel_scan_get(state);
    if (!scan) {
        return 0;
    }
    if (scan->retry) {
        scan->retry = 0;
        if (opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1
            && scan->map_sequence == state->trunk_chan_map_seq) {
            (void)channel_scan_start_row(opts, state, scan->row);
        }
        return channel_scan_waiting(state);
    }
    if (!scan->request) {
        return 0;
    }
    const dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(scan->request, NULL);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return 1;
    }
    if (result == DSD_TRUNK_TUNE_RESULT_OK && scan->map_sequence == state->trunk_chan_map_seq && opts->scanner_mode == 1
        && opts->trunk_scan_enabled != 1) {
        scan->request = 0;
        (void)channel_scan_commit(opts, state, scan);
        return channel_scan_waiting(state);
    }
    channel_scan_end_calls(opts, state);
    scan->request = 0;
    return 0;
}

int
dsd_engine_channel_scan_sync_ready(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    const channel_scan* scan = channel_scan_get(state);
    if (!scan || (!scan->request && !scan->retry)) {
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

static int
channel_scan_start_row(dsd_opts* opts, dsd_state* state, int row) {
    channel_scan* scan = channel_scan_get(state);
    if (row < 0) {
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
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, scan, free);
    }
    scan->row = row;
    scan->mode = dsd_channel_mode_get(state, (size_t)row);
    scan->map_sequence = state->trunk_chan_map_seq;
    dsd_scan_settings before;
    dsd_scan_settings next;
    dsd_scan_settings_capture(opts, state, &before);
    if (dsd_scan_mode_prepare(opts, state, scan->mode, &next) != 0) {
        return -1;
    }
    dsd_scan_mode_configured(opts, state, &scan->configured);
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
    scan->request = 0;
    return -1;
}

int
dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->trunk_scan_enabled == 1 || state->lcn_freq_count <= 0) {
        return 0;
    }
    if (channel_scan_waiting(state)) {
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

void
dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state) {
    const int active = channel_scan_get(state) != NULL || dsd_scan_mode_configured_view(state) != NULL
                       || dsd_scan_mode_updating(state);
    if (active) {
        channel_scan_end_calls(opts, state);
    }
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, NULL, NULL);
    dsd_scan_mode_leave(opts, state);
    if (active) {
        dsd_frame_sync_reset_acquisition(opts, state, 1);
        if (opts->audio_in_type == AUDIO_IN_RTL) {
            const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
            const int filter =
                opts->analog_only || !dsd_opts_has_digital_decode_mode(opts)
                    ? DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE
                    : dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, state->rf_mod);
            (void)dsd_rtl_stream_metrics_hook_apply_demod_profile(state->rf_mod == 1, profile.symbol_rate_hz,
                                                                  profile.levels, filter, state->samplesPerSymbol);
        }
    }
}
