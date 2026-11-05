// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/channel_scan_hooks.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "manual_tune_pending.h"
#include "scan_analog_internal.h"
#include "scan_entry_internal.h"
#include "scan_landing.h"
#include "scan_owner.h"
#include "scan_step_untyped.h"

/* One row's session edit (issue #518). */
typedef struct {
    int row;
    dsd_scan_row_edit edit;
} channel_scan_row_edit;

typedef struct {
    uint64_t request;
    /* The list the staged row belongs to (dsd_state::lcn_list_rev) and the frequency the row held when its tune was
       staged. A channel learned over the air moves the sparse map (trunk_chan_map_seq), never the list: the row is
       still the one its tune went to (issue #697). An in-place write to the row keeps the list, so the frequency says
       the row is another channel now. */
    uint64_t list_rev;
    long row_freq;
    int row;
    int retry;
    int needs_commit;
    dsd_scan_mode mode;
    /* The analog kind (dsd_analog_demod) the staged tune's settings run: an nfm row's FM, an am row's AM, or on a
     * blank row the configured kind (issue #524). It says which configured width that tune carries. */
    int row_kind;
    dsd_scan_settings configured;
    dsd_scan_key_change keys;
    uint64_t key_epoch;
    /* The live receive-family requests the front end had accepted when the staged tune was queued
     * (dsd_engine_scan_family_requests(); issue #526), and whether that tune attached a receive family, which any later
     * request supersedes (dsd_engine_scan_retune_attaches_family()). A tune that attached none (a typed digital row on
     * an analog session) only a later analog request supersedes while the scan runs (a scan leave, which supersedes
     * it too, ends the scan), which the stream's count of such supersedes says
     * (dsd_engine_scan_familyless_retune_supersedes(); issue #582). */
    uint32_t family_requests;
    uint32_t familyless_supersedes;
    int family_attached;
    /* The list whose rows were last checked against the input (issues #521, #526; dsd_state::lcn_list_rev, which a
     * channel learned over the air leaves alone, issue #697), and the DSP rate its analog row widths were last held to,
     * with the configured NFM and AM widths a row without its own runs (issue #526). */
    uint64_t rows_checked_list;
    int rows_checked;
    int widths_checked_rate_hz;
    int widths_checked_nfm_hz;
    int widths_checked_am_hz;
    int widths_checked;
    /* The row whose frequency the receiver last landed on, and the list it belongs to (issue #527): a failed start
     * moves lcn_freq_roll past a row the receiver never reached, so only a completed tune says where the receiver
     * is. */
    int tuned;
    int tuned_row;
    uint64_t tuned_list;
    long tuned_freq;
    /* The receiver left the carrier, but no row has committed since and so no boundary was noted: a staged tune's
     * rigctl leg moved the radio before its RTL leg went on asynchronously (PENDING), or a completed tune landed a row
     * that was then restaged (channel_scan_commit()). A commit notes its own boundary; a row that ends unlanded notes
     * the one this owes (channel_scan_note_unlanded_move()). */
    int left_carrier;
    /* The row whose scope is installed, its options in force (the last row committed), or -1 before the first. */
    int committed_row;
    /* The operator's session edits of rows' own settings ("this channel", issue #518), one per edited row, laid over the
     * row's options wherever they are read (channel_scan_row_values()). They go with this extension when the scanner
     * leaves, and so never outlive the map they were made on: a map change leaves first. edits_epoch moves with every
     * change, and a tune staged before one restages (channel_scan_staged_stale()), so it lands what the row now runs.
     * session names this scan to the editors (dsd_state::scan_row_session). */
    channel_scan_row_edit* edits;
    size_t edit_count;
    size_t edit_capacity;
    uint32_t edits_epoch;
    uint32_t staged_edits_epoch;
    uint32_t session;
    /* The options the staged row was prepared with (its session edit merged), which its tune's rigctl leg reads
     * (dsd_engine_scan_tuning_row_options()); staged_has_values 0 for a row with none. */
    dsd_scan_option_values staged_values;
    int staged_has_values;
    /* The cursor a resume found its row from (issue #671), which a refusal puts back, and why the last row start failed
       (DSD_SCAN_RESUME_FAILED_*): at its tune, refused or deferred, or around it, the row's settings, scopes or keys
       not prepared before it or its decode mode not entered after it landed. A refused resume reports it. */
    int resume_roll;
    int start_failure;
    /* The direction of the step the staged row belongs to (issue #666): -1 for a Previous, else forward (0, as a new
       scan starts, reads as forward). Every start sets it before it stages its row, and a restage of that row keeps it,
       so the row's failure, at its tune or late, is handled as that step's: going forward the cursor moves past the
       row, going back it stays and the row is the one the next Previous retries (channel_scan_row_refused()). */
    int step_dir;
    /* The tune the staged row belongs to: the request its first tune began, which a restage of that row keeps (an
       entry waiting on the row names it, issue #644), and the receiver's centre before that first tune, where a stream
       stopped while it is pending reopens (dsd_engine_channel_scan_stream_stopping()). */
    uint64_t attempt;
    uint32_t attempt_prior_hz;
} channel_scan;

static void
channel_scan_free(void* ptr) {
    channel_scan* scan = (channel_scan*)ptr;
    if (!scan) {
        return;
    }
    dsd_scan_key_change_clear(&scan->keys);
    free(scan->edits);
    free(scan);
}

static channel_scan*
channel_scan_get(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(channel_scan, state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN);
}

/* The scan's extension, created by the first row a scan starts. NULL when it cannot be allocated. */
static channel_scan*
channel_scan_obtain(dsd_state* state) {
    channel_scan* scan = channel_scan_get(state);
    if (scan) {
        return scan;
    }
    scan = (channel_scan*)calloc(1, sizeof(*scan));
    if (!scan) {
        return NULL;
    }
    scan->committed_row = -1;
    scan->session = dsd_scan_row_edit_new_session();
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, scan, channel_scan_free);
    return scan;
}

static channel_scan_row_edit*
channel_scan_find_edit(const channel_scan* scan, int row) {
    for (size_t i = 0; scan && i < scan->edit_count; i++) {
        if (scan->edits[i].row == row) {
            return &scan->edits[i];
        }
    }
    return NULL;
}

/* The options @p row runs: its profile's, with the operator's session edit laid over them (issue #518). NULL for a row
   with neither; @p scratch holds the merged copy when there is an edit. */
static const dsd_scan_option_values*
channel_scan_row_values(const dsd_state* state, int row, dsd_scan_option_values* scratch) {
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
    const dsd_scan_option_values* base = profile ? &profile->values : NULL;
    const channel_scan_row_edit* entry = channel_scan_find_edit(channel_scan_get(state), row);
    if (!entry || dsd_scan_row_edit_fields_edited(&entry->edit) == 0U) {
        return base;
    }
    dsd_scan_row_edit_apply(base, &entry->edit, dsd_channel_mode_get(state, (size_t)row), scratch);
    return scratch;
}

const dsd_scan_option_values*
dsd_engine_channel_scan_row_values(const dsd_state* state, int row, dsd_scan_option_values* scratch) {
    return (state && scratch && row >= 0) ? channel_scan_row_values(state, row, scratch) : NULL;
}

/* Publish the row whose scope is in force for the "this channel" editors. */
static void
channel_scan_publish_row(dsd_state* state, const channel_scan* scan) {
    const int row = scan->committed_row;
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
    const channel_scan_row_edit* entry = channel_scan_find_edit(scan, row);
    state->scan_row_session = scan->session;
    state->scan_row_index = row;
    state->scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN;
    state->scan_row_editable = (uint16_t)dsd_scan_row_edit_fields(dsd_channel_mode_get(state, (size_t)row), 0);
    state->scan_row_edited = (uint16_t)dsd_scan_row_edit_fields_edited(entry ? &entry->edit : NULL);
    state->scan_row_listed = (uint16_t)dsd_scan_row_edit_fields_listed(profile ? &profile->values : NULL, 0);
}

static void
channel_scan_clear_published_row(dsd_state* state) {
    if (state->scan_row_scanner != (uint8_t)DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN) {
        return;
    }
    state->scan_row_session = 0U;
    state->scan_row_index = -1;
    state->scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_NONE;
    state->scan_row_editable = 0U;
    state->scan_row_edited = 0U;
    state->scan_row_listed = 0U;
}

const dsd_scan_option_values*
dsd_engine_scan_tuning_row_options(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return NULL;
    }
    const channel_scan* scan = channel_scan_get(state);
    if (scan && opts->trunk_scan_enabled != 1) {
        return scan->staged_has_values ? &scan->staged_values : NULL;
    }
    return dsd_scan_mode_row_options(state);
}

int
dsd_engine_channel_scan_waiting(const dsd_state* state) {
    const channel_scan* scan = channel_scan_get(state);
    return scan && (scan->request != 0 || scan->retry);
}

static int channel_scan_start_row(dsd_opts* opts, dsd_state* state, int row);

/* Whether @p row of the list @p list_rev still holds @p freq: the same list (no replacement since), the row still in it
   and not rewritten in place. */
static int
channel_scan_row_still_listed(const dsd_state* state, uint64_t list_rev, int row, long freq) {
    return list_rev == state->lcn_list_rev && row >= 0 && row < state->lcn_freq_count
           && *dsd_state_trunk_lcn_slot_const(state, row) == freq;
}

/* A resume still pending (issue #671) owns its row as a running scan does: only its landing publishes the scan. */
static int
channel_scan_row_is_current(const dsd_opts* opts, const dsd_state* state, const channel_scan* scan) {
    return dsd_channel_scan_owns_tuner(opts, state) && opts->trunk_scan_enabled != 1
           && channel_scan_row_still_listed(state, scan->list_rev, scan->row, scan->row_freq);
}

/* What a row tune that failed with @p result comes to for an entry waiting on it (issue #644). */
static dsd_scan_goto_result
channel_scan_entry_failure(dsd_trunk_tune_result result) {
    return result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? DSD_SCAN_GOTO_DEFERRED : DSD_SCAN_GOTO_REFUSED;
}

/* End every slot's call and commit it, an NFM call first (dsd_engine_end_calls()). */
static void
channel_scan_end_calls(dsd_opts* opts, dsd_state* state) {
    dsd_engine_end_calls(opts, state, dsd_decode_now_mono_s(), DSD_CALL_END_EXPLICIT, 0);
    (void)dsd_recent_activity_clear_all(state);
    opts->trunk_is_tuned = 0;
}

/* A row that ends unlanded after the receiver moved (@p moved, or an earlier move still owed, scan->left_carrier): the
   receiver is on another carrier all the same, so this is a carrier boundary (issue #575) as a committed row's is, and
   what the carrier left sent is forgotten. Runs before the calls end, as a commit's boundary does. */
static void
channel_scan_note_unlanded_move(dsd_opts* opts, dsd_state* state, channel_scan* scan, int moved) {
    if (moved || scan->left_carrier) {
        dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_SCAN_STEP, 0);
    }
    scan->left_carrier = 0;
}

/* Whether the tune staged for the row carries a configured channel width (issue #526): an untyped row's does on the
 * analog family the configured options select, and an analog row's unless it sets a width of its own, whatever the
 * configured family. A typed digital row filters with its own channel profile. */
static int
channel_scan_row_runs_configured_width(const dsd_state* state, const channel_scan* scan) {
    if (scan->mode == DSD_SCAN_MODE_INHERIT) {
        return scan->configured.analog_only == 1;
    }
    if (!dsd_scan_mode_is_analog(scan->mode)) {
        return 0;
    }
    dsd_scan_option_values scratch;
    const dsd_scan_option_values* values = channel_scan_row_values(state, scan->row, &scratch);
    return !(values && (values->present & DSD_SCAN_OPT_BANDWIDTH));
}

/* Whether a configured setting the staged tune was prepared from changed while it was outstanding. A configured channel
 * width counts only where the tune carries it (channel_scan_row_runs_configured_width()), and only the width of the
 * analog kind the row runs (issue #524): an nfm row's NFM width, an am row's AM width, a blank row's width of the
 * configured kind. That is not always the one dsd_scan_settings_equal() compares, the configured kind's whenever the
 * configured family is analog (an nfm row on an AM session carries the NFM width). */
static int
channel_scan_configured_changed(const dsd_state* state, const dsd_scan_settings* latest, const channel_scan* scan) {
    dsd_scan_settings others = *latest;
    others.analog_nfm_bandwidth_hz = scan->configured.analog_nfm_bandwidth_hz;
    others.analog_am_bandwidth_hz = scan->configured.analog_am_bandwidth_hz;
    if (!dsd_scan_settings_equal(&others, &scan->configured, 1)) {
        return 1;
    }
    if (!channel_scan_row_runs_configured_width(state, scan)) {
        return 0;
    }
    if (scan->row_kind == DSD_ANALOG_DEMOD_AM) {
        return latest->analog_am_bandwidth_hz != scan->configured.analog_am_bandwidth_hz;
    }
    return latest->analog_nfm_bandwidth_hz != scan->configured.analog_nfm_bandwidth_hz;
}

