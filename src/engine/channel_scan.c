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

#include "scan_analog_internal.h"

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
    /* The live receive-family requests the front end had accepted when the staged tune was queued
     * (dsd_engine_scan_family_requests(); issue #526), and whether that tune attached a receive family, which is all a
     * later request supersedes (dsd_engine_scan_retune_attaches_family()). */
    uint32_t family_requests;
    int family_attached;
    /* The map generation whose rows were last checked against the input (issues #521, #526), and the DSP rate its
     * analog row widths were last held to, with the configured NFM width a row without its own runs (issue #526). */
    uint64_t rows_checked_map;
    int rows_checked;
    int widths_checked_rate_hz;
    int widths_checked_nfm_hz;
    int widths_checked;
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
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)scan->row);
    return !(profile && (profile->values.present & DSD_SCAN_OPT_BANDWIDTH));
}

/* Whether a configured setting the staged tune was prepared from changed while it was outstanding. The configured
 * channel widths count only where the tune carries one (channel_scan_row_runs_configured_width()), although
 * dsd_scan_settings_equal() compares them whenever the configured family is analog. */
static int
channel_scan_configured_changed(const dsd_state* state, const dsd_scan_settings* latest, const channel_scan* scan) {
    dsd_scan_settings others = *latest;
    others.analog_nfm_bandwidth_hz = scan->configured.analog_nfm_bandwidth_hz;
    others.analog_am_bandwidth_hz = scan->configured.analog_am_bandwidth_hz;
    if (!dsd_scan_settings_equal(&others, &scan->configured, 1)) {
        return 1;
    }
    return channel_scan_row_runs_configured_width(state, scan)
           && (latest->analog_nfm_bandwidth_hz != scan->configured.analog_nfm_bandwidth_hz
               || latest->analog_am_bandwidth_hz != scan->configured.analog_am_bandwidth_hz);
}

/* Whether the tune staged for the row no longer describes what it should land: a configured setting or the keyring it
 * was prepared from changed while it was outstanding, or a live receive-family request superseded the family its
 * retune carries. That request acts for the row still in scope (a width edit or a config apply republishing the
 * outgoing nfm row's analog monitor), yet the front end takes it as the newer word on the family and lands the staged
 * retune with neither its family nor its symbol profile (rtl_stream_prepare_retune_analog_profile_for_target()): a
 * digital row would otherwise commit on the analog monitor, or an nfm row on the digital family (issue #526). A retune
 * that carries no family lands its symbol profile whatever the requests, so it is not restaged for them. */
static int
channel_scan_staged_stale(const dsd_opts* opts, const dsd_state* state, const channel_scan* scan) {
    dsd_scan_settings latest;
    dsd_scan_mode_configured(opts, state, &latest);
    return channel_scan_configured_changed(state, &latest, scan) || scan->key_epoch != state->enc_lockout_key_epoch
           || (scan->family_attached && scan->family_requests != dsd_engine_scan_family_requests(opts));
}

static int
channel_scan_commit(dsd_opts* opts, dsd_state* state, channel_scan* scan) {
    /* Row commits run only in conventional scanner mode. Entry into that mode
     * is guarded (or precedes startup), and P25 recovery admission excludes it,
     * so policy installation cannot overlap an admitted watchdog tick (#554). */
    /* Hardware has moved. Even if a later retry rolls back, frames cannot use
     * the outgoing profile until a row is successfully committed. */
    scan->needs_commit = 1;
    if (channel_scan_staged_stale(opts, state, scan)) {
        /* Stage the new effective profile in a fresh request before any frame
         * can use it. */
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
        const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
        const dsd_scan_option_values* values = profile ? &profile->values : NULL;
        /* An analog row's squelch gates exactly what it is for on any input: its monitor and carrier. */
        if (!dsd_scan_mode_is_analog(dsd_channel_mode_get(state, (size_t)row))) {
            channel_scan_warn_row_squelch(opts, row, *dsd_state_trunk_lcn_slot_const(state, row), values);
            continue;
        }
        char label[64];
        channel_scan_row_label(state, row, label, sizeof label);
        (void)dsd_engine_scan_warn_analog_squelch(opts, state, values, label);
    }
}

/* Whether @p row is an analog row the scanner visits: a placeholder row (frequency 0) is never tuned. */
static int
channel_scan_row_visited_analog(const dsd_state* state, int row) {
    return *dsd_state_trunk_lcn_slot_const(state, row) != 0
           && dsd_scan_mode_is_analog(dsd_channel_mode_get(state, (size_t)row));
}

