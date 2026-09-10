// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

static const dsd_protocol_handler*
dsd_find_protocol_handler(int synctype) {
    const dsd_protocol_handler* handler = dsd_protocol_handlers;

    while (handler->name != NULL) {
        if (handler->matches_synctype != NULL && handler->matches_synctype(synctype)) {
            return handler;
        }
        handler++;
    }

    return NULL;
}

void
processFrame(dsd_opts* opts, dsd_state* state) {

    if (state->rf_mod == 1) {
        state->maxref = state->max * 0.80F;
        state->minref = state->min * 0.80F;
    } else {
        state->maxref = state->max;
        state->minref = state->min;
    }

    /* Default-productive, and set before the call so an early return inside a handler
     * cannot leave the previous frame's verdict standing. A synctype no handler claims
     * consumed nothing, so crediting it changes nothing either way. */
    state->sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_PRODUCTIVE;

    const dsd_protocol_handler* handler = dsd_find_protocol_handler(state->synctype);
    if (handler != NULL && handler->handle_frame != NULL) {
        const int sync = state->synctype;
        const uint64_t generation = dsd_trunk_tuning_generation();
        state->sps_hunt_last_frame_verdict = (int)handler->handle_frame(opts, state);
        /* These handlers report success only after payload validation. DMR and
         * P25 publish ownership at their validated control/grant boundaries. */
        if ((DSD_SYNC_IS_NXDN(sync) || DSD_SYNC_IS_EDACS(sync))
            && state->sps_hunt_last_frame_verdict == DSD_FRAME_VERDICT_PRODUCTIVE
            && dsd_trunk_tuning_frame_is_current(generation)) {
            dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_OTHER);
        }
    }
}

const dsd_protocol_handler dsd_protocol_handlers[] = {
    {"NXDN", dsd_dispatch_matches_nxdn, dsd_dispatch_handle_nxdn, NULL},
    {"D-STAR", dsd_dispatch_matches_dstar, dsd_dispatch_handle_dstar, NULL},
    {"DMR", dsd_dispatch_matches_dmr, dsd_dispatch_handle_dmr, NULL},
    {"X2-TDMA", dsd_dispatch_matches_x2tdma, dsd_dispatch_handle_x2tdma, NULL},
    {"ProVoice", dsd_dispatch_matches_provoice, dsd_dispatch_handle_provoice, NULL},
    {"EDACS", dsd_dispatch_matches_edacs, dsd_dispatch_handle_edacs, NULL},
    {"YSF", dsd_dispatch_matches_ysf, dsd_dispatch_handle_ysf, NULL},
    {"M17", dsd_dispatch_matches_m17, dsd_dispatch_handle_m17, NULL},
    {"P25P2", dsd_dispatch_matches_p25p2, dsd_dispatch_handle_p25p2, NULL},
    {"dPMR", dsd_dispatch_matches_dpmr, dsd_dispatch_handle_dpmr, NULL},
    {"P25P1", dsd_dispatch_matches_p25p1, dsd_dispatch_handle_p25p1, NULL},
    {0, 0, 0, 0},
};
