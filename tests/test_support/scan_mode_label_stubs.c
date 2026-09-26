// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <assert.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include "scan_mode_label_stubs.h"

static dsd_opts snapshot_opts;
static dsd_state snapshot_state;
static int snapshots_available = 1;
static dsd_scan_mode active_mode;
static dsd_scan_settings configured_settings;
static int have_configured;
static dsd_scan_option_values row_options;
static int have_row_options;

void
dsd_test_scan_labels_row_options(const dsd_scan_option_values* values) {
    have_row_options = values != NULL;
    if (values) {
        row_options = *values;
    }
}

const dsd_scan_option_values*
dsd_scan_mode_row_options(const dsd_state* state) {
    assert(state == &snapshot_state || state == NULL);
    return state && have_row_options ? &row_options : NULL;
}

int
dsd_scan_mode_is_analog(dsd_scan_mode mode) {
    return mode == DSD_SCAN_MODE_NFM;
}

void
dsd_test_scan_labels_configured(const dsd_scan_settings* settings) {
    have_configured = settings != NULL;
    if (settings) {
        configured_settings = *settings;
    }
}

void
dsd_test_scan_labels_set(int available, dsd_scan_mode mode) {
    snapshots_available = available;
    active_mode = mode;
}

const dsd_scan_settings*
dsd_scan_mode_configured_view(const dsd_state* state) {
    assert(state == &snapshot_state || state == NULL);
    return state && have_configured ? &configured_settings : NULL;
}

/* As scan_mode.c reads it: the stubbed configured view while one is set, dsd_opts otherwise. */
int
dsd_scan_mode_configured_analog_width(const dsd_opts* opts, const dsd_state* state, int kind) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    int width_hz = 0;
    if (configured) {
        width_hz =
            kind == DSD_ANALOG_DEMOD_AM ? configured->analog_am_bandwidth_hz : configured->analog_nfm_bandwidth_hz;
    } else if (opts) {
        width_hz = kind == DSD_ANALOG_DEMOD_AM ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
    }
    return width_hz > 0 ? width_hz : 0;
}

void
dsd_app_snapshot_configured_mode(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out) {
    assert(opts == &snapshot_opts);
    assert(state == &snapshot_state);
    out->use_cosine_filter = opts->use_cosine_filter;
    out->monitor_input_audio = opts->monitor_input_audio;
}

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return snapshots_available ? &snapshot_opts : NULL;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    return snapshots_available ? &snapshot_state : NULL;
}

/* Fail if a label or picker passes the live menu context to an extension reader. */
dsdneoUserDecodeMode
dsd_scan_mode_configured_preset(const dsd_opts* opts, const dsd_state* state) {
    assert(opts == &snapshot_opts);
    assert(state == &snapshot_state);
    return dsd_infer_decode_mode_preset(opts);
}

dsd_scan_mode
dsd_scan_mode_active(const dsd_state* state) {
    assert(state == &snapshot_state || state == NULL);
    return state ? active_mode : DSD_SCAN_MODE_INHERIT;
}

const char*
dsd_scan_mode_name(dsd_scan_mode mode) {
    switch (mode) {
        case DSD_SCAN_MODE_P25: return "p25";
        case DSD_SCAN_MODE_NFM: return "nfm";
        default: return "";
    }
}

static size_t tg_avoid_count;
static uint64_t tg_context;

void
dsd_test_tg_avoids(size_t count, uint64_t context) {
    tg_avoid_count = count;
    tg_context = context;
}

size_t
dsd_tg_policy_session_avoid_count(const dsd_state* state, uint32_t start, uint32_t end) {
    assert(state == &snapshot_state || state == NULL);
    assert(start == 0 && end == UINT32_MAX);
    return state ? tg_avoid_count : 0;
}

void
dsd_tg_policy_table_version(const dsd_state* state, uint64_t* context, unsigned int* generation) {
    assert(state == &snapshot_state || state == NULL);
    if (context) {
        *context = state ? tg_context : 0;
    }
    if (generation) {
        *generation = 0;
    }
}