/* Whether the tune staged for the row no longer describes what it should land: a configured setting or the keyring it
 * was prepared from changed while it was outstanding, or a live receive-family request superseded the family its
 * retune carries. That request acts for the row still in scope (a width edit or a config apply republishing the
 * outgoing analog row's monitor), yet the front end takes it as the newer word on the family and lands the staged
 * retune with neither its family nor its symbol profile (rtl_stream_prepare_retune_analog_profile_for_target()): a
 * digital row would otherwise commit on the analog monitor, or an nfm or am row on the digital family (issue #526). A
 * retune that carries no family (a typed digital row on an analog session) lands its symbol profile whatever the
 * digital requests, but a later analog request asked for the monitor, and the front end lands that retune with its
 * centre only (issue #582): the row would commit on the monitor, so it is restaged for that request alone. */
static int
channel_scan_staged_stale(const dsd_opts* opts, const dsd_state* state, const channel_scan* scan) {
    dsd_scan_settings latest;
    dsd_scan_mode_configured(opts, state, &latest);
    if (channel_scan_configured_changed(state, &latest, scan) || scan->key_epoch != state->enc_lockout_key_epoch
        || scan->staged_edits_epoch != scan->edits_epoch) {
        return 1;
    }
    if (scan->family_attached) {
        return scan->family_requests != dsd_engine_scan_family_requests(opts);
    }
    return scan->familyless_supersedes != dsd_engine_scan_familyless_retune_supersedes(opts);
}

static int
channel_scan_commit(dsd_opts* opts, dsd_state* state, channel_scan* scan) {
    /* Row commits run only in conventional scanner mode. Entry into that mode
     * is guarded (or precedes startup), and P25 recovery admission excludes it,
     * so policy installation cannot overlap an admitted watchdog tick (#554). */
    /* Hardware has moved. Even if a later retry rolls back, frames cannot use
     * the outgoing profile until a row is successfully committed. */
    scan->needs_commit = 1;
    scan->tuned = 1;
    scan->tuned_row = scan->row;
    scan->tuned_list = scan->list_rev;
    scan->tuned_freq = scan->row_freq;
    if (channel_scan_staged_stale(opts, state, scan)) {
        /* Stage the new effective profile in a fresh request before any frame
         * can use it. The receiver has left the carrier already: the row's commit
         * or its end unlanded notes the boundary. */
        scan->request = 0;
        scan->retry = 1;
        scan->left_carrier = 1;
        return 0;
    }
    const int outgoing_force = state->M;
    /* The next row is another carrier: the carrier boundary (issue #575) ends the calls with the outgoing row's keys and
       label still installed, and forgets what that carrier left. */
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_SCAN_STEP, 0);
    scan->left_carrier = 0;
    channel_scan_end_calls(opts, state);
    dsd_engine_reset_no_carrier_state(opts, state);
    if (dsd_scan_mode_enter(opts, state, scan->mode) != 0) {
        scan->retry = 1;
        return -1;
    }
    state->lcn_freq_roll = scan->row + 1;
    dsd_state_lcn_on_air_note(state, scan->row);
    /* The scan has the tuner on a row of its own (issue #644): the operator's calls ended above, so a start that found
       no list, or turned the scanner on over a manual tune, stops filing its calls as the operator's here. */
    state->operator_tuned = 0U;
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)scan->row);
    dsd_scan_option_values values_scratch;
    (void)dsd_scan_mode_options(opts, state, channel_scan_row_values(state, scan->row, &values_scratch));
    scan->committed_row = scan->row;
    channel_scan_publish_row(state, scan);
    dsd_engine_scan_ensure_output(opts, state);
    dsd_scan_groups_enter(state, profile);
    const int maps_changed = dsd_scan_maps_enter(state, profile);
    const int keys_changed = dsd_scan_key_change_commit(state, &scan->keys);
    if (keys_changed || maps_changed || outgoing_force != state->M) {
        dsd_enc_lockout_bump_key_epoch(state);
    }
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    state->nxdn_last_ran = -1;
    dsd_scan_voice_gate_note_retune(state, state->last_cc_sync_time_m);
    scan->request = 0;
    scan->needs_commit = 0;
    dsd_engine_channel_scan_resume_landed(opts, state);
    /* The row landed and committed: an analog call on audio input is heard on its frequency (issue #667 D9), and an
       entry waiting on it holds it (issue #644). */
    dsd_engine_scan_landing_note_row(state, scan->row, *dsd_state_trunk_lcn_slot(state, scan->row));
    dsd_engine_scan_entry_landed(state, scan->row, scan->attempt);
    return 1;
}

/* A resume still pending (issue #671) leaves scanner_mode 0, so no call is named after a row the scan has not reached.
   The row's tune and commit read it as the running scan's (the tune's rigctl and RTL legs, the row's receive profile),
   so they run as one; no frame decodes meanwhile, since a typed row outstanding gates sync
   (dsd_engine_channel_scan_service_sync()). The landing publishes the scan; otherwise scanner_mode goes back to 0. */
static int
channel_scan_resume_owns(dsd_opts* opts, const dsd_state* state) {
    if (!state->lcn_scan_resume_pending) {
        return 0;
    }
    opts->scanner_mode = 1;
    return 1;
}

static void
channel_scan_resume_disowns(dsd_opts* opts, const dsd_state* state, int owned) {
    if (owned && state->lcn_scan_resume_pending) {
        opts->scanner_mode = 0;
    }
}

/* channel_scan_start_row() and channel_scan_commit() for a running scan or, owned as one, a resume still pending. */
static int
channel_scan_start_row_owned(dsd_opts* opts, dsd_state* state, int row) {
    const int owned = channel_scan_resume_owns(opts, state);
    const int result = channel_scan_start_row(opts, state, row);
    channel_scan_resume_disowns(opts, state, owned);
    return result;
}

static int
channel_scan_commit_owned(dsd_opts* opts, dsd_state* state, channel_scan* scan) {
    const int owned = channel_scan_resume_owns(opts, state);
    const int result = channel_scan_commit(opts, state, scan);
    channel_scan_resume_disowns(opts, state, owned);
    return result;
}

/* The staged row's tune ended in @p result without landing. A refusal (FAILED) of a forward step moves the cursor past
   the row, so a bad row does not keep a later one from recovering the receiver and retiring the failed tune's frame
   gate; a Previous's (issue #666) keeps the cursor and leaves the row for the next Previous to retry. Before any row
   landed, either leaves no row heard rather than the row before the cursor. A deferral or a timeout changes neither:
   the same row is tried again. */
static void
channel_scan_row_refused(dsd_state* state, const channel_scan* scan, dsd_trunk_tune_result result) {
    if (result != DSD_TRUNK_TUNE_RESULT_FAILED) {
        return;
    }
    dsd_state_lcn_on_air_note_refused(state);
    if (scan->step_dir < 0) {
        dsd_state_lcn_back_retry_note(state, scan->row);
        return;
    }
    state->lcn_freq_roll = scan->row + 1;
}

/* A resume's row never landed (issue #671): its tune failed late, or a retry was refused. The scan the resume began
   stays paused, its cursor back where the resume found its row, and the toast names the frequency its tune went to
   (the row may hold another since, or be gone). */
static void
channel_scan_resume_row_failed(dsd_opts* opts, dsd_state* state, const channel_scan* scan, int why) {
    state->lcn_freq_roll = scan->resume_roll;
    dsd_engine_channel_scan_resume_failed(opts, state, scan->row_freq, why);
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
        if (!channel_scan_row_is_current(opts, state, scan)) {
            scan->retry = 0;
            channel_scan_note_unlanded_move(opts, state, scan, 0);
            dsd_engine_scan_entry_intent_drop_request(state, scan->attempt);
        } else if (channel_scan_start_row_owned(opts, state, scan->row) < 0 && state->lcn_scan_resume_pending) {
            channel_scan_resume_row_failed(opts, state, scan, scan->start_failure);
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
        (void)channel_scan_commit_owned(opts, state, scan);
        return dsd_engine_channel_scan_waiting(state);
    }
    /* The row will not land. A tune that completed moved the receiver all the same (its row went out of date while it
       was outstanding), and so did a rigctl leg ahead of an RTL leg that went on asynchronously and failed. */
    channel_scan_note_unlanded_move(opts, state, scan, result == DSD_TRUNK_TUNE_RESULT_OK);
    if (state->lcn_scan_resume_pending) {
        scan->request = 0;
        channel_scan_resume_row_failed(opts, state, scan, DSD_SCAN_RESUME_FAILED_TUNE);
        return 0;
    }
    channel_scan_end_calls(opts, state);
    if (channel_scan_row_still_listed(state, scan->list_rev, scan->row, scan->row_freq)) {
        channel_scan_row_refused(state, scan, result);
    }
    /* An entry waiting on the tune holds nothing: it never landed, and its failure is kept for its toast (issue
       #644). */
    dsd_engine_scan_entry_intent_fail_request(state, scan->attempt, channel_scan_entry_failure(result), NULL);
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

int
dsd_engine_scan_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders) {
    if (!state || state->lcn_freq_count <= 0) {
        return -1;
    }
    const int count = state->lcn_freq_count;
    const int start = (from_roll < 0 || from_roll >= count) ? 0 : from_roll;
    /* From the start to the end of the list, then from its top back to the start. */
    for (int pass = 0; pass < 2; pass++) {
        const int first = pass == 0 ? start : 0;
        const int end = pass == 0 ? count : start;
        for (int row = first; row < end; row++) {
            if (!dsd_state_trunk_lcn_avoid_get(state, (size_t)row)
                && (!skip_placeholders || *dsd_state_trunk_lcn_slot_const(state, row) != 0)) {
                return row;
            }
        }
    }
    return -1;
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

static void
channel_scan_row_label(const dsd_state* state, int row, char* label, size_t label_size) {
    const long freq = *dsd_state_trunk_lcn_slot_const(state, row);
    DSD_SNPRINTF(label, label_size, "Scan channel %d (%.6lf MHz)", row + 1, (double)freq / 1000000.0);
}

/* What the rows owe the operator about their squelch, said once per row when a scan (or a newly imported map)
 * starts: a digital row's squelch that cannot gate digital acquisition on this input (issue #521), and an analog
 * row's squelch that lets noise hold it (issue #526). Neither depends on the DSP rate. A placeholder row (frequency 0)
 * is never tuned, so it owes nothing. */
static void
channel_scan_warn_rows_squelch(const dsd_opts* opts, const dsd_state* state) {
    for (int row = 0; row < state->lcn_freq_count; row++) {
        if (*dsd_state_trunk_lcn_slot_const(state, row) == 0) {
            continue;
        }
        dsd_scan_option_values scratch;
        const dsd_scan_option_values* values = channel_scan_row_values(state, row, &scratch);
        /* An analog row's squelch gates exactly what it is for on any input: its monitor and carrier. */
        if (!dsd_scan_mode_is_analog(dsd_channel_mode_get(state, (size_t)row))) {
            channel_scan_warn_row_squelch(opts, row, *dsd_state_trunk_lcn_slot_const(state, row), values);
            continue;
        }
        char label[64];
        channel_scan_row_label(state, row, label, sizeof label);
        const int am = dsd_channel_mode_get(state, (size_t)row) == DSD_SCAN_MODE_AM;
        (void)dsd_engine_scan_warn_analog_squelch(opts, state, values, am, label);
    }
}

/* Whether @p row is an analog row the scanner visits: a placeholder row (frequency 0) is never tuned. */
static int
channel_scan_row_visited_analog(const dsd_state* state, int row) {
    return *dsd_state_trunk_lcn_slot_const(state, row) != 0
           && dsd_scan_mode_is_analog(dsd_channel_mode_get(state, (size_t)row));
}

/* The widths the analog rows run, held to @p dsp_rate_hz: every row's, or, after only configured widths changed
 * (@p configured_changed, 1 << dsd_analog_demod per changed kind), those of the rows that run a changed one; any other
 * row was named at this rate already, so it is counted without being named again
 * (dsd_engine_scan_row_named_again()). Every row skipped at every visit reaches the status line, since a frontend
 * without the log (Android) would otherwise never learn why a row is not visited. A placeholder row (frequency 0) is
 * never tuned and is left out, as dsd_engine_channel_scan_refused_rows() leaves it out. */
static void
channel_scan_warn_rows_width(const dsd_opts* opts, dsd_state* state, int dsp_rate_hz, unsigned configured_changed) {
    dsd_engine_scan_skipped skipped = {0};
    for (int row = 0; row < state->lcn_freq_count; row++) {
        if (!channel_scan_row_visited_analog(state, row)) {
            continue;
        }
        const int kind = dsd_scan_mode_analog_kind(dsd_channel_mode_get(state, (size_t)row));
        dsd_scan_option_values scratch;
        const dsd_scan_option_values* values = channel_scan_row_values(state, row, &scratch);
        char label[64];
        char brief[DSD_ANALOG_ERROR_TEXT_MAX];
        channel_scan_row_label(state, row, label, sizeof label);
        const int row_skipped =
            dsd_engine_scan_row_named_again(values, kind, configured_changed)
                ? dsd_engine_scan_warn_analog_width(opts, state, values, kind, dsp_rate_hz, label, brief, sizeof brief)
                      == DSD_ENGINE_SCAN_WIDTH_SKIPPED
                : dsd_engine_scan_analog_width_skipped(opts, state, values, kind, dsp_rate_hz, brief, sizeof brief);
        if (row_skipped) {
            dsd_engine_scan_skipped_add(&skipped, label, brief);
        }
    }
    dsd_engine_scan_note_skipped_rows(state, &skipped);
}

/* Once per list the rows' squelch, and the analog row widths once the DSP rate they must fit is known: an RTL stream
 * that has published no rate yet leaves the widths to a later row start, and a changed rate (the operator changed the
 * RTL DSP bandwidth) names them again, since a row whose width the rate cannot fit is skipped quietly at every visit
 * (dsd_engine_scan_tune_to_freq()). A changed configured NFM or AM width (a width command, a config apply) names again
 * the rows that run it. */
static void
channel_scan_check_rows(const dsd_opts* opts, dsd_state* state, channel_scan* scan) {
    if (!scan->rows_checked || scan->rows_checked_list != state->lcn_list_rev) {
        scan->rows_checked = 1;
        scan->rows_checked_list = state->lcn_list_rev;
        scan->widths_checked = 0;
        channel_scan_warn_rows_squelch(opts, state);
    }
    const int dsp_rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    const int nfm_hz = dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_FM);
    const int am_hz = dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_AM);
    if (opts->audio_in_type == AUDIO_IN_RTL && dsp_rate_hz <= 0) {
        return;
    }
    const int same_rate = scan->widths_checked && scan->widths_checked_rate_hz == dsp_rate_hz;
    const unsigned configured_changed = (scan->widths_checked_nfm_hz != nfm_hz ? 1U << DSD_ANALOG_DEMOD_FM : 0U)
                                        | (scan->widths_checked_am_hz != am_hz ? 1U << DSD_ANALOG_DEMOD_AM : 0U);
    if (same_rate && configured_changed == 0U) {
        return;
    }
    scan->widths_checked = 1;
    scan->widths_checked_rate_hz = dsp_rate_hz;
    scan->widths_checked_nfm_hz = nfm_hz;
    scan->widths_checked_am_hz = am_hz;
    channel_scan_warn_rows_width(opts, state, dsp_rate_hz, same_rate ? configured_changed : 0U);
}

