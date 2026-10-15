// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/protocol_dispatch.h>
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
        const uint32_t carrier_seq = state->carrier_seq;
        /* The frame's protocol and carrier, for the decoders' MBE capture: a decoder that borrows another protocol's
           vocoder changes state->synctype, not this, and a frame read across a carrier boundary is captured nowhere
           (issue #661). */
        state->frame_dispatch_synctype = state->synctype;
        state->frame_dispatch_carrier_seq = carrier_seq;
        state->frame_dispatch_active = 1;
        state->sps_hunt_last_frame_verdict = (int)handler->handle_frame(opts, state);
        state->frame_dispatch_active = 0;
        /* A replay read that adopted a recorded retune ran the carrier boundary inside the frame (issue #575): its
           decoder dropped what it read, as on a sync loss, so the frame validated nothing on either carrier, whatever
           the handler reported for the symbols it read. */
        if (state->carrier_seq != carrier_seq) {
            state->sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_UNPRODUCTIVE;
        } else if (state->input_interrupted) {
            /* An input interruption cut the frame (a Signal search left it, or a live read gave up on a stalled
               input): its decoder dropped it, so whatever the handler reported, a sticky confirmation or a proof, the
               frame validated nothing. Nor did it fail anything: the cut was the engine's, not the signal's, so like a
               frame the engine declined to dispatch it is withheld from the SPS hunt's accounting. */
            state->sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_WITHHELD;
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
    {"TETRA", dsd_dispatch_matches_tetra, dsd_dispatch_handle_tetra, NULL},
    {"P25P1", dsd_dispatch_matches_p25p1, dsd_dispatch_handle_p25p1, NULL},
    {0, 0, 0, 0},
};
