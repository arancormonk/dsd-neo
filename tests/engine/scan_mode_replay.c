// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** Test-only host: -C parses the real map, row zero prepares the production override,
 * then the real engine replays without scanner retunes (unsupported by I/Q replay). */
#include <assert.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stdio.h>
#include <stdlib.h>

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
        const dsd_scan_mode mode = dsd_channel_mode_get(state, 0);
        assert(mode != DSD_SCAN_MODE_INHERIT);
        dsd_scan_settings prepared;
        assert(dsd_scan_mode_prepare(opts, state, mode, &prepared) == 0);
        assert(dsd_scan_mode_enter(opts, state, mode) == 0);
        assert(dsd_scan_mode_active(state) == mode);
        DSD_FPRINTF(stderr, "Scan override applied: %s (%d symbols/s)\n", dsd_scan_mode_name(mode),
                    dsd_scan_mode_effective_profile(opts, state).symbol_rate_hz);
        rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    }
    dsd_scan_mode_leave(opts, state);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}