/* The squelch an analog row runs with: its own, else the configured one. */
static dsd_squelch_setting
channel_scan_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row) {
    if (row && (row->present & DSD_SCAN_OPT_SQUELCH)) {
        return dsd_squelch_mode_is_dynamic(row->squelch_mode)
                   ? dsd_squelch_setting_dynamic(row->squelch_mode, row->squelch_margin_db)
                   : dsd_squelch_setting_of_level(dsd_squelch_level_from_sql((double)row->squelch_db));
    }
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    if (configured) {
        return dsd_squelch_mode_is_dynamic(configured->rtl_squelch_mode)
                   ? dsd_squelch_setting_dynamic(configured->rtl_squelch_mode, configured->rtl_squelch_margin_db)
                   : dsd_squelch_setting_of_level(configured->rtl_squelch_level);
    }
    return dsd_squelch_setting_of_opts(opts);
}

/* Whether that squelch holds the row on noise: off, or at -100 dB and below, every block re-arms its carrier hold, so
 * only the visit cap or a manual advance or avoid moves on. The -100 dB comparison allows for the rounding of the
 * level's power. The auto and noise squelches close on noise on a radio input (issue #518 follow-up), and the noise
 * squelch on audio input's FM rows too (issue #628; a source with nothing above voice says so when it finds out);
 * elsewhere they are off. */
static int
channel_scan_analog_squelch_open(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                 int am) {
    const dsd_squelch_setting squelch = channel_scan_analog_squelch(opts, state, row);
    if (dsd_squelch_setting_is_dynamic(&squelch)) {
        const int input = dsd_squelch_input_kind(opts);
        if (input == DSD_SQUELCH_INPUT_RADIO) {
            return 0;
        }
        return !(input == DSD_SQUELCH_INPUT_AUDIO && squelch.mode == DSD_SQUELCH_MODE_NOISE && !am);
    }
    const double floor_level = dsd_squelch_level_from_sql(-100.0);
    return dsd_squelch_is_off(squelch.level) || squelch.level <= floor_level * (1.0 + 1e-9);
}

void
dsd_engine_scan_ensure_output(dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return;
    }
    if (dsd_opts_is_analog_family(opts)) {
        (void)dsd_audio_ensure_analog_output(opts);
    } else {
        (void)dsd_audio_ensure_digital_output(opts, state);
    }
}

int
dsd_engine_scan_warn_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                    int am, const char* label) {
    if (!opts || !state || !label || !channel_scan_analog_squelch_open(opts, state, row, am)) {
        return 0;
    }
    LOG_WARN("WARNING: %s: the analog channel's squelch is off or at -100 dB or below, so noise holds it on air until "
             "--scan-max-visit-ms or a manual advance or avoid moves on; give it --squelch noise (an nfm row), "
             "--squelch auto (on a radio input) or --squelch-db, or set a squelch.\n",
             label);
    return 1;
}

int
dsd_engine_scan_width_refused(const dsd_opts* opts, int kind, int width_hz, int dsp_rate_hz, char* why,
                              size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    const int held_hz = dsd_engine_scan_held_width_hz(kind, width_hz);
    if (!opts || held_hz <= 0 || opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    char err[DSD_ANALOG_ERROR_TEXT_MAX];
    if (dsd_analog_channel_lpf_off_check(kind, width_hz, dsd_engine_scan_channel_lpf_forced_off(), err, sizeof err)
        != 0) {
        if (why && why_size > 0U) {
            DSD_SNPRINTF(why, why_size, "%s", err);
        }
        return 1;
    }
    /* Worded with the fix this input allows, as the stream start's refusal is (issue #525). */
    return dsp_rate_hz > 0
           && dsd_analog_width_check_at(kind, held_hz, dsp_rate_hz, dsd_input_dsp_rate_policy(opts).source, why,
                                        why_size)
                  != 0;
}

/* The status-line reason a refused width is skipped for (dsd_engine_scan_width_refused()): short, as the log line
 * beside it carries the full text and the fix. @p width_hz 0 is the kind's default. */
static void
scan_width_refusal_brief(int kind, int width_hz, int dsp_rate_hz, char* brief, size_t brief_size) {
    if (!brief || brief_size == 0U) {
        return;
    }
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    char rate[DSD_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_analog_width_format(dsd_analog_width_effective_hz(kind, width_hz), width, sizeof width);
    if (dsd_engine_scan_channel_lpf_forced_off()) {
        DSD_SNPRINTF(brief, brief_size, "%s %s needs the filter DSD_NEO_CHANNEL_LPF=0 turns off",
                     dsd_analog_demod_label(kind), width);
        return;
    }
    (void)dsd_analog_width_format(dsp_rate_hz, rate, sizeof rate);
    DSD_SNPRINTF(brief, brief_size, "%s %s does not fit the %s DSP rate", dsd_analog_demod_label(kind), width, rate);
}

void
dsd_engine_scan_skipped_add(dsd_engine_scan_skipped* skipped, const char* label, const char* brief) {
    if (!skipped) {
        return;
    }
    if (skipped->count++ == 0) {
        DSD_SNPRINTF(skipped->label, sizeof skipped->label, "%s", label ? label : "");
        DSD_SNPRINTF(skipped->brief, sizeof skipped->brief, "%s", brief ? brief : "");
    }
}

void
dsd_engine_scan_note_skipped_rows(dsd_state* state, const dsd_engine_scan_skipped* skipped) {
    if (!state || !skipped || skipped->count <= 0) {
        return;
    }
    if (skipped->count == 1) {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Skipped at every visit: %s: %s", skipped->label,
                     skipped->brief);
    } else {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Skipped at every visit: %s and %d more: %s", skipped->label,
                     skipped->count - 1, skipped->brief);
    }
    state->ui_msg_expire = dsd_realtime_time() + 5;
}

/* A row without a width of its own runs the configured width of its kind @p kind. The width commands hold that width to
 * the DSP rate while the scan has such a row (dsd_engine_scan_runs_configured_nfm_width(), _am_width()), but a list
 * loaded over a width the rate cannot filter, or a rate a device forced, still leaves one the rate cannot filter: that
 * skips the row at every visit as well. So can the AM default, which always runs its channel filter. Audio input
 * filters nothing, so no DSP rate holds the configured width there: with a rigctl peer it is the passband the peer is
 * asked for (issue #621), which the peer takes or refuses at the tune, and no row is named for it here. */
static int
scan_warn_configured_width(const dsd_opts* opts, const dsd_state* state, int kind, int dsp_rate_hz, const char* label,
                           char* brief, size_t brief_size) {
    const int width_hz = dsd_scan_mode_configured_analog_width(opts, state, kind);
    char why[DSD_ANALOG_ERROR_TEXT_MAX];
    if (dsp_rate_hz <= 0 || !dsd_engine_scan_width_refused(opts, kind, width_hz, dsp_rate_hz, why, sizeof why)) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    LOG_WARN("WARNING: %s: it sets no %s width of its own, and the configured %s; until then it is skipped at every "
             "visit.\n",
             label, dsd_analog_demod_label(kind), why);
    scan_width_refusal_brief(kind, width_hz, dsp_rate_hz, brief, brief_size);
    return DSD_ENGINE_SCAN_WIDTH_SKIPPED;
}

/* Whether an analog row of demodulator @p kind is one nothing receives as AM (issue #526): an am row on audio input with
 * no rigctl peer. Audio input arrives demodulated by whatever produced it, and only a rigctl peer can be asked for AM
 * (trunk_tuning.c); such an input tunes no row at all (dsd_engine_scan_tune_to_freq()), so the row is never on air. */
static int
scan_am_row_unreceivable(const dsd_opts* opts, int kind, char* brief, size_t brief_size) {
    if (kind != DSD_ANALOG_DEMOD_AM || opts->audio_in_type == AUDIO_IN_RTL || opts->use_rigctl == 1) {
        return 0;
    }
    if (brief && brief_size > 0U) {
        DSD_SNPRINTF(brief, brief_size, "%s", "AM needs an I/Q input or a rigctl peer");
    }
    return 1;
}

int
dsd_engine_scan_warn_analog_width(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                  int kind, int dsp_rate_hz, const char* label, char* brief, size_t brief_size) {
    if (brief && brief_size > 0U) {
        brief[0] = '\0';
    }
    if (!opts || !label || !dsd_analog_demod_is_valid(kind)) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    if (scan_am_row_unreceivable(opts, kind, brief, brief_size)) {
        LOG_WARN("WARNING: %s: AM rows need an input DSD-neo demodulates (rtl, rtltcp, soapy, airspy) or a rigctl peer "
                 "(-U) that demodulates AM; this audio input has neither, so it is skipped at every visit.\n",
                 label);
        return DSD_ENGINE_SCAN_WIDTH_SKIPPED;
    }
    if (!row || !(row->present & DSD_SCAN_OPT_BANDWIDTH)) {
        return scan_warn_configured_width(opts, state, kind, dsp_rate_hz, label, brief, brief_size);
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        /* A rigctl peer demodulates audio input, and takes the row's width as its passband (trunk_tuning.c). */
        if (opts->use_rigctl == 1) {
            return DSD_ENGINE_SCAN_WIDTH_OK;
        }
        LOG_WARN("WARNING: %s: --%s-bandwidth-hz %d has no effect on audio input, which arrives demodulated and has no "
                 "rigctl peer to take it; set the demodulator's own passband.\n",
                 label, kind == DSD_ANALOG_DEMOD_AM ? "am" : "nfm", row->channel_bw_hz);
        return DSD_ENGINE_SCAN_WIDTH_NO_EFFECT;
    }
    /* The row's rate rule, and DSD_NEO_CHANNEL_LPF=0, which refuses the width at any rate; the text is the front
     * end's. */
    char why[DSD_ANALOG_ERROR_TEXT_MAX];
    if (!dsd_engine_scan_width_refused(opts, kind, row->channel_bw_hz, dsp_rate_hz, why, sizeof why)) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    LOG_WARN("WARNING: %s: %s; until then it is skipped at every visit.\n", label, why);
    scan_width_refusal_brief(kind, row->channel_bw_hz, dsp_rate_hz, brief, brief_size);
    return DSD_ENGINE_SCAN_WIDTH_SKIPPED;
}

int
dsd_engine_scan_analog_width_skipped(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                     int kind, int dsp_rate_hz, char* brief, size_t brief_size) {
    if (brief && brief_size > 0U) {
        brief[0] = '\0';
    }
    if (!opts || !dsd_analog_demod_is_valid(kind)) {
        return 0;
    }
    if (scan_am_row_unreceivable(opts, kind, brief, brief_size)) {
        return 1;
    }
    const int width_hz = (row && (row->present & DSD_SCAN_OPT_BANDWIDTH))
                             ? row->channel_bw_hz
                             : dsd_scan_mode_configured_analog_width(opts, state, kind);
    if (!dsd_engine_scan_width_refused(opts, kind, width_hz, dsp_rate_hz, NULL, 0U)) {
        return 0;
    }
    scan_width_refusal_brief(kind, width_hz, dsp_rate_hz, brief, brief_size);
    return 1;
}

/* The analog rows the front end refuses at @p dsp_rate_hz, with the first one's index and status-line reason
 * (dsd_engine_channel_scan_refused_rows()). */
static int
channel_scan_count_refused_rows(const dsd_opts* opts, const dsd_state* state, int dsp_rate_hz, int* first_row,
                                char* brief, size_t brief_size) {
    int refused = 0;
    for (int row = 0; row < state->lcn_freq_count; row++) {
        if (!channel_scan_row_visited_analog(state, row)) {
            continue;
        }
        dsd_scan_option_values scratch;
        const int kind = dsd_scan_mode_analog_kind(dsd_channel_mode_get(state, (size_t)row));
        char row_brief[DSD_ANALOG_ERROR_TEXT_MAX];
        if (!dsd_engine_scan_analog_width_skipped(opts, state, channel_scan_row_values(state, row, &scratch), kind,
                                                  dsp_rate_hz, row_brief, sizeof row_brief)) {
            continue;
        }
        if (refused++ == 0) {
            *first_row = row;
            DSD_SNPRINTF(brief, brief_size, "%s", row_brief);
        }
    }
    return refused;
}

