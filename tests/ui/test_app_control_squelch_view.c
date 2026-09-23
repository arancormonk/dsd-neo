// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The shared squelch readout (issue #521): effective value first, "(row; default X)" while a
 * scan row overrides it, and a notice that says so when an edit of the default is shadowed. */

#include <assert.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int
level_is(double level, double db) {
    const double want = dsd_squelch_level_from_sql(db);
    return fabs(level - want) <= 1e-9 * fmax(fabs(level), fabs(want));
}

static void
expect_text(const dsd_app_squelch_view* view, const char* readout, const char* notice) {
    char out[96];
    assert(dsd_app_squelch_view_format(view, out, sizeof out) == 0);
    assert(strcmp(out, readout) == 0);
    assert(dsd_app_squelch_view_edit_notice(view, out, sizeof out) == 0);
    assert(strcmp(out, notice) == 0);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->wav_sample_rate = 48000;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->frame_dmr = 1;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);

    dsd_app_squelch_view view;
    /* No scan scope: one value, no annotation. */
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(!view.row_override && level_is(view.effective_level, -80.0) && level_is(view.configured_level, -80.0));
    expect_text(&view, "-80.0 dB", "Applied: RTL squelch -> -80.0 dB");

    /* A row that inherits is not an override. */
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(!view.row_override);
    expect_text(&view, "-80.0 dB", "Applied: RTL squelch -> -80.0 dB");

    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_SQUELCH;
    row.squelch_db = -60;
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.row_override && level_is(view.effective_level, -60.0) && level_is(view.configured_level, -80.0));
    expect_text(&view, "-60.0 dB (row; default -80.0 dB)",
                "Default squelch -80.0 dB; this channel overrides it (-60.0 dB)");

    /* Suspended for a command, dsd_opts holds the default being edited; the row still shows. */
    assert(dsd_scan_mode_suspend(opts, state));
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-75.0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.row_override && level_is(view.effective_level, -60.0) && level_is(view.configured_level, -75.0));
    expect_text(&view, "-60.0 dB (row; default -75.0 dB)",
                "Default squelch -75.0 dB; this channel overrides it (-60.0 dB)");
    assert(dsd_scan_mode_resume(opts, state) == 0);

    /* Off reads as off on either side of the annotation. */
    row.squelch_db = 0;
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    expect_text(&view, "off (row; default -75.0 dB)", "Default squelch -75.0 dB; this channel overrides it (off)");
    assert(fabs(dsd_app_squelch_db_or_off(view.effective_level)) < 1e-12);
    assert(fabs(dsd_app_squelch_db_or_off(view.configured_level) - (-75.0)) < 1e-9);
    dsd_scan_mode_leave(opts, state);
    opts->rtl_squelch_level = 0.0;
    row.squelch_db = -45;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    expect_text(&view, "-45.0 dB (row; default off)", "Default squelch off; this channel overrides it (-45.0 dB)");

    /* A frontend snapshot pair reads the same as the live state. */
    dsd_state* copy = (dsd_state*)calloc(1, sizeof(*copy));
    assert(copy);
    dsd_scan_mode_copy_snapshot(copy, state);
    dsd_app_squelch_view snap;
    assert(dsd_app_squelch_view_get(opts, copy, &snap) == 0);
    assert(snap.row_override && level_is(snap.effective_level, -45.0) && dsd_squelch_is_off(snap.configured_level));
    dsd_scan_mode_leave(opts, state);

    char out[8];
    assert(dsd_app_squelch_view_get(NULL, state, &view) == -1 && !view.row_override);
    assert(dsd_app_squelch_view_get(opts, state, NULL) == -1);
    assert(dsd_app_squelch_view_format(&view, NULL, 0) == -1);
    assert(dsd_app_squelch_view_format(NULL, out, sizeof out) == -1 && out[0] == '\0');
    assert(dsd_app_squelch_view_edit_notice(NULL, out, sizeof out) == -1 && out[0] == '\0');

    dsd_state_ext_free_all(copy);
    dsd_state_ext_free_all(state);
    free(copy);
    free(state);
    free(opts);
    return 0;
}
