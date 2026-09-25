// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>

/* Action-only tests have no tuner; exercise the production scoped-settings teardown. */
void
dsd_engine_channel_scan_leave(dsd_opts* opts, dsd_state* state) {
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_CHANNEL_SCAN, NULL, NULL);
    dsd_scan_mode_leave(opts, state);
}

/* Nor a running scan list: no row waits to run the configured NFM width (issue #526). APP_COMMAND_QUEUE drives the real
   list through the engine. */
int
dsd_engine_scan_runs_configured_nfm_width(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
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