/* The row the receiver is on: the one whose tune last landed on this list, while the row still holds the frequency
 * landed (typed lists); else, for an untyped list or before the first landing, the row heard
 * (dsd_state_lcn_heard_row(), issue #666), which a placeholder the scan parks on does not move; -1 before the first
 * step. */
static int
channel_scan_on_air_row(const dsd_state* state) {
    const channel_scan* scan = channel_scan_get(state);
    if (scan && scan->tuned
        && channel_scan_row_still_listed(state, scan->tuned_list, scan->tuned_row, scan->tuned_freq)) {
        return scan->tuned_row;
    }
    return dsd_state_lcn_heard_row(state);
}

int
dsd_engine_channel_scan_on_air_row(const dsd_state* state) {
    return state ? channel_scan_on_air_row(state) : -1;
}

int
dsd_engine_channel_scan_on_avoided_row(const dsd_state* state) {
    /* The avoid count first: with nothing avoided, the frame loop's question costs one read. */
    if (!state || state->lcn_avoid_count == 0U) {
        return 0;
    }
    const int row = channel_scan_on_air_row(state);
    return row >= 0 && row < state->lcn_freq_count && dsd_state_trunk_lcn_avoid_get(state, (size_t)row);
}

int
dsd_engine_channel_scan_hold_keeps_row(const dsd_state* state) {
    return state && state->lcn_scan_hold && !dsd_engine_channel_scan_on_avoided_row(state);
}

int
dsd_engine_channel_scan_row_width_fits(const dsd_opts* opts, const dsd_state* state, int row, int dsp_rate_hz) {
    if (!channel_scan_row_visited_analog(state, row)) {
        return 1;
    }
    dsd_scan_option_values scratch;
    const int kind = dsd_scan_mode_analog_kind(dsd_channel_mode_get(state, (size_t)row));
    return !dsd_engine_scan_analog_width_skipped(opts, state, channel_scan_row_values(state, row, &scratch), kind,
                                                 dsp_rate_hz, NULL, 0U);
}

/* Whether a step can land on @p row: it has a frequency, the operator has not avoided it, and it is not an analog row
 * skipped at every visit before any backend moves: one whose width, of the kind its demodulator runs, the front end
 * refuses at @p dsp_rate_hz, or an am row nothing demodulates (dsd_engine_channel_scan_row_width_fits()). */
static int
channel_scan_row_reachable(const dsd_opts* opts, const dsd_state* state, int row, int dsp_rate_hz) {
    return *dsd_state_trunk_lcn_slot_const(state, row) != 0 && !dsd_state_trunk_lcn_avoid_get(state, (size_t)row)
           && dsd_engine_channel_scan_row_width_fits(opts, state, row, dsp_rate_hz);
}

/* A tone policy as it judges (dsd_analog_tone_policy_configure()): a list policy with a list, else none (OFF and an
 * empty list). */
static void
scan_tone_policy_as_judged(int* mode, dsd_tone_set* set) {
    if ((*mode != DSD_TONE_FILTER_ALLOW && *mode != DSD_TONE_FILTER_BLOCK) || dsd_tone_set_count(set) == 0) {
        *mode = DSD_TONE_FILTER_OFF;
        DSD_MEMSET(set, 0, sizeof(*set));
    }
}

/* Whether list policy @p mode with @p set rejects the value the detectors hold locked in @p rx. */
static int
scan_tone_policy_rejects_locked(int mode, const dsd_tone_set* set, const dsd_analog_rx_publication* rx) {
    int listed = 0;
    if (rx->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        listed = dsd_tone_set_contains_ctcss(set, rx->ctcss_tenths_hz);
    } else if (rx->tone_kind == DSD_ANALOG_TONE_KIND_DCS) {
        listed = dsd_tone_set_contains_dcs(set, rx->dcs_code, rx->dcs_inverted);
    }
    return (mode == DSD_TONE_FILTER_ALLOW) ? !listed : listed;
}

/* Whether a row that runs received-tone detection and is tuned to the frequency on air would reject the traffic the
 * CTCSS/DCS receive policy in force rejected (issue #527), so that a step to it would only end the reception and judge
 * the same traffic again. Its policy is @p row's own (DSD_SCAN_OPT_TONE), else the configured one
 * (dsd_scan_mode_configured_tone_policy()), each as it judges. It rejects the traffic when it is the policy in force
 * (dsd_opts), which already did, its codes perhaps respelled (dsd_tone_set_same_signals()); otherwise when it is a list
 * policy that rejects what the verdict rests on: the value the detectors hold locked (an allow list that does not list
 * it, a block list that does), or, after a "no tone" verdict, no tone (an allow list). With neither in hand (the
 * rejected value has since been lost) only the policy in force is known to reject it. A row with no policy passes
 * everything. @p row may be NULL (no options). */
static int
channel_scan_tone_rejects_alike(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row) {
    int mode = DSD_TONE_FILTER_OFF;
    dsd_tone_set set;
    DSD_MEMSET(&set, 0, sizeof(set));
    if (row && (row->present & DSD_SCAN_OPT_TONE)) {
        mode = row->tone_filter;
        set = row->tone_set;
    } else {
        dsd_scan_mode_configured_tone_policy(opts, state, &mode, &set);
    }
    scan_tone_policy_as_judged(&mode, &set);
    int in_force_mode = opts->analog_tone_filter;
    dsd_tone_set in_force_set = opts->analog_tone_set;
    scan_tone_policy_as_judged(&in_force_mode, &in_force_set);
    if (mode == in_force_mode && dsd_tone_set_same_signals(&set, &in_force_set)) {
        return 1;
    }
    if (mode == DSD_TONE_FILTER_OFF) {
        return 0;
    }
    const dsd_analog_rx_publication* rx = &state->analog_rx;
    if (rx->tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
        return scan_tone_policy_rejects_locked(mode, &set, rx);
    }
    return rx->gate_no_tone && mode == DSD_TONE_FILTER_ALLOW;
}

/* Whether a step to @p row, tuned to the frequency on air, would only judge the traffic the tone policy rejected again
 * and reject it (issue #527): the row runs received-tone detection (dsd_channel_mode_hears_tones()), and its tone
 * policy rejects that traffic too (channel_scan_tone_rejects_alike()). A row of another class -- a digital or am
 * row, or one inheriting a decode mode other than the FM monitor -- or one whose policy passes the traffic is
 * somewhere to go. */
static int
channel_scan_row_rejects_alike(const dsd_opts* opts, const dsd_state* state, int row) {
    if (!dsd_channel_mode_hears_tones(state, (size_t)row, dsd_scan_mode_configured_fm_monitor(opts, state))) {
        return 0;
    }
    dsd_scan_option_values scratch;
    return channel_scan_tone_rejects_alike(opts, state, channel_scan_row_values(state, row, &scratch));
}

int
dsd_engine_channel_scan_has_other_row(const dsd_opts* opts, const dsd_state* state) {
    /* Channels learned over the air are no list to scan: only the user's own rows are somewhere to go. */
    if (!opts || !state || !dsd_state_trunk_lcn_user_list_present(opts, state)) {
        return 0;
    }
    const int current = channel_scan_on_air_row(state);
    const long current_freq = current >= 0 ? *dsd_state_trunk_lcn_slot_const(state, current) : 0L;
    const int dsp_rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    for (int row = 0; row < state->lcn_freq_count; row++) {
        if (row == current || !channel_scan_row_reachable(opts, state, row, dsp_rate_hz)) {
            continue;
        }
        if (*dsd_state_trunk_lcn_slot_const(state, row) != current_freq
            || !channel_scan_row_rejects_alike(opts, state, row)) {
            return 1;
        }
    }
    return 0;
}

/* The rows a step can land on (channel_scan_row_reachable()), counted up to @p stop_at. */
static int
channel_scan_count_reachable(const dsd_opts* opts, const dsd_state* state, int stop_at) {
    const int dsp_rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    int reachable = 0;
    for (int row = 0; row < state->lcn_freq_count && reachable < stop_at; row++) {
        if (channel_scan_row_reachable(opts, state, row, dsp_rate_hz)) {
            reachable++;
        }
    }
    return reachable;
}

int
dsd_engine_channel_scan_can_rotate(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || !dsd_state_trunk_lcn_user_list_present(opts, state)) {
        return 0;
    }
    return channel_scan_count_reachable(opts, state, 2) >= 2;
}

int
dsd_engine_channel_scan_reachable_rows(const dsd_opts* opts, const dsd_state* state) {
    return (opts && state) ? channel_scan_count_reachable(opts, state, INT_MAX) : 0;
}

int
dsd_engine_channel_scan_refused_rows(const dsd_opts* opts, const dsd_state* state, int dsp_rate_hz, int* first_row,
                                     char* brief, size_t brief_size) {
    int refused = 0;
    int first = -1;
    char first_brief[DSD_ANALOG_ERROR_TEXT_MAX] = "";
    if (opts && state && opts->audio_in_type == AUDIO_IN_RTL) {
        refused = channel_scan_count_refused_rows(opts, state, dsp_rate_hz, &first, first_brief, sizeof first_brief);
    }
    if (first_row) {
        *first_row = first;
    }
    if (brief && brief_size > 0U) {
        DSD_SNPRINTF(brief, brief_size, "%s", first_brief);
    }
    return refused;
}

/* What a row start stages in the scan: the row, what it was prepared from and with (its options, its keys, the
   configured settings and the keyring epoch), and the tune that carries it, with the step it belongs to. A go-to of a
   row (issue #644) prepares its row in one of these first and swaps it in for the tune, so a preparation that
   fails, or a tune refused before any backend moved, puts back the row staged before, its tune included. */
typedef struct {
    int row;
    dsd_scan_mode mode;
    uint64_t list_rev;
    long row_freq;
    int row_kind;
    dsd_scan_settings configured;
    dsd_scan_key_change keys;
    uint64_t key_epoch;
    uint32_t edits_epoch;
    dsd_scan_option_values values;
    int has_values;
    uint32_t family_requests;
    uint32_t familyless_supersedes;
    int family_attached;
    uint64_t request;
    int retry;
    int step_dir;
    uint64_t attempt;
    uint32_t attempt_prior_hz;
} channel_scan_staging;

/* Exchange what @p staging holds with what @p scan has staged, the keys moved, not copied. */
static void
channel_scan_staging_swap(channel_scan* scan, channel_scan_staging* staging) {
    channel_scan_staging held;
    held.row = scan->row;
    held.mode = scan->mode;
    held.list_rev = scan->list_rev;
    held.row_freq = scan->row_freq;
    held.row_kind = scan->row_kind;
    held.configured = scan->configured;
    DSD_MEMCPY(&held.keys, &scan->keys, sizeof(held.keys));
    held.key_epoch = scan->key_epoch;
    held.edits_epoch = scan->staged_edits_epoch;
    held.values = scan->staged_values;
    held.has_values = scan->staged_has_values;
    held.family_requests = scan->family_requests;
    held.familyless_supersedes = scan->familyless_supersedes;
    held.family_attached = scan->family_attached;
    held.request = scan->request;
    held.retry = scan->retry;
    held.step_dir = scan->step_dir;
    held.attempt = scan->attempt;
    held.attempt_prior_hz = scan->attempt_prior_hz;
    scan->row = staging->row;
    scan->mode = staging->mode;
    scan->list_rev = staging->list_rev;
    scan->row_freq = staging->row_freq;
    scan->row_kind = staging->row_kind;
    scan->configured = staging->configured;
    DSD_MEMCPY(&scan->keys, &staging->keys, sizeof(scan->keys));
    scan->key_epoch = staging->key_epoch;
    scan->staged_edits_epoch = staging->edits_epoch;
    scan->staged_values = staging->values;
    scan->staged_has_values = staging->has_values;
    scan->family_requests = staging->family_requests;
    scan->familyless_supersedes = staging->familyless_supersedes;
    scan->family_attached = staging->family_attached;
    scan->request = staging->request;
    scan->retry = staging->retry;
    scan->step_dir = staging->step_dir;
    scan->attempt = staging->attempt;
    scan->attempt_prior_hz = staging->attempt_prior_hz;
    DSD_MEMCPY(staging, &held, sizeof(*staging));
    DSD_SECURE_ZERO(&held, sizeof(held));
}

/* Free what @p staging still holds (its keys). */
static void
channel_scan_staging_clear(channel_scan_staging* staging) {
    dsd_scan_key_change_clear(&staging->keys);
}

/* Prepare @p row (on @p freq) into @p out, and the settings its tune runs into @p next, for a step in direction
   @p step_dir: the scan itself is left alone. -1 when the row's settings, scopes or keys cannot be prepared (@p out
   then holds nothing to free beyond channel_scan_staging_clear()). */