/* The widths the analog rows run, held to @p dsp_rate_hz: every row's, or, after the configured NFM width alone
 * changed (@p inherited_only), those of the rows that run it; a row with a width of its own was named at this rate
 * already, so it is counted without being named again. Every row skipped at every visit reaches the status line, since
 * a frontend without the log (Android) would otherwise never learn why a row is not visited. A placeholder row
 * (frequency 0) is never tuned and is left out, as dsd_engine_channel_scan_refused_rows() leaves it out. */
static void
channel_scan_warn_rows_width(const dsd_opts* opts, dsd_state* state, int dsp_rate_hz, int inherited_only) {
    dsd_engine_scan_skipped skipped = {0};
    for (int row = 0; row < state->lcn_freq_count; row++) {
        if (!channel_scan_row_visited_analog(state, row)) {
            continue;
        }
        const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
        const dsd_scan_option_values* values = profile ? &profile->values : NULL;
        char label[64];
        char brief[DSD_ANALOG_ERROR_TEXT_MAX];
        channel_scan_row_label(state, row, label, sizeof label);
        const int named = inherited_only && values && (values->present & DSD_SCAN_OPT_BANDWIDTH);
        const int row_skipped =
            named ? dsd_engine_scan_analog_width_skipped(opts, state, values, dsp_rate_hz, brief, sizeof brief)
                  : dsd_engine_scan_warn_analog_width(opts, state, values, dsp_rate_hz, label, brief, sizeof brief)
                        == DSD_ENGINE_SCAN_WIDTH_SKIPPED;
        if (row_skipped) {
            dsd_engine_scan_skipped_add(&skipped, label, brief);
        }
    }
    dsd_engine_scan_note_skipped_rows(state, &skipped);
}

/* Once per map the rows' squelch, and the analog row widths once the DSP rate they must fit is known: an RTL stream
 * that has published no rate yet leaves the widths to a later row start, and a changed rate (the operator changed the
 * RTL DSP bandwidth) names them again, since a row whose width the rate cannot fit is skipped quietly at every visit
 * (dsd_engine_scan_tune_to_freq()). A changed configured NFM width (the width command, a config apply) names again the
 * rows that run it. */
static void
channel_scan_check_rows(const dsd_opts* opts, dsd_state* state, channel_scan* scan) {
    if (!scan->rows_checked || scan->rows_checked_map != state->trunk_chan_map_seq) {
        scan->rows_checked = 1;
        scan->rows_checked_map = state->trunk_chan_map_seq;
        scan->widths_checked = 0;
        channel_scan_warn_rows_squelch(opts, state);
    }
    const int dsp_rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    const int nfm_hz = dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_FM);
    if (opts->audio_in_type == AUDIO_IN_RTL && dsp_rate_hz <= 0) {
        return;
    }
    const int same_rate = scan->widths_checked && scan->widths_checked_rate_hz == dsp_rate_hz;
    if (same_rate && scan->widths_checked_nfm_hz == nfm_hz) {
        return;
    }
    scan->widths_checked = 1;
    scan->widths_checked_rate_hz = dsp_rate_hz;
    scan->widths_checked_nfm_hz = nfm_hz;
    channel_scan_warn_rows_width(opts, state, dsp_rate_hz, same_rate);
}

/* The squelch an analog row runs with: its own, else the configured one. Off, or at -100 dB and below, holds the row
 * on noise: every block re-arms its carrier hold, so only the visit cap or a manual advance or avoid moves on. The
 * -100 dB comparison allows for the rounding of the level's power. */
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
dsd_engine_scan_warn_analog_squelch(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                    const char* label) {
    if (!opts || !state || !label || !channel_scan_analog_squelch_open(opts, state, row)) {
        return 0;
    }
    LOG_WARN(
        "WARNING: %s: the analog channel's squelch is off or at -100 dB or below, so noise holds it on air until "
        "--scan-max-visit-ms or a manual advance or avoid moves on; give it --squelch-db or set a squelch level.\n",
        label);
    return 1;
}

