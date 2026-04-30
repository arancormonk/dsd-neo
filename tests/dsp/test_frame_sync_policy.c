// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static void
reset(dsd_opts* opts, dsd_state* state) {
    memset(opts, 0, sizeof(*opts));
    memset(state, 0, sizeof(*state));
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.carrier = 1;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 0);

    opts.p25_trunk = 1;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 1);

    state.carrier = 0;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 0);

    state.carrier = 1;
    state.lastsynctype = DSD_SYNC_YSF_POS;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 0);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);

    opts.p25_trunk = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 5);

    opts.p25_is_tuned = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);

    opts.p25_is_tuned = 0;
    opts.frame_p25p1 = 0;
    opts.frame_p25p2 = 0;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);

    return 0;
}