static int
channel_scan_prepare_row(dsd_opts* opts, dsd_state* state, channel_scan* scan, int row, long freq, int step_dir,
                         channel_scan_staging* out, dsd_scan_settings* next) {
    DSD_MEMSET(out, 0, sizeof(*out));
    channel_scan_check_rows(opts, state, scan);
    out->row = row;
    out->mode = dsd_channel_mode_get(state, (size_t)row);
    out->list_rev = state->lcn_list_rev;
    out->row_freq = freq;
    out->step_dir = step_dir;
    /* The row's own acquisition options (its analog channel width, a session edit's included) are part of what the
       tune queues. */
    dsd_scan_option_values values_scratch;
    const dsd_scan_option_values* staged = channel_scan_row_values(state, row, &values_scratch);
    out->edits_epoch = scan->edits_epoch;
    out->has_values = staged != NULL;
    if (staged) {
        out->values = *staged;
    }
    if (dsd_scan_mode_prepare(opts, state, out->mode, staged, next) != 0) {
        return -1;
    }
    out->row_kind = next->analog_demod;
    dsd_scan_mode_configured(opts, state, &out->configured);
    if (dsd_scan_groups_begin(state)
        || dsd_scan_key_change_prepare(state, dsd_state_trunk_lcn_keys_get(state, (size_t)row), &out->keys)) {
        return -1;
    }
    out->key_epoch = state->enc_lockout_key_epoch;
    return 0;
}

/* Tune the row staged in @p scan with the settings it was prepared with (@p next), noting the family requests its
   commit checks it against. */
static dsd_trunk_tune_result
channel_scan_tune_staged(dsd_opts* opts, dsd_state* state, channel_scan* scan, const dsd_scan_settings* next) {
    const uint32_t prior_hz = opts->rtlsdr_center_freq;
    dsd_scan_settings before;
    dsd_scan_settings_capture(opts, state, &before);
    dsd_scan_settings_restore(next, opts, state);
    scan->family_requests = dsd_engine_scan_family_requests(opts);
    scan->familyless_supersedes = dsd_engine_scan_familyless_retune_supersedes(opts);
    scan->family_attached = dsd_engine_scan_retune_attaches_family(opts, state, state->samplesPerSymbol);
    const dsd_trunk_tune_result result =
        dsd_engine_scan_tune_to_freq(opts, state, scan->row_freq, state->samplesPerSymbol, &scan->request);
    dsd_scan_settings_restore(&before, opts, state);
    /* A new attempt is named by its first request; a restage keeps the name. An entry waiting on the row is this
       attempt's (issue #644). */
    if (scan->attempt == 0U) {
        scan->attempt = scan->request;
        scan->attempt_prior_hz = prior_hz;
    }
    dsd_engine_scan_entry_intent_bind(state, scan->row, scan->attempt);
    return result;
}

/* The staged row's tune answered @p result: commit it (OK), wait for it (PENDING), or end it unlanded. Returns as
   channel_scan_start_row() does. */
static int
channel_scan_tune_outcome(dsd_opts* opts, dsd_state* state, channel_scan* scan, dsd_trunk_tune_result result) {
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        return channel_scan_commit(opts, state, scan);
    }
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        /* A rigctl leg that moved ahead of the RTL leg left the carrier whatever the RTL leg's completion says. */
        if (dsd_engine_scan_tune_moved()) {
            scan->left_carrier = 1;
        }
        return 0;
    }
    /* A failed second backend may already have moved the first one. Keep the tune
     * ledger's frame gate closed until a subsequent successful request recovers.
     * When the rigctl leg did move the radio, the row ends unlanded on another carrier. */
    channel_scan_note_unlanded_move(opts, state, scan, dsd_engine_scan_tune_moved());
    scan->start_failure =
        result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? DSD_SCAN_RESUME_FAILED_DEFERRED : DSD_SCAN_RESUME_FAILED_TUNE;
    channel_scan_end_calls(opts, state);
    channel_scan_row_refused(state, scan, result);
    dsd_engine_scan_entry_intent_fail_request(state, scan->attempt, channel_scan_entry_failure(result), NULL);
    scan->request = 0;
    return -1;
}

static int
channel_scan_start_row(dsd_opts* opts, dsd_state* state, int row) {
    if (row < 0 || row >= state->lcn_freq_count) {
        return 0;
    }
    /* Whatever this start comes to, the receiver is no longer known to be on the row last landed: a placeholder row
       tunes nothing, and a tune that fails or is still pending lands nothing (issue #667 D9). */
    dsd_engine_scan_landing_forget(state);
    const long freq = *dsd_state_trunk_lcn_slot(state, row);
    if (!freq) {
        state->lcn_freq_roll = row + 1;
        state->last_cc_sync_time = dsd_decode_time();
        state->last_cc_sync_time_m = dsd_decode_now_mono_s();
        dsd_scan_voice_gate_note_retune(state, state->last_cc_sync_time_m);
        return 0;
    }
    channel_scan* scan = channel_scan_obtain(state);
    if (!scan) {
        return -1;
    }
    scan->start_failure = DSD_SCAN_RESUME_FAILED_SETUP;
    channel_scan_staging staging;
    dsd_scan_settings next;
    /* A failed preparation leaves the row staged before as it was, a completed tune's retry gate included: that tune
       may already have moved the receiver. */
    if (channel_scan_prepare_row(opts, state, scan, row, freq, scan->step_dir, &staging, &next) != 0) {
        channel_scan_staging_clear(&staging);
        return -1;
    }
    /* The new request takes over from whatever was staged before. A retry is the restage of the row staged, the same
       attempt. */
    staging.attempt = scan->retry && scan->row == row ? scan->attempt : 0U;
    channel_scan_staging_swap(scan, &staging);
    channel_scan_staging_clear(&staging);
    return channel_scan_tune_outcome(opts, state, scan, channel_scan_tune_staged(opts, state, scan, &next));
}

/* A step in direction @p dir begins (issue #666): its row's failure is handled as that step's
   (channel_scan_row_refused()). A forward step also starts from the cursor, whatever a refused Previous left to retry.
   A scan's first row creates the extension, which reads as forward; a Previous creates it here. Returns -1 when it
   cannot be allocated. */
static int
channel_scan_begin_step(dsd_state* state, int dir) {
    if (dir > 0) {
        dsd_state_lcn_back_retry_clear(state);
    }
    channel_scan* scan = dir < 0 ? channel_scan_obtain(state) : channel_scan_get(state);
    if (scan) {
        scan->step_dir = dir;
    }
    return (dir < 0 && !scan) ? -1 : 0;
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
    /* Another row's tune begins: an entry waiting on a landing is no longer the operator's last word (issue #644). */
    dsd_engine_scan_entry_intent_cancel(state);
    (void)channel_scan_begin_step(state, 1);
    return channel_scan_start_row(opts, state, channel_scan_next_row(state));
}

/* Where a manual step in direction @p dir starts looking (issue #666). Forward: the cursor. Back: the row before the
   one a refused Previous left to retry, else before the row heard (the position: the last row a step landed, not a
   row it failed or still waits on), wrapping; with neither, the last row. */
static int
channel_scan_manual_start(const dsd_state* state, int dir) {
    const int count = state->lcn_freq_count;
    if (dir > 0) {
        return (state->lcn_freq_roll < 0 || state->lcn_freq_roll >= count) ? 0 : state->lcn_freq_roll;
    }
    int anchor = dsd_state_lcn_back_retry_row(state);
    if (anchor < 0) {
        anchor = dsd_state_lcn_on_air_row(state);
    }
    return anchor < 0 ? count - 1 : (anchor - 1 + count) % count;
}

/* The row a manual step lands on: from @p start, which is examined first, one row at a time in direction @p dir,
   wrapping, the first with a frequency that is not avoided. Rows on one frequency are all kept: each may run another
   mode, options or keys. One pass at most; -1 for none. */
static int
channel_scan_pick_row(const dsd_state* state, int start, int dir) {
    const int count = state->lcn_freq_count;
    int row = start;
    for (int examined = 0; examined < count; examined++) {
        if (*dsd_state_trunk_lcn_slot_const(state, row) != 0 && !dsd_state_trunk_lcn_avoid_get(state, (size_t)row)) {
            return row;
        }
        row = (row + dir + count) % count;
    }
    return -1;
}

int
dsd_engine_channel_scan_step_manual_dir(dsd_opts* opts, dsd_state* state, int dir) {
    if (!opts || !state || opts->trunk_scan_enabled == 1 || state->lcn_freq_count <= 0) {
        return 0;
    }
    /* Next and Previous (an avoid's step among them) cancel an entry waiting on its row's landing, whether they step or
       service the tune outstanding (issue #644). */
    dsd_engine_scan_entry_intent_cancel(state);
    const channel_scan* scan = channel_scan_get(state);
    if (scan && (scan->request || scan->retry)) {
        return dsd_engine_channel_scan_step(opts, state);
    }
    dir = dir < 0 ? -1 : 1;
    const int row = channel_scan_pick_row(state, channel_scan_manual_start(state, dir), dir);
    if (row < 0) {
        /* Nowhere to land: nothing is tuned, though a forward press still drops what a refused Previous left to retry. */
        if (dir > 0) {
            dsd_state_lcn_back_retry_clear(state);
        }
        return 0;
    }
    if (channel_scan_begin_step(state, dir) < 0) {
        return -1;
    }
    const int result = channel_scan_start_row(opts, state, row);
    if (result >= 0) {
        LOG_INFO("Channel Cycle: tuning to %.06lf MHz\n",
                 (double)*dsd_state_trunk_lcn_slot_const(state, row) / 1000000);
    }
    return result;
}

int
dsd_engine_channel_scan_step_manual(dsd_opts* opts, dsd_state* state) {
    return dsd_engine_channel_scan_step_manual_dir(opts, state, 1);
}

/* Put the RTL front end back on the configured receive family. Under -fA that is the configured analog profile
 * (demodulator and channel width, 0 meaning the default); otherwise the digital family and the symbol profile the
 * restored decoder runs on, in that order, so the demod thread switches family before it applies the profile. Returns
 * what became of the analog profile request: 1 queued, -1 refused at once (dsd_engine_channel_scan_leave()); 0 when
 * none was made (not RTL input, or the digital family).
 *
 * A front end still on the analog family (the -fA session whose configured mode was changed to a digital one while a
 * row ran) switches to digital only here, after the configured timing was saved for the analog family's output rate:
 * the monitor's resampled audio, or the rate a typed row's profile ran at. The decoder, and the profile it publishes,
 * are timed for the rate the digital family lands on instead, as svc_publish_symbol_profile() times a mode change
 * outside a row. So they are when work still outstanding lands a family on a front end already digital (issue #583: a
 * row's retune that carries the digital family, still in flight, or an analog row's), or when the row on air was timed
 * for that landing: the leave decides once, as a republish does, with the configured mode digital and either the row's
 * recorded decision (@p row_timed_digital_family, read from the scope before the leave removed it:
 * dsd_scan_mode_timed_digital_family()) or the stream's answer
 * (dsd_rtl_stream_metrics_hook_family_landing_after_pending(), which includes the live analog family). The row's
 * decision stands when the stream's answer has fallen since (a scoped command's resume timed the row for the landing
 * while its retune was outstanding, and that work failed before the scan stopped), so the configured decoder is not
 * timed and published by a different answer than the row was. The digital family asked for then is the landing
 * (dsd_rtl_stream_metrics_hook_request_digital_family_landing()), which puts the front end on that prediction whichever
 * family it runs where it lands: the leave supersedes a retune still outstanding, which lands its centre only, and one
 * that landed before it leaves the landing to put the configured decoder's profile where it is timed. The configured
 * decoder is no target, so its CQPSK state is an open's, as DSD_NEO_CQPSK decides when set. With no decision for the
 * landing on a digital front end the saved timing stands, and the plain request is a no-op there.
 *
 * A leave that switches the front end's family, or its analog kind (an nfm row's FM monitor on an -fM session, issue
 * #524), also drops the analog monitor block the decoder has part-collected from the old family's or kind's output, as
 * a decode-mode change between them does. A leave from a typed digital row switches neither (the row kept the front
 * end on the analog family, on the row's channel): getSymbol() drops that block itself, and collects nothing, until
 * the monitor this requests is published (issue #582). Before any of its requests the leave supersedes the retunes
 * queued with no family (a typed row's: dsd_engine_scan_supersede_familyless_retunes()), so one the controller lands
 * late moves its centre only and does not undo the configured decoder's profile. */
static int
channel_scan_restore_frontend(const dsd_opts* opts, dsd_state* state, int row_timed_digital_family) {
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    /* A typed row's retune still outstanding carries no family, so the requests below would not supersede it: the
       controller could land it after the demod thread took them, and put the row's symbol profile back over the
       configured decoder's for good. Superseded first, it lands its centre only (issue #582). */
    dsd_engine_scan_supersede_familyless_retunes(opts);
    const int analog_family_active = dsd_rtl_stream_metrics_hook_analog_family_active();
    if (dsd_opts_is_analog_family(opts)) {
        int running_kind = opts->analog_demod;
        const int monitor_published = dsd_rtl_stream_metrics_hook_analog_profile(&running_kind, NULL, NULL) == 1;
        if (!analog_family_active || (monitor_published && running_kind != opts->analog_demod)) {
            dsd_symbol_analog_block_reset(state);
        }
        const int rc = dsd_rtl_stream_metrics_hook_apply_analog_profile(DSD_RX_FAMILY_ANALOG, opts->analog_demod,
                                                                        dsd_opts_analog_width_hz(opts));
        return rc == 0 ? 1 : -1;
    }
    const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
    if (analog_family_active) {
        dsd_symbol_analog_block_reset(state);
    }
    const int lands_digital_family =
        dsd_scan_mode_configured_digital(opts, state)
        && (row_timed_digital_family || dsd_rtl_stream_metrics_hook_family_landing_after_pending());
    if (lands_digital_family) {
        const unsigned int rate_hz = dsd_rtl_stream_metrics_hook_output_rate_for_family(
            DSD_RX_FAMILY_DIGITAL, state->rf_mod == 1, profile.symbol_rate_hz, 0);
        if (rate_hz > 0U) {
            state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, (int)rate_hz);
            state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
        }
    }
    const int filter = opts->analog_only || !dsd_opts_has_digital_decode_mode(opts)
                           ? DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE
                           : dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, state->rf_mod);
    if (lands_digital_family) {
        (void)dsd_rtl_stream_metrics_hook_request_digital_family_landing(0);
    } else {
        (void)dsd_rtl_stream_metrics_hook_apply_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0);
    }
    (void)dsd_rtl_stream_metrics_hook_apply_demod_profile(state->rf_mod == 1, profile.symbol_rate_hz, profile.levels,
                                                          filter, state->samplesPerSymbol);
    return 0;
}