int
dsd_engine_scan_width_refused(const dsd_opts* opts, int kind, int width_hz, int dsp_rate_hz, char* why,
                              size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    if (!opts || width_hz <= 0 || opts->audio_in_type != AUDIO_IN_RTL) {
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
           && dsd_analog_width_check_at(kind, width_hz, dsp_rate_hz, dsd_opts_analog_rate_source(opts), why, why_size)
                  != 0;
}

/* The status-line reason a refused width is skipped for (dsd_engine_scan_width_refused()): short, as the log line
 * beside it carries the full text and the fix. */
static void
scan_width_refusal_brief(int kind, int width_hz, int dsp_rate_hz, char* brief, size_t brief_size) {
    if (!brief || brief_size == 0U) {
        return;
    }
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    char rate[DSD_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_analog_width_format(width_hz, width, sizeof width);
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
    state->ui_msg_expire = time(NULL) + 5;
}

/* A row without a width of its own runs the configured NFM width. The width commands hold that width to the DSP rate
 * while the scan has such a row (dsd_engine_scan_runs_configured_nfm_width()), but a list loaded over a width the rate
 * cannot filter, or a rate a device forced, still leaves one the rate cannot filter: that skips the row at every visit
 * as well. Audio input filters nothing, and the configured width is no row's to name there. */
static int
scan_warn_configured_nfm_width(const dsd_opts* opts, const dsd_state* state, int dsp_rate_hz, const char* label,
                               char* brief, size_t brief_size) {
    const int width_hz = dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_FM);
    if (width_hz <= 0 || dsp_rate_hz <= 0 || opts->audio_in_type != AUDIO_IN_RTL) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    char why[DSD_ANALOG_ERROR_TEXT_MAX];
    if (!dsd_engine_scan_width_refused(opts, DSD_ANALOG_DEMOD_FM, width_hz, dsp_rate_hz, why, sizeof why)) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    LOG_WARN("WARNING: %s: it sets no NFM width of its own, and the configured %s; until then it is skipped at every "
             "visit.\n",
             label, why);
    scan_width_refusal_brief(DSD_ANALOG_DEMOD_FM, width_hz, dsp_rate_hz, brief, brief_size);
    return DSD_ENGINE_SCAN_WIDTH_SKIPPED;
}

int
dsd_engine_scan_warn_analog_width(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                  int dsp_rate_hz, const char* label, char* brief, size_t brief_size) {
    if (brief && brief_size > 0U) {
        brief[0] = '\0';
    }
    if (!opts || !label) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    if (!row || !(row->present & DSD_SCAN_OPT_BANDWIDTH)) {
        return scan_warn_configured_nfm_width(opts, state, dsp_rate_hz, label, brief, brief_size);
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        LOG_WARN("WARNING: %s: --nfm-bandwidth-hz %d has no effect on audio input, which arrives demodulated; set "
                 "the demodulator's own passband (-B for a rigctl peer).\n",
                 label, row->channel_bw_hz);
        return DSD_ENGINE_SCAN_WIDTH_NO_EFFECT;
    }
    /* The row's rate rule (dsd_scan_option_width_check()), and DSD_NEO_CHANNEL_LPF=0, which refuses the width at any
     * rate; the text is the front end's (dsd_engine_scan_width_refused()). */
    if (dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, row, dsp_rate_hz, NULL, 0U) == 0
        && !dsd_engine_scan_channel_lpf_forced_off()) {
        return DSD_ENGINE_SCAN_WIDTH_OK;
    }
    char why[DSD_ANALOG_ERROR_TEXT_MAX];
    (void)dsd_engine_scan_width_refused(opts, DSD_ANALOG_DEMOD_FM, row->channel_bw_hz, dsp_rate_hz, why, sizeof why);
    LOG_WARN("WARNING: %s: %s; until then it is skipped at every visit.\n", label, why);
    scan_width_refusal_brief(DSD_ANALOG_DEMOD_FM, row->channel_bw_hz, dsp_rate_hz, brief, brief_size);
    return DSD_ENGINE_SCAN_WIDTH_SKIPPED;
}

int
dsd_engine_scan_analog_width_skipped(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* row,
                                     int dsp_rate_hz, char* brief, size_t brief_size) {
    if (brief && brief_size > 0U) {
        brief[0] = '\0';
    }
    if (!opts) {
        return 0;
    }
    const int width_hz = (row && (row->present & DSD_SCAN_OPT_BANDWIDTH))
                             ? row->channel_bw_hz
                             : dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_FM);
    if (!dsd_engine_scan_width_refused(opts, DSD_ANALOG_DEMOD_FM, width_hz, dsp_rate_hz, NULL, 0U)) {
        return 0;
    }
    scan_width_refusal_brief(DSD_ANALOG_DEMOD_FM, width_hz, dsp_rate_hz, brief, brief_size);
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
        const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
        char row_brief[DSD_ANALOG_ERROR_TEXT_MAX];
        if (!dsd_engine_scan_analog_width_skipped(opts, state, profile ? &profile->values : NULL, dsp_rate_hz,
                                                  row_brief, sizeof row_brief)) {
            continue;
        }
        if (refused++ == 0) {
            *first_row = row;
            DSD_SNPRINTF(brief, brief_size, "%s", row_brief);
        }
    }
    return refused;
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
    scan->family_requests = dsd_engine_scan_family_requests(opts);
    scan->family_attached = dsd_engine_scan_retune_attaches_family(opts, state, state->samplesPerSymbol);
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
