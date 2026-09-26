// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** Test-only host: -C parses the real map, row zero prepares the production override and
 * installs the row's own options, then the real engine replays without scanner retunes
 * (unsupported by I/Q replay).
 *
 * DSD_NEO_SCAN_REPLAY_DEFAULT_SQL_DB (whole dB, rtl_sql convention) sets the configured squelch
 * default before the row is entered, which I/Q replay otherwise has no way to take. */
#include <assert.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdio.h>
#include <stdlib.h>

static void
apply_default_squelch(dsd_opts* opts) {
    const char* text = getenv("DSD_NEO_SCAN_REPLAY_DEFAULT_SQL_DB");
    int db = 0;
    if (text && dsd_parse_int_strict(text, 10, -100, 0, &db) == 0) {
        opts->rtl_squelch_level = dsd_squelch_level_from_sql((double)db);
    }
}

/* Row zero's own options, as the conventional scanner installs them on commit (issue #521). */
static void
enter_row_zero(dsd_opts* opts, dsd_state* state) {
    const dsd_scan_mode mode = dsd_channel_mode_get(state, 0);
    assert(mode != DSD_SCAN_MODE_INHERIT);
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, 0);
    dsd_scan_settings prepared;
    assert(dsd_scan_mode_prepare(opts, state, mode, profile ? &profile->values : NULL, &prepared) == 0);
    assert(dsd_scan_mode_enter(opts, state, mode) == 0);
    assert(dsd_scan_mode_active(state) == mode);
    assert(dsd_scan_mode_options(opts, state, profile ? &profile->values : NULL) == 0);
    char sql[24];
    (void)dsd_squelch_format(opts->rtl_squelch_level, " dB", sql, sizeof sql);
    DSD_FPRINTF(stderr, "Scan override applied: %s (%d symbols/s); squelch %s%s\n", dsd_scan_mode_name(mode),
                dsd_scan_mode_effective_profile(opts, state).symbol_rate_hz, sql,
                (dsd_scan_mode_option_fields(state) & DSD_SCAN_OPT_SQUELCH) ? " (row)" : "");
}

int
main(int argc, char** argv) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    initOpts(opts);
    initState(state);
    int rc = 1;
    if (dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &rc) == DSD_BOOTSTRAP_CONTINUE) {
        assert(opts->scanner_mode == 0 && opts->trunk_scan_enabled == 0);
        assert(state->lcn_freq_count > 0);
        apply_default_squelch(opts);
        enter_row_zero(opts, state);
        rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    }
    dsd_scan_mode_leave(opts, state);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}
