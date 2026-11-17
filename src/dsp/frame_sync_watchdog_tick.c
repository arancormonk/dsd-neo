// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A P25 watchdog beat's tick run in place inside a frame-sync hunt read that waits on a quiet input (the silent PCM wait,
 * the RTL idle-bounded read): no sample is in flight there. A tick that moved the chain under the hunt (a release to
 * the control channel, a hunt step or a CQPSK retry: the modulation, the symbol profile or a tune) is noted, and the
 * hunt's next checkpoint starts its context over (getFrameSync()). Apart from dsd_symbol.c and dsd_frame_sync.c, which
 * tests link one without the other. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

/* A tick a hunt read ran moved the chain. Decoder thread only. */
static int s_moved_chain = 0;

void
frame_sync_service_p25_watchdog_in_read(dsd_opts* opts, dsd_state* state) {
    const int rf_mod = state->rf_mod;
    const int sps_idx = state->sps_hunt_idx;
    const int sps = state->samplesPerSymbol;
    const uint64_t last_request = dsd_trunk_tuning_last_request();
    if (dsd_frame_sync_hook_p25_sm_watchdog_service(opts, state)
        && (state->rf_mod != rf_mod || state->sps_hunt_idx != sps_idx || state->samplesPerSymbol != sps
            || dsd_trunk_tuning_last_request() != last_request)) {
        s_moved_chain = 1;
    }
}

int
frame_sync_take_p25_watchdog_moved_chain(void) {
    const int moved = s_moved_chain;
    s_moved_chain = 0;
    return moved;
}
