// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A P25 tick run inside the frame-sync hunt, on the decoder thread: a watchdog beat's (at the hunt's checkpoint, and in
 * a hunt read that waits on its transport) or the decoder's own once a second (getFrameSync()'s entry). A tick that
 * moved the chain under the hunt (a release to the control channel, a hunt step or a CQPSK retry: the modulation, the
 * symbol profile, or a tune that may have moved the receiver) leaves nothing of the window or the symbol in hand worth
 * matching, and the engine took the tuning generation, and the hunt its decode time, before the tune: the hunt hands
 * back at once with no sync and no no-carrier pass, as a read that leaves for a queued command does, and the engine's
 * next call starts on the new carrier. A tune request the receiver refused or deferred before it moved changes
 * nothing (dsd_trunk_tuning_retuned_since()). Apart from dsd_symbol.c and dsd_frame_sync.c, which tests link one
 * without the other. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

void
frame_sync_chain_mark_take(frame_sync_chain_mark* mark, const dsd_state* state) {
    mark->rf_mod = state->rf_mod;
    mark->sps_hunt_idx = state->sps_hunt_idx;
    mark->samples_per_symbol = state->samplesPerSymbol;
    mark->last_request = dsd_trunk_tuning_last_request();
}

int
frame_sync_chain_moved(const frame_sync_chain_mark* mark, const dsd_state* state) {
    return state->rf_mod != mark->rf_mod || state->sps_hunt_idx != mark->sps_hunt_idx
           || state->samplesPerSymbol != mark->samples_per_symbol || dsd_trunk_tuning_retuned_since(mark->last_request);
}

int
frame_sync_service_p25_watchdog(dsd_opts* opts, dsd_state* state) {
    frame_sync_chain_mark mark;
    frame_sync_chain_mark_take(&mark, state);
    return dsd_frame_sync_hook_p25_sm_watchdog_service(opts, state) && frame_sync_chain_moved(&mark, state);
}
