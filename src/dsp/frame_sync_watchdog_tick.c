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
 * nothing (dsd_trunk_tuning_retuned_since()). One still in flight (an RTL retune the stream applies on its own thread)
 * is noted by the tick itself (p25_sm_tick_tunes_in_flight(), p25_sm_watchdog.c): the engine starts no hunt until it
 * lands, so the next takes the generation at the actual tune boundary. A beat standing behind queued work (shutdown, a
 * command, a put-off publication) moved nothing: a hunt read keeps its partial symbol and gives way to the work where
 * it would with no beat (a stream pause, the RTL idle bound), and the hunt's checkpoint goes on with its window, the
 * work waiting for the hunt's return as at any checkpoint; a TCP reconnect, whose stream already ended, gives up for
 * it. The tick is left for the engine loop. Apart from dsd_symbol.c
 * and dsd_frame_sync.c, which tests link one without the other. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

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
    const int serviced = dsd_frame_sync_hook_p25_sm_watchdog_service(opts, state);
    if (serviced < 0) {
        return FRAME_SYNC_WD_DEFERRED;
    }
    return serviced > 0 && frame_sync_chain_moved(&mark, state) ? FRAME_SYNC_WD_MOVED : FRAME_SYNC_WD_NONE;
}
