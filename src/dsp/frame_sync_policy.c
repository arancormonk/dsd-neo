// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_frame_sync_suppress_p25_alt_sync(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    return opts->p25_trunk == 1 && state->carrier == 1 && DSD_SYNC_IS_P25(state->lastsynctype);
}

int
dsd_frame_sync_sps_hunt_dwell_passes(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 3;
    }
    if (opts->p25_trunk == 1 && opts->p25_is_tuned == 0 && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1)) {
        return 5;
    }
    return 3;
}