/* Drop what a row in flight or on air holds, keeping the scan's list state and session edits for a resume (issue
   #671): a request staged or pending, a retry and a commit owed (else dsd_engine_channel_scan_service_sync() would drop
   every frame of the manual session), the row on air, and the staged row's options (else
   dsd_engine_scan_tuning_row_options() would hand them to a rigctl request) and keys. */
static void
channel_scan_drop_row(channel_scan* scan) {
    scan->request = 0;
    scan->retry = 0;
    scan->needs_commit = 0;
    scan->tuned = 0;
    scan->committed_row = -1;
    /* The pause runs its own carrier boundary (the entry's tune) or forget (in place): no move stays owed. */
    scan->left_carrier = 0;
    scan->staged_has_values = 0;
    DSD_MEMSET(&scan->staged_values, 0, sizeof(scan->staged_values));
    dsd_scan_key_change_clear(&scan->keys);
    scan->family_requests = 0U;
    scan->familyless_supersedes = 0U;
    scan->family_attached = 0;
    /* The direction of the step the row belonged to goes too: a resume's row is a forward step's (issue #666). */
    scan->step_dir = 0;
    /* And the attempt it was: a resume's row is a new one (issue #644). */
    scan->attempt = 0U;
    scan->attempt_prior_hz = 0U;
}

void
dsd_engine_scan_forget_carrier(dsd_opts* opts, dsd_state* state) {
    dsd_engine_forget_carrier_decoding(opts, state);
    dsd_rxid_reset(state);
    dmr_ras_reset(state);
    dsd_state_note_carrier_change(state);
}

/* Whether an untyped -Y list runs (issue #644 B-3): scanner_mode over a list of the user's own, and nothing typed. Such
   a list has no scan object and no scope for a leave to find, yet its calls are named after its rows and keyed with
   their keys. Not a resume still waiting on its row: the calls heard meanwhile are the manual session's, on the manual
   frequency, which the stop leaves the receiver on. Channels learned over the air are no list: the scan never moved for
   them. */
static int
channel_scan_untyped_runs(const dsd_opts* opts, const dsd_state* state) {
    return opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1 && !dsd_channel_modes_present(state)
           && dsd_state_trunk_lcn_user_list_present(opts, state);
}

/* At a release no entry waits on a landing any more, and an entry hold goes with the scan it held, while an ordinary
   hold outlives a stop as it always has (issue #644). */
static void
channel_scan_release_entry(dsd_state* state) {
    dsd_engine_scan_entry_intent_cancel(state);
    if (state->lcn_scan_entry_hold) {
        state->lcn_scan_hold = 0U;
        state->lcn_scan_entry_hold = 0U;
    }
}

/* The -Y scan's leave (@p keep 0) and pause (@p keep 1, issue #671). Every end of a scan moves the session, so a row
   tune still pending from it never commits its row afterwards. A pause ends the calls whatever the list (an untyped one
   has no scope); a leave ends them where a scan is active, and where an untyped list runs (issue #644 B-3), while
   the caller still has its row named and keyed: they end under that row, before the configured keys come back. The
   untyped list's leave stops there: its rows ran the configured decoder and front end, so nothing else goes back. A
   paused list (no resume pending) was already left by its pause, which kept only its list state: its leave drops that
   and leaves the manual session alone, so a stop over a pause ends no call, resets no acquisition and asks the monitor
   for nothing. */
static int
channel_scan_release(dsd_opts* opts, dsd_state* state, int keep, int in_place) {
    state->lcn_scan_session++;
    /* The scan no longer says where the receiver is (issue #667 D9). */
    dsd_engine_scan_landing_forget(state);
    channel_scan_release_entry(state);
    channel_scan* scan = channel_scan_get(state);
    const int already_left = !keep && state->lcn_scan_paused && !state->lcn_scan_resume_pending;
    const int active =
        !already_left
        && (scan != NULL || dsd_scan_mode_configured_view(state) != NULL || dsd_scan_mode_updating(state));
    if (active || keep || (!already_left && channel_scan_untyped_runs(opts, state))) {
        channel_scan_end_calls(opts, state);
    }
    if (keep && in_place) {
        dsd_engine_scan_forget_carrier(opts, state);
    }
    if (keep && scan) {
        channel_scan_drop_row(scan);
    } else {
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, NULL, NULL);
    }
    channel_scan_clear_published_row(state);
    dsd_scan_groups_leave(state);
    dsd_scan_maps_leave(state);
    /* The landing decision the row on air was timed with goes with its scope; the restore decides by it too. */
    const int row_timed_digital_family = dsd_scan_mode_timed_digital_family(state);
    dsd_scan_mode_leave(opts, state);
    int monitor_request = 0;
    if (active) {
        /* A trunk-scan target's P25 modulation stays with its scope while the coordinator owns the tuner; a pause
           leaves for the manual session, which no target names (issue #671). */
        dsd_frame_sync_reset_acquisition(opts, state, !dsd_trunk_scan_owns_tuner(opts, state));
        monitor_request = channel_scan_restore_frontend(opts, state, row_timed_digital_family);
        /* A rigctl peer that demodulated an am row, or ran a row's own passband, goes back to what the configured
         * session asks for: no later tune outside a scan would undo it (issue #526). It queues no receive request, so
         * the analog profile request stays the leave's last. */
        dsd_engine_scan_rigctl_restore(opts, state);
    }
    return monitor_request;
}

int
dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    return channel_scan_release(opts, state, 0, 0);
}

int
dsd_engine_channel_scan_pause(dsd_opts* opts, dsd_state* state, int in_place) {
    if (!dsd_channel_scan_owns_tuner(opts, state)) {
        return 0;
    }
    /* The calls end under the row on air, still named and keyed (F3); the configured keys come back after them. */
    const int monitor_request = channel_scan_release(opts, state, 1, in_place);
    dsd_scan_keys_leave(state);
    opts->scanner_mode = 0;
    state->lcn_scan_hold = 0U;
    state->lcn_scan_entry_hold = 0U;
    state->lcn_scan_paused = 1U;
    state->lcn_scan_resume_pending = 0U;
    state->operator_tuned = 1U;
    return monitor_request;
}

int
dsd_engine_channel_scan_end_running_calls(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->scanner_mode != 1 || opts->trunk_scan_enabled == 1
        || !dsd_state_trunk_lcn_user_list_present(opts, state)) {
        return 0;
    }
    channel_scan_end_calls(opts, state);
    return 1;
}

/* Why a start or a resume did nothing, or a resume left the scan paused, on the status line every frontend shows, for
   as long as a tune's failure stays there. */
static void
channel_scan_toast_resume(dsd_state* state, const char* text) {
    DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "%s", text);
    state->ui_msg_expire = dsd_realtime_time() + DSD_MANUAL_TUNE_FAILED_TOAST_S;
}

/* The receiver's centre as the decoder knows it: the RTL front end's cached centre, else the frequency last sent to a
   rigctl peer; 0 with neither (audio input, which no scan moves). */
static uint32_t
channel_scan_receiver_hz(const dsd_opts* opts) {
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return opts->rtlsdr_center_freq;
    }
    return opts->use_rigctl == 1 ? opts->rigctl_sent_freq_hz : 0U;
}

/* A start's ordinary hold kept across the stop (issue #644): it holds the row heard, so it stays only while the
   receiver is there. A walk the start took as its own is the receiver's next row and keeps it (@p adopted); one it
   could not take (@p walk_left) moves the receiver where the hold would not follow, and a centre on another frequency
   (a manual tune since the stop) is not the row's: either lets the hold go, so the running scan's next timed step tunes
   a row of its own. An entry hold never outlives a stop; an unknown centre keeps the hold. */
static void
channel_scan_start_settles_hold(const dsd_opts* opts, dsd_state* state, int adopted, int walk_left) {
    if (!state->lcn_scan_hold || state->lcn_scan_entry_hold || adopted) {
        return;
    }
    if (!walk_left) {
        const uint32_t hz = channel_scan_receiver_hz(opts);
        const int row = channel_scan_on_air_row(state);
        if (hz == 0U
            || (row >= 0 && row < state->lcn_freq_count && *dsd_state_trunk_lcn_slot_const(state, row) == (long)hz)) {
            return;
        }
    }
    state->lcn_scan_hold = 0U;
}

void
dsd_engine_channel_scan_stream_stopping(dsd_opts* opts, const dsd_state* state) {
    const channel_scan* scan = state ? channel_scan_get(state) : NULL;
    if (!opts || !scan || (!scan->request && !scan->retry) || scan->attempt_prior_hz == 0U || scan->row_freq <= 0
        || opts->rtlsdr_center_freq != (uint32_t)scan->row_freq) {
        return;
    }
    opts->rtlsdr_center_freq = scan->attempt_prior_hz;
}

void
dsd_engine_channel_scan_config_start(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || !dsd_state_trunk_lcn_user_list_present(opts, state)) {
        return;
    }
    const int walk = dsd_engine_manual_tune_operator_walk_noted();
    const int adopted = walk && dsd_engine_manual_tune_adopt_walk(state);
    channel_scan_start_settles_hold(opts, state, adopted, walk && !adopted);
}

int
dsd_engine_channel_scan_start(dsd_opts* opts, dsd_state* state) {
    /* A paused scan resumes instead (dsd_engine_channel_scan_resume()): a start would publish it running beside its
       pause. */
    if (!opts || !state || state->lcn_scan_paused || state->lcn_scan_resume_pending) {
        return -1;
    }
    /* With no list of the user's own the scan never moves (channels learned over the air are no list): the receiver
       stays the operator's, so its calls go on and it stays the operator's tuner, as a stop over such a session ends
       nothing (channel_scan_untyped_runs()). */
    const int users_list = dsd_state_trunk_lcn_user_list_present(opts, state);
    /* A walk off the scan still waiting on its tune would land once the scan owns the tuner, on a row the scan never
       chose (issue #644): the scan takes it over as its own walk, with no tune of its own, so its landing commits its
       row and its failure passes it as the scan's walk does. One the scan cannot take (a typed list's, a row rewritten
       since) refuses the start, which changes nothing: the walk settles off the scan, and a start after it runs. */
    const int walk = users_list && dsd_engine_manual_tune_operator_walk_noted();
    const int adopted = walk && dsd_engine_manual_tune_adopt_walk(state);
    if (walk && !adopted) {
        channel_scan_toast_resume(state, "Scanner not started: channel change still pending; retry");
        return -1;
    }
    if (users_list) {
        channel_scan_start_settles_hold(opts, state, adopted, 0);
        /* The calls heard so far end under what they began with (no row, the operator's tuner after a manual tune):
           only then does the tuner become the scan's, so what is heard next is the scan's, as a resume's landing
           does. */
        channel_scan_end_calls(opts, state);
        state->operator_tuned = 0U;
    }
    opts->scanner_mode = 1;
    return 0;
}

void
dsd_engine_channel_scan_resume_landed(dsd_opts* opts, dsd_state* state) {
    if (!state->lcn_scan_resume_pending) {
        return;
    }
    opts->scanner_mode = 1;
    state->lcn_scan_paused = 0U;
    state->lcn_scan_resume_pending = 0U;
    state->operator_tuned = 0U;
}

void
dsd_engine_scan_toast_resume_failed(dsd_state* state, long freq, int why) {
    char mhz[16];
    (void)dsd_freq_hz_mhz_text(freq > 0 && (unsigned long)freq <= UINT32_MAX ? (uint32_t)freq : 0U, mhz, sizeof mhz);
    char text[sizeof state->ui_msg];
    if (why == DSD_SCAN_RESUME_FAILED_DEFERRED) {
        DSD_SNPRINTF(text, sizeof text, "Scan stays paused: tune to %s MHz deferred; retry", mhz);
    } else if (why == DSD_SCAN_RESUME_FAILED_SETUP) {
        DSD_SNPRINTF(text, sizeof text, "Scan stays paused: scan channel %s MHz could not be set up", mhz);
    } else if (why == DSD_SCAN_RESUME_INTERRUPTED) {
        /* Not a failure: a reopened stream starts on the frequency the tune asked for, so the receiver can be there. */
        DSD_SNPRINTF(text, sizeof text, "Scan stays paused: the resume to %s MHz was interrupted", mhz);
    } else {
        DSD_SNPRINTF(text, sizeof text, "Scan stays paused: tune to %s MHz failed", mhz);
    }
    channel_scan_toast_resume(state, text);
}

