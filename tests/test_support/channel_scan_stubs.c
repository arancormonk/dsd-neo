// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>
#include <stdint.h>
#include "channel_scan_stubs.h"

static int g_leave_saw_scanner_mode = -1;
static int g_leave_saw_row_keys = -1;

void
dsd_test_channel_scan_leave_seen(int* scanner_mode, int* row_keys) {
    *scanner_mode = g_leave_saw_scanner_mode;
    *row_keys = g_leave_saw_row_keys;
}

/* Action-only tests have no tuner; exercise the production scoped-settings teardown. Nothing is asked of a monitor. */
int
dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state) {
    g_leave_saw_scanner_mode = opts->scanner_mode;
    g_leave_saw_row_keys = state->scan_keys_active_set;
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, NULL, NULL);
    dsd_scan_mode_leave(opts, state);
    return 0;
}

/* Nor calls to end at a -Y start: the start's ownership flags only. ENGINE_CHANNEL_SCAN and ENGINE_NO_CARRIER_RESET
   drive the engine's, which ends the calls first (issue #644 B-4). */
void
dsd_engine_channel_scan_start(dsd_opts* opts, dsd_state* state) {
    opts->scanner_mode = 1;
    state->operator_tuned = 0U;
}

/* Nor a -Y scan to pause or resume (issue #671): ENGINE_CHANNEL_SCAN and ENGINE_NO_CARRIER_RESET drive the engine's. */
int
dsd_engine_channel_scan_pause(dsd_opts* opts, dsd_state* state, int in_place) {
    (void)in_place;
    return dsd_engine_channel_scan_leave(opts, state);
}

int
dsd_engine_channel_scan_resume(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return DSD_SCAN_RESUME_NO_ROW;
}

/* Nor a trunk scan to pause or resume (issue #671): ENGINE_TRUNK_SCAN and APP_COMMAND_QUEUE drive the coordinator's. */
int
dsd_engine_trunk_scan_pause_ex_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_pause_kind kind,
                                      dsd_trunk_scan_leave_report* out) {
    (void)opts;
    (void)state;
    (void)kind;
    if (out) {
        out->monitor_request = 0;
        out->restart_gain = 0;
    }
    return -1;
}

int
dsd_engine_trunk_scan_resume_ex_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_leave_report* out) {
    (void)opts;
    (void)state;
    if (out) {
        out->monitor_request = 0;
        out->restart_gain = 0;
    }
    return -1;
}

/* Nor a scan to send to the row a typed frequency picked (issue #644): ENGINE_CHANNEL_SCAN, ENGINE_NO_CARRIER_RESET,
   ENGINE_TRUNK_SCAN and APP_COMMAND_QUEUE drive the engine's go-to, entry hold and its release. */
dsd_scan_goto_result
dsd_engine_trunk_scan_goto_locked(dsd_opts* opts, dsd_state* state, int index, dsd_trunk_scan_goto_report* report) {
    (void)opts;
    (void)state;
    (void)index;
    if (report) {
        report->monitor_request = 0;
        report->restart_gain = 0;
        report->park_hz = 0;
    }
    return DSD_SCAN_GOTO_REFUSED;
}

dsd_scan_goto_result
dsd_engine_channel_scan_goto_row(dsd_opts* opts, dsd_state* state, int row) {
    (void)opts;
    (void)state;
    (void)row;
    return DSD_SCAN_GOTO_REFUSED;
}

dsd_scan_goto_result
dsd_engine_channel_scan_resume_at(dsd_opts* opts, dsd_state* state, int row) {
    (void)opts;
    (void)state;
    (void)row;
    return DSD_SCAN_GOTO_REFUSED;
}

void
dsd_engine_channel_scan_entry_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lcn_scan_hold = 0U;
    state->lcn_scan_entry_hold = 0U;
}

int
dsd_engine_channel_scan_entry_pending(const dsd_state* state) {
    (void)state;
    return 0;
}

void
dsd_engine_trunk_scan_entry_release_locked(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->trunk_scan_hold = 0U;
    state->trunk_scan_entry_hold = 0U;
}

int
dsd_engine_trunk_scan_entry_pending(const dsd_state* state) {
    (void)state;
    return 0;
}

int
dsd_engine_scan_entry_take_failure(dsd_state* state, dsd_scan_entry_failure_t* out) {
    (void)state;
    (void)out;
    return 0;
}

/* Nor a -Y tune or retry staged: a rigctl follow (issue #621) is not left to one. */
int
dsd_engine_channel_scan_waiting(const dsd_state* state) {
    (void)state;
    return 0;
}

/* Nor a running scan list: no row waits to run the configured NFM width (issue #526). APP_COMMAND_QUEUE drives the real
   list through the engine. */
int
dsd_engine_scan_runs_configured_nfm_width(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

/* Nor one that waits to run the configured AM width. */
int
dsd_engine_scan_runs_configured_am_width(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

/* Model the gain coordinator's three outcomes from the fixture's published scan state. UI_MENU_SERVICES exercises
   no scan, a target override, and an inherited gain; ENGINE_TRUNK_SCAN and APP_COMMAND_QUEUE drive the real owner. */
int
dsd_engine_trunk_scan_set_configured_gain(dsd_opts* opts, dsd_state* state, int gain) {
    if (!opts || !state || state->trunk_scan_target_count == 0) {
        return -1;
    }
    state->trunk_scan_configured_gain = (int16_t)gain;
    if (state->trunk_scan_gain_override) {
        return 0;
    }
    opts->rtl_gain_value = gain;
    return 1;
}

int
dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on) {
    (void)state;
    if (out_on) {
        *out_on = 0;
    }
    return 0;
}

/* Nor a channel map whose row widths a front end refuses: the width rules run in the engine, which ENGINE_CHANNEL_SCAN
   and APP_COMMAND_QUEUE drive. */
int
dsd_engine_channel_scan_refused_rows(const dsd_opts* opts, const dsd_state* state, int dsp_rate_hz, int* first_row,
                                     char* brief, size_t brief_size) {
    (void)opts;
    (void)state;
    (void)dsp_rate_hz;
    if (first_row) {
        *first_row = -1;
    }
    if (brief && brief_size > 0U) {
        brief[0] = '\0';
    }
    return 0;
}

/* Nor an I/Q replay whose decode clock a stream stop would leave (issue #572): ENGINE_REPLAY_DECODE_CLOCK drives the
   engine's. */
void
dsd_engine_decode_clock_leave_replay(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}
