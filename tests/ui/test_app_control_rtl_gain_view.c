// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The shared tuner gain readout under --trunk-scan (issue #518 follow-up): the configured gain the controls edit, the
 * parked target's own gain while it overrides it, and notices that say which one an edit reached. */

#include <assert.h>
#include <dsd-neo/app_control/rtl_gain_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdlib.h>
#include <string.h>

static void
expect_texts(const dsd_app_rtl_gain_view* view, int live_on, const char* label, const char* notice,
             const char* autogain_label, const char* autogain_notice) {
    char out[96];
    assert(dsd_app_rtl_gain_view_label(view, out, sizeof out) == 0);
    assert(strcmp(out, label) == 0);
    assert(dsd_app_rtl_gain_view_edit_notice(view, out, sizeof out) == 0);
    assert(strcmp(out, notice) == 0);
    assert(dsd_app_rtl_autogain_view_label(view, live_on, out, sizeof out) == 0);
    assert(strcmp(out, autogain_label) == 0);
    assert(dsd_app_rtl_autogain_view_edit_notice(view, out, sizeof out) == 0);
    assert(strcmp(out, autogain_notice) == 0);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    dsd_app_rtl_gain_view view;

    /* No trunk scan: the gain in force is the configured gain, and autogain reads the stream's. */
    opts->rtl_gain_value = 22;
    state->trunk_scan_configured_gain = 5; /* stale publication without a scan is ignored */
    assert(dsd_app_rtl_gain_view_get(opts, state, &view) == 0);
    assert(view.configured_gain == 22 && view.effective_gain == 22 && !view.row_override && !view.scan_configured);
    expect_texts(&view, 1, "Gain... [22]", "Applied: RTL gain -> 22", "Tuner autogain [On]",
                 "Default tuner autogain -> Off; manual gain 22 suspends it");

    /* Trunk scan parked on a target with its own gain: the configured gain leads, the target's follows. */
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_target_count = 3U;
    state->trunk_scan_configured_gain = 30;
    state->trunk_scan_gain_override = 1U;
    state->trunk_scan_configured_autogain = 1U;
    opts->rtl_gain_value = 10;
    assert(dsd_app_rtl_gain_view_get(opts, state, &view) == 0);
    assert(view.configured_gain == 30 && view.effective_gain == 10 && view.row_override && view.scan_configured);
    expect_texts(&view, 0, "Gain... [30] (target: 10)", "Default RTL gain -> 30; this channel overrides it (10)",
                 "Tuner autogain [On] (suspended: gain 10)",
                 "Default tuner autogain -> On; manual gain 10 suspends it");

    /* An inheriting target under AGC: plain text, and the configured autogain is the one in force. */
    state->trunk_scan_configured_gain = 0;
    state->trunk_scan_gain_override = 0U;
    state->trunk_scan_configured_autogain = 0U;
    opts->rtl_gain_value = 0;
    assert(dsd_app_rtl_gain_view_get(opts, state, &view) == 0);
    expect_texts(&view, 1, "Gain... [AGC]", "Applied: RTL gain -> AGC", "Tuner autogain [Off]",
                 "Applied: tuner autogain -> Off");

    /* A target with its own AGC under a manual configured gain. */
    state->trunk_scan_configured_gain = 18;
    state->trunk_scan_gain_override = 1U;
    assert(dsd_app_rtl_gain_view_get(opts, state, &view) == 0);
    expect_texts(&view, 0, "Gain... [18] (target: AGC)", "Default RTL gain -> 18; this channel overrides it (AGC)",
                 "Tuner autogain [Off]", "Applied: tuner autogain -> Off");

    /* Bad arguments. */
    char out[8];
    assert(dsd_app_rtl_gain_view_get(NULL, state, &view) == -1);
    assert(dsd_app_rtl_gain_view_get(opts, state, NULL) == -1);
    assert(dsd_app_rtl_gain_view_label(&view, out, 0U) == -1);
    assert(dsd_app_rtl_gain_view_edit_notice(NULL, out, sizeof out) == -1);
    assert(dsd_app_rtl_autogain_view_label(&view, 0, NULL, 8U) == -1);
    assert(dsd_app_rtl_autogain_view_edit_notice(&view, NULL, 8U) == -1);

    free(opts);
    free(state);
    return 0;
}