/* The goto outcome a resume's failure @p why names, for an entry the resume carried (issue #644); HOLDING for an
   interrupted resume, which a stream reopen ends: no failure of the entry's. */
static dsd_scan_goto_result
channel_scan_resume_failure_result(int why) {
    switch (why) {
        case DSD_SCAN_RESUME_FAILED_DEFERRED: return DSD_SCAN_GOTO_DEFERRED;
        case DSD_SCAN_RESUME_FAILED_SETUP: return DSD_SCAN_GOTO_SETUP_FAILED;
        case DSD_SCAN_RESUME_INTERRUPTED: return DSD_SCAN_GOTO_HOLDING;
        default: return DSD_SCAN_GOTO_REFUSED;
    }
}

void
dsd_engine_channel_scan_resume_failed(dsd_opts* opts, dsd_state* state, long freq, int why) {
    /* A resume-at's entry failed with its resume: kept for its toast before the pause below cancels it (issue #644). */
    const dsd_scan_goto_result entry_result = channel_scan_resume_failure_result(why);
    if (entry_result != DSD_SCAN_GOTO_HOLDING) {
        dsd_engine_scan_entry_intent_fail(state, entry_result);
    }
    if (dsd_channel_modes_present(state)) {
        /* The pause again, in place of the row attempt: every scope it began is detached and the configured front end
           is asked back, through the pause app-control installed, which records that request for the decoder as an
           interactive pause does (issue #578). The attempt ended the manual calls before it tuned, and no frame
           decoded while its row was outstanding. */
        const dsd_channel_scan_pause_fn pause = dsd_channel_scan_pause_hook_get();
        (void)(pause ? pause(opts, state, 0) : dsd_engine_channel_scan_pause(opts, state, 0));
    } else {
        /* An untyped row holds no scope, and its keys were never installed: the scan only goes back to paused. A
           transmission heard on the manual frequency while the row was outstanding goes on as one call. */
        opts->scanner_mode = 0;
        dsd_engine_scan_entry_intent_cancel(state);
        state->lcn_scan_hold = 0U;
        state->lcn_scan_entry_hold = 0U;
        state->lcn_scan_paused = 1U;
        state->lcn_scan_resume_pending = 0U;
        state->operator_tuned = 1U;
    }
    dsd_engine_scan_toast_resume_failed(state, freq, why);
}

/* The row a resume lands on (N2): the first from the kept cursor, wrapping, that a step can land on. A typed list skips
   the rows a step skips before any backend moves (channel_scan_row_reachable(): an analog row whose width the front end
   refuses, an am row nothing demodulates), since a resume that failed on one would keep its cursor and try it again;
   an untyped list, every row with a frequency that is not avoided. -1 for none. */
static int
channel_scan_resume_row(const dsd_opts* opts, const dsd_state* state) {
    if (!dsd_channel_modes_present(state)) {
        return dsd_engine_scan_untyped_find_row(state, state->lcn_freq_roll, 1);
    }
    const int count = state->lcn_freq_count;
    if (count <= 0) {
        return -1;
    }
    const int start = (state->lcn_freq_roll < 0 || state->lcn_freq_roll >= count) ? 0 : state->lcn_freq_roll;
    const int dsp_rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    for (int pass = 0; pass < 2; pass++) {
        const int first = pass == 0 ? start : 0;
        const int end = pass == 0 ? count : start;
        for (int row = first; row < end; row++) {
            if (channel_scan_row_reachable(opts, state, row, dsp_rate_hz)) {
                return row;
            }
        }
    }
    return -1;
}

/* An untyped resume's row (N2): entered as the timed step enters one, published at once when it lands. Returns 1 when
   its tune was refused or deferred, with @p why saying which, so a deferral reads as the typed list's does (issue
   #666); 0 when it landed or is pending. */
static int
channel_scan_resume_untyped_row(dsd_opts* opts, dsd_state* state, int row, int* why) {
    const int result = dsd_engine_scan_untyped_enter_row(opts, state, row, dsd_decode_time());
    if (result == DSD_UNTYPED_STEP_OK) {
        dsd_engine_channel_scan_resume_landed(opts, state);
        return 0;
    }
    *why = result == DSD_UNTYPED_STEP_DEFERRED ? DSD_SCAN_RESUME_FAILED_DEFERRED : DSD_SCAN_RESUME_FAILED_TUNE;
    return result != DSD_UNTYPED_STEP_PENDING;
}

/* Resume the paused scan on @p row (issue #671), with @p entry an entry waiting on its landing (issue #644): the manual
   session's calls end, a new scan session begins, and the row is tuned as a step would tune it. Returns a
   DSD_SCAN_RESUME_* result; @p why says why a REFUSED never landed (DSD_SCAN_RESUME_FAILED_*). */
static int
channel_scan_resume_on(dsd_opts* opts, dsd_state* state, int row, int entry, int* why) {
    /* The manual session's calls end under no row's name, before any row is tuned (F3). */
    channel_scan_end_calls(opts, state);
    opts->trunk_enable = 0;
    dsd_engine_legacy_tune_caches_forget();
    state->lcn_scan_session++;
    state->lcn_scan_resume_pending = 1U;
    if (entry) {
        dsd_engine_scan_entry_intent_set(state, row, 0U);
    } else {
        dsd_engine_scan_entry_intent_cancel(state);
    }
    const int kept_roll = state->lcn_freq_roll;
    const long freq = *dsd_state_trunk_lcn_slot_const(state, row);
    int refused = 0;
    *why = DSD_SCAN_RESUME_FAILED_TUNE;
    if (dsd_channel_modes_present(state)) {
        refused = channel_scan_start_row_owned(opts, state, row) < 0;
        channel_scan* scan = channel_scan_get(state); /* the first row of a scan creates it */
        if (scan) {
            scan->resume_roll = kept_roll;
            *why = scan->start_failure;
        } else {
            *why = DSD_SCAN_RESUME_FAILED_SETUP;
        }
    } else {
        refused = channel_scan_resume_untyped_row(opts, state, row, why);
    }
    if (!state->lcn_scan_resume_pending) {
        return DSD_SCAN_RESUME_LANDED;
    }
    if (!refused) {
        return DSD_SCAN_RESUME_PENDING;
    }
    state->lcn_freq_roll = kept_roll;
    dsd_engine_channel_scan_resume_failed(opts, state, freq, *why);
    return DSD_SCAN_RESUME_REFUSED;
}

int
dsd_engine_channel_scan_resume(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return DSD_SCAN_RESUME_REFUSED;
    }
    if (state->lcn_scan_resume_pending) {
        return DSD_SCAN_RESUME_PENDING;
    }
    if (opts->trunk_scan_enabled == 1) {
        channel_scan_toast_resume(state, "Trunk scan active: conventional scanner unavailable");
        return DSD_SCAN_RESUME_REFUSED;
    }
    if (!state->lcn_scan_paused) {
        if (opts->scanner_mode == 1) {
            return DSD_SCAN_RESUME_LANDED; /* already running: nothing to resume */
        }
        channel_scan_toast_resume(state, "Not scanning: no paused scan to resume");
        return DSD_SCAN_RESUME_REFUSED;
    }
    /* A landing first (N2): without a row to go to, the resume never starts and the manual session goes on. */
    const int row = channel_scan_resume_row(opts, state);
    if (row < 0) {
        channel_scan_toast_resume(state, "Scan stays paused: no scan channel to resume on");
        return DSD_SCAN_RESUME_NO_ROW;
    }
    int why = DSD_SCAN_RESUME_FAILED_TUNE;
    return channel_scan_resume_on(opts, state, row, 0, &why);
}

/* Store @p next as @p row's session edit, dropping the entry once it edits nothing. Returns -1 when it cannot be
   stored (no memory), with nothing changed. */
static int
channel_scan_store_edit(channel_scan* scan, int row, const dsd_scan_row_edit* next) {
    channel_scan_row_edit* entry = channel_scan_find_edit(scan, row);
    if (dsd_scan_row_edit_fields_edited(next) == 0U) {
        if (entry) {
            *entry = scan->edits[scan->edit_count - 1U];
            scan->edit_count--;
        }
        return 0;
    }
    if (!entry) {
        if (scan->edit_count == scan->edit_capacity) {
            const size_t capacity = scan->edit_capacity ? scan->edit_capacity * 2U : 4U;
            channel_scan_row_edit* grown =
                (channel_scan_row_edit*)realloc(scan->edits, capacity * sizeof(*scan->edits));
            if (!grown) {
                return -1;
            }
            scan->edits = grown;
            scan->edit_capacity = capacity;
        }
        entry = &scan->edits[scan->edit_count++];
        entry->row = row;
    }
    entry->edit = *next;
    return 0;
}

/* Install @p next as @p row's session edit: from its next visit when its scope is not the one in force, at once when it
   is. A tune staged before the change restages (edits_epoch). */
static int
channel_scan_install_edit(dsd_opts* opts, dsd_state* state, channel_scan* scan, int row, const dsd_scan_row_edit* next,
                          dsd_scan_row_edit_result* out) {
    const channel_scan_row_edit* entry = channel_scan_find_edit(scan, row);
    if (entry) {
        out->previous = entry->edit;
    }
    const int width_before = dsd_opts_analog_width_hz(opts);
    dsd_scan_option_values before_scratch;
    const dsd_scan_option_values* before = channel_scan_row_values(state, row, &before_scratch);
    if (channel_scan_store_edit(scan, row, next) != 0) {
        DSD_SNPRINTF(out->err, sizeof out->err, "%s", "out of memory");
        return DSD_SCAN_ROW_EDIT_REFUSED;
    }
    scan->edits_epoch++;
    dsd_scan_option_values scratch;
    const dsd_scan_option_values* values = channel_scan_row_values(state, row, &scratch);
    if (values) {
        out->options = *values;
    }
    if (row != scan->committed_row) {
        return DSD_SCAN_ROW_EDIT_STORED;
    }
    const int receive_changed = dsd_scan_option_receive_differs(before, values);
    const int filters_were_enabled = dsd_receive_filters_enabled(opts);
    (void)dsd_scan_mode_options(opts, state, values);
    if (receive_changed) {
        dsd_frame_sync_hook_receive_policy_boundary(opts, state, filters_were_enabled);
    }
    /* The row on air is the one its last tune staged: its staged options carry the edit too, for whatever reads the
       tune's (dsd_engine_scan_tuning_row_options()). A live rigctl request reads the scope in force. */
    if (row == scan->row) {
        scan->staged_has_values = values != NULL;
        if (values) {
            scan->staged_values = *values;
        }
    }
    /* A tune still outstanding restages with the new options and lands them itself. Otherwise the receiver's request
       changed if the width the decoder runs did, or, for a rigctl peer, whether the row sets one of its own. */
    out->publish_width =
        dsd_scan_mode_is_analog(dsd_channel_mode_get(state, (size_t)row)) && scan->request == 0 && !scan->retry
        && (dsd_opts_analog_width_hz(opts) != width_before || dsd_scan_row_edit_width_request_differs(before, values));
    channel_scan_publish_row(state, scan);
    return DSD_SCAN_ROW_EDIT_APPLIED;
}

/* The -Y scan @p session names, or the reason it is not this one. */
static int
channel_scan_edit_scan(const dsd_opts* opts, const dsd_state* state, uint32_t session, int row, channel_scan** out) {
    *out = NULL;
    if (!opts || !state || opts->scanner_mode != 1 || opts->trunk_scan_enabled == 1) {
        return DSD_SCAN_ROW_EDIT_UNAVAILABLE;
    }
    channel_scan* scan = channel_scan_get(state);
    if (!scan || session == 0U || session != scan->session || row < 0 || row >= state->lcn_freq_count) {
        return DSD_SCAN_ROW_EDIT_STALE;
    }
    *out = scan;
    return 0;
}

int
dsd_engine_channel_scan_edit_row(dsd_opts* opts, dsd_state* state, uint32_t session, int row, uint32_t field,
                                 int action, const dsd_scan_row_edit_value* value, dsd_scan_row_edit_result* out) {
    dsd_scan_row_edit_result local;
    dsd_scan_row_edit_result* result = out ? out : &local;
    DSD_MEMSET(result, 0, sizeof(*result));
    channel_scan* scan = NULL;
    const int rc = channel_scan_edit_scan(opts, state, session, row, &scan);
    if (rc != 0) {
        return rc;
    }
    const dsd_scan_mode mode = dsd_channel_mode_get(state, (size_t)row);
    if (!(dsd_scan_row_edit_fields(mode, 0) & field)) {
        DSD_SNPRINTF(result->err, sizeof result->err, "%s", "this channel cannot take that setting");
        return DSD_SCAN_ROW_EDIT_REFUSED;
    }
    if (action == DSD_SCAN_ROW_EDIT_SET
        && !dsd_scan_row_edit_value_valid(mode, field, value, result->err, sizeof result->err)) {
        return DSD_SCAN_ROW_EDIT_REFUSED;
    }
    const channel_scan_row_edit* entry = channel_scan_find_edit(scan, row);
    dsd_scan_row_edit next;
    if (entry) {
        next = entry->edit;
    } else {
        DSD_MEMSET(&next, 0, sizeof(next));
    }
    if (dsd_scan_row_edit_change(&next, field, (dsd_scan_row_edit_action)action, value) != 0) {
        DSD_SNPRINTF(result->err, sizeof result->err, "%s", "invalid edit");
        return DSD_SCAN_ROW_EDIT_REFUSED;
    }
    /* A width the DSP rate cannot filter would only skip the row at every visit; the list's own width (RESET) was
       named when the scan checked the map. */
    const int rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    if (field == DSD_SCAN_ROW_FIELD_WIDTH && action != DSD_SCAN_ROW_EDIT_RESET && rate_hz > 0) {
        const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
        dsd_scan_option_values after;
        dsd_scan_row_edit_apply(profile ? &profile->values : NULL, &next, mode, &after);
        char brief[DSD_ANALOG_ERROR_TEXT_MAX] = {0};
        if (dsd_engine_scan_analog_width_skipped(opts, state, &after, dsd_scan_mode_analog_kind(mode), rate_hz, brief,
                                                 sizeof brief)) {
            DSD_SNPRINTF(result->err, sizeof result->err, "%s",
                         brief[0] ? brief : "the DSP rate cannot filter that width");
            return DSD_SCAN_ROW_EDIT_REFUSED;
        }
    }
    return channel_scan_install_edit(opts, state, scan, row, &next, result);
}

