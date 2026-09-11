// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

/* Step 0 contract stub: the reason/phrase table lands with the terminal renderer. */
int
dsd_app_scan_timing_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_scan_timing* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts || !state) {
        return -1;
    }
    (void)now_m;
    return 0;
}
