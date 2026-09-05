// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <assert.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>

static dsd_opts snapshot_opts;
static dsd_state snapshot_state;

void
dsd_app_snapshot_configured_mode(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out) {
    assert(opts == &snapshot_opts);
    assert(state == &snapshot_state);
    out->use_cosine_filter = opts->use_cosine_filter;
    out->monitor_input_audio = opts->monitor_input_audio;
}

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return &snapshot_opts;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    return &snapshot_state;
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
    return DSD_SCAN_MODE_INHERIT;
}

const char*
dsd_scan_mode_name(dsd_scan_mode mode) {
    (void)mode;
    return "";
}