int
dsd_engine_channel_scan_restore_row_edit(dsd_opts* opts, dsd_state* state, uint32_t session, int row, uint32_t fields,
                                         const dsd_scan_row_edit* edit, dsd_scan_row_edit_result* out) {
    dsd_scan_row_edit_result local;
    dsd_scan_row_edit_result* result = out ? out : &local;
    /* @p edit may be @p out's previous edit: take it before the result is cleared. */
    dsd_scan_row_edit from = {0};
    if (edit) {
        from = *edit;
    }
    DSD_MEMSET(result, 0, sizeof(*result));
    channel_scan* scan = NULL;
    const int rc = channel_scan_edit_scan(opts, state, session, row, &scan);
    if (rc != 0) {
        return rc;
    }
    if (!edit) {
        return DSD_SCAN_ROW_EDIT_UNAVAILABLE;
    }
    const channel_scan_row_edit* entry = channel_scan_find_edit(scan, row);
    dsd_scan_row_edit next = {0};
    if (entry) {
        next = entry->edit;
    }
    dsd_scan_row_edit_take_fields(&next, &from, fields);
    return channel_scan_install_edit(opts, state, scan, row, &next, result);
}

/* Whether @p row is one a go-to takes (issue #644): in the list, and a row a step can land on
   (channel_scan_row_reachable()). */
static int
channel_scan_goto_row_valid(const dsd_opts* opts, const dsd_state* state, int row) {
    return row >= 0 && row < state->lcn_freq_count
           && channel_scan_row_reachable(opts, state, row, dsd_engine_scan_dsp_rate_hz(opts, state));
}

/* Whether untyped row @p row belongs to a channel the operator avoided (issue #644 D4, D5): on a list without modes a
   run of adjacent rows on one frequency is one channel, which its first row stands for, as the walk and the match take
   it (an avoided first row skips its run), so a row of that run that is not confirmed on air is no go-to's. */
static int
channel_scan_untyped_run_avoided(const dsd_state* state, int row) {
    if (dsd_channel_modes_present(state)) {
        return 0;
    }
    const long freq = *dsd_state_trunk_lcn_slot_const(state, row);
    int first = row;
    while (first > 0 && *dsd_state_trunk_lcn_slot_const(state, first - 1) == freq) {
        first--;
    }
    return dsd_state_trunk_lcn_avoid_get(state, (size_t)first);
}

/* Whether a -Y scan over a list of the user's own runs: not paused, no resume pending, no trunk scan. */
static int
channel_scan_runs_users_list(const dsd_opts* opts, const dsd_state* state) {
    return opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1 && !state->lcn_scan_paused
           && !state->lcn_scan_resume_pending && dsd_state_trunk_lcn_user_list_present(opts, state);
}

/* A typed list's go-to of @p row (issue #644): the row prepared apart and swapped in for its tune. A preparation
   that fails, or a tune refused or deferred before any backend moved, puts back the row staged before, its tune and
   an earlier entry (@p retired 0). Once the tune is taken, or a backend moved, the row staged before is retired
   (@p retired 1), its request let go, and the tune's answer is a forward step's. */
static dsd_scan_goto_result
channel_scan_goto_typed(dsd_opts* opts, dsd_state* state, int row, int* retired) {
    *retired = 0;
    channel_scan* scan = channel_scan_obtain(state);
    if (!scan) {
        return DSD_SCAN_GOTO_SETUP_FAILED;
    }
    channel_scan_staging staging;
    dsd_scan_settings next;
    if (channel_scan_prepare_row(opts, state, scan, row, *dsd_state_trunk_lcn_slot_const(state, row), 1, &staging,
                                 &next)
        != 0) {
        channel_scan_staging_clear(&staging);
        return DSD_SCAN_GOTO_SETUP_FAILED;
    }
    channel_scan_staging_swap(scan, &staging);
    const dsd_trunk_tune_result result = channel_scan_tune_staged(opts, state, scan, &next);
    const int taken = result == DSD_TRUNK_TUNE_RESULT_OK || result == DSD_TRUNK_TUNE_RESULT_PENDING;
    if (!taken && !dsd_engine_scan_tune_moved()) {
        /* Nothing moved: the row staged before, its tune, an earlier entry and the row confirmed on air stand. The
           refused request has nothing staged for it any more, so it keeps no frame gate closed (a rigctl leg's
           refusal on an RTL input leaves it gated, as if the leg might have moved). */
        channel_scan_staging_swap(scan, &staging);
        if (staging.request != 0U) {
            dsd_trunk_tuning_request_disown(staging.request);
        }
        channel_scan_staging_clear(&staging);
        return result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? DSD_SCAN_GOTO_DEFERRED : DSD_SCAN_GOTO_REFUSED;
    }
    *retired = 1;
    if (staging.request != 0U) {
        dsd_trunk_tuning_request_disown(staging.request);
    }
    channel_scan_staging_clear(&staging);
    /* The receiver left the row landed, or will: it is no longer known to be on air (issue #667 D9). The entry is this
       attempt's, replacing any earlier one, and an entry hold on the row left goes with it. */
    dsd_engine_scan_landing_forget(state);
    dsd_engine_scan_entry_hold_replaced(state);
    dsd_engine_scan_entry_intent_set(state, row, scan->attempt);
    dsd_state_lcn_back_retry_clear(state);
    if (channel_scan_tune_outcome(opts, state, scan, result) > 0) {
        dsd_engine_scan_entry_hold_now(state);
        return DSD_SCAN_GOTO_HOLDING;
    }
    /* A row that landed but restages, or whose settings are retried, is still outstanding, as a step's would be. */
    if (taken) {
        return DSD_SCAN_GOTO_PENDING;
    }
    return result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? DSD_SCAN_GOTO_DEFERRED : DSD_SCAN_GOTO_REFUSED;
}

/* Whether typed row @p row's tune is already outstanding in this scan (issue #644): the attempt staged for it, by a
   go-to or a step, still waits on its request or a retry, on the list it was staged from. A go-to then waits on that
   attempt: its landing holds the row, with no second tune, as the trunk scan's go-to waits on a park in flight. */
static int
channel_scan_row_in_flight(const dsd_opts* opts, const dsd_state* state, int row) {
    const channel_scan* scan = channel_scan_get(state);
    return scan && (scan->request != 0 || scan->retry) && scan->row == row
           && channel_scan_row_is_current(opts, state, scan);
}

dsd_scan_goto_result
dsd_engine_channel_scan_goto_row(dsd_opts* opts, dsd_state* state, int row) {
    if (!opts || !state || !channel_scan_runs_users_list(opts, state)
        || !channel_scan_goto_row_valid(opts, state, row)) {
        return DSD_SCAN_GOTO_REFUSED;
    }
    /* The entry's intent lives in the scan owner: without one no landing could hold the row. */
    if (!dsd_engine_scan_owner_ensure(state)) {
        return DSD_SCAN_GOTO_SETUP_FAILED;
    }
    if (dsd_engine_channel_scan_row_confirmed(state, row)) {
        /* On air already: the hold only. */
        dsd_engine_scan_entry_hold_now(state);
        return DSD_SCAN_GOTO_HOLDING;
    }
    if (!dsd_channel_modes_present(state)) {
        return channel_scan_untyped_run_avoided(state, row) ? DSD_SCAN_GOTO_REFUSED
                                                            : dsd_engine_scan_untyped_goto_row(opts, state, row);
    }
    if (channel_scan_row_in_flight(opts, state, row)) {
        /* An entry hold on the row left goes, as at any go-to; the attempt's landing sets it again. */
        dsd_engine_scan_entry_hold_replaced(state);
        dsd_engine_scan_entry_intent_set(state, row, channel_scan_get(state)->attempt);
        return DSD_SCAN_GOTO_PENDING;
    }
    int retired = 0;
    return channel_scan_goto_typed(opts, state, row, &retired);
}

/* What a resume's DSD_SCAN_RESUME_* @p result, with @p why for a refusal, is to a go-to. */
static dsd_scan_goto_result
channel_scan_goto_of_resume(int result, int why) {
    switch (result) {
        case DSD_SCAN_RESUME_LANDED: return DSD_SCAN_GOTO_HOLDING;
        case DSD_SCAN_RESUME_PENDING: return DSD_SCAN_GOTO_PENDING;
        default: break;
    }
    if (why == DSD_SCAN_RESUME_FAILED_DEFERRED) {
        return DSD_SCAN_GOTO_DEFERRED;
    }
    return why == DSD_SCAN_RESUME_FAILED_SETUP ? DSD_SCAN_GOTO_SETUP_FAILED : DSD_SCAN_GOTO_REFUSED;
}

/* A resume-at over a typed resume still pending: the go-to's row staged apart, under the resume's ownership. A
   refusal after a backend moved retired that resume too, so the scan stays paused, its cursor back where the resume
   found its row. A resume whose tune is for that row already is the entry's: it waits on that tune, with no second
   one, and the resume's landing holds the row. */
static dsd_scan_goto_result
channel_scan_resume_at_typed_pending(dsd_opts* opts, dsd_state* state, int row) {
    if (channel_scan_row_in_flight(opts, state, row)) {
        dsd_engine_scan_entry_intent_set(state, row, channel_scan_get(state)->attempt);
        return DSD_SCAN_GOTO_PENDING;
    }
    const int owned = channel_scan_resume_owns(opts, state);
    int retired = 0;
    const dsd_scan_goto_result result = channel_scan_goto_typed(opts, state, row, &retired);
    channel_scan_resume_disowns(opts, state, owned);
    const channel_scan* scan = channel_scan_get(state);
    if (retired && scan && state->lcn_scan_resume_pending
        && (result == DSD_SCAN_GOTO_REFUSED || result == DSD_SCAN_GOTO_DEFERRED)) {
        channel_scan_resume_row_failed(opts, state, scan, scan->start_failure);
    }
    return result;
}

dsd_scan_goto_result
dsd_engine_channel_scan_resume_at(dsd_opts* opts, dsd_state* state, int row) {
    if (!opts || !state || opts->trunk_scan_enabled == 1 || !state->lcn_scan_paused
        || !dsd_state_trunk_lcn_user_list_present(opts, state) || !channel_scan_goto_row_valid(opts, state, row)
        || channel_scan_untyped_run_avoided(state, row)) {
        return DSD_SCAN_GOTO_REFUSED;
    }
    if (!dsd_engine_scan_owner_ensure(state)) {
        return DSD_SCAN_GOTO_SETUP_FAILED;
    }
    if (state->lcn_scan_resume_pending) {
        /* An untyped resume still pending is the untyped go-to's to replace (engine.c). */
        return dsd_channel_modes_present(state) ? channel_scan_resume_at_typed_pending(opts, state, row)
                                                : dsd_engine_scan_untyped_goto_row(opts, state, row);
    }
    int why = DSD_SCAN_RESUME_FAILED_TUNE;
    const int resumed = channel_scan_resume_on(opts, state, row, 1, &why);
    const dsd_scan_goto_result result = channel_scan_goto_of_resume(resumed, why);
    if (result == DSD_SCAN_GOTO_HOLDING) {
        /* Landed within this call (an untyped row's landing names no tune an entry waits on). */
        dsd_engine_scan_entry_hold_now(state);
    }
    return result;
}

void
dsd_engine_channel_scan_entry_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (!state) {
        return;
    }
    dsd_engine_scan_entry_intent_cancel(state);
    if (!state->lcn_scan_entry_hold) {
        return;
    }
    state->lcn_scan_hold = 0U;
    state->lcn_scan_entry_hold = 0U;
    /* The rotation left its windows alone while held: the row gets a full hangtime from now, as at `Y` off. */
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    state->scan_visit_since_m = state->last_cc_sync_time_m;
    state->scan_visit_roll_seen = state->lcn_freq_roll;
    state->scan_visit_rearm_pending = 0U;
}

void
dsd_engine_channel_scan_entry_cancel(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_engine_scan_entry_intent_cancel(state);
    state->lcn_scan_entry_hold = 0U;
}

void
dsd_engine_channel_scan_entry_supersede(dsd_state* state) {
    dsd_engine_scan_entry_intent_cancel(state);
}

int
dsd_engine_channel_scan_entry_pending(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    const dsd_engine_scan_entry_intent intent = dsd_engine_scan_entry_intent_get(state);
    return intent.active && dsd_engine_scan_entry_intent_is(state, intent.row);
}
