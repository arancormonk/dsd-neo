// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/dmr/dmr.h>
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

/* What retunes the P25 watchdog made since the last frame left, applied before this frame notes anything of the
   carrier it reached. The decoder thread's own retunes, the frame's handler's included (dsd_rxid_request_trunk_tune(),
   while frame_dispatch_active is set), apply theirs where they are made. */
static void
process_frame_apply_off_thread_retunes(dsd_state* state) {
    /* The carrier's access code, and with it the carrier left's RAS evidence. */
    if (dsd_rxid_apply_trunk_tune(state)) {
        dmr_ras_reset(state);
    }
    /* What such a retune left of the DMR calls' decryption (dsd_engine_apply_dmr_leave()): one load while nothing
       waits. */
    if (state->dmr_leave_pending != 0U || state->dmr_leave_request != 0U) {
        (void)dsd_engine_apply_dmr_leave(state);
    }
}

/* Whether a frame of @p synctype can be a call's for the -Y stay's call marker: a ProVoice frame only. An X2-TDMA burst
   is not (one voice sync on noise is taken too; an X2-TDMA call moves the marker as its decoder corroborates the
   superframe and the call store begins it). TETRA has its content check (a TETRA call moves the marker as the call
   store begins it, dsd_call_state_observe()). */
static int
process_frame_may_be_a_call(int synctype) {
    return DSD_SYNC_IS_PROVOICE(synctype);
}

/* The handler's verdict, settled once the frame ran on @p carrier_seq, and the -Y stay's call marker (@p call_frame: a
   frame of a protocol with no content check that is a call's when its decoder takes it). */
static void
process_frame_settle_verdict(dsd_state* state, uint32_t carrier_seq, int call_frame) {
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
    /* The -Y stay's call marker (issue #834) moves here only for a ProVoice frame its decoder took, on the carrier it was
       dispatched on and uninterrupted: once a second sync confirmed its transmission (one sync is no call). The other
       protocols with no content check move it as the call store begins their call (process_frame_may_be_a_call()). */
    if (call_frame
        && (state->sps_hunt_last_frame_verdict == DSD_FRAME_VERDICT_PRODUCTIVE
            || state->sps_hunt_last_frame_verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN)) {
        state->scan_y_call_seq++;
    }
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

    process_frame_apply_off_thread_retunes(state);
    const dsd_protocol_handler* handler = dsd_find_protocol_handler(state->synctype);
    if (handler != NULL && handler->handle_frame != NULL) {
        /* A frame whose protocol has no content check the -Y step could read (issue #834): one store, whatever the
           settings, so a real call of it keeps its -Y row while its frames come, its first candidate included. Not
           TETRA: its bursts report the content their CRCs checked themselves (tetra_burst_proof(), issue #890). */
        const int unchecked = DSD_SYNC_IS_X2TDMA(state->synctype) || DSD_SYNC_IS_PROVOICE(state->synctype);
        if (unchecked) {
            state->scan_y_unchecked_seq++;
        }
        /* A frame of another protocol family replaces the identity the last one left, also when the new protocol
           sends none: what this frame's decoder notes is the carrier's from here. Behind a P25p1 word accepted within a
           tolerance, which may be no word at all, the P25p1 dispatcher notes it once the frame's NID passes. */
        if (!DSD_SYNC_IS_P25P1(state->synctype)
            || dsd_frame_sync_p25p1_sync_kind(state, NULL, NULL) == DSD_P25P1_SYNC_EXACT) {
            dsd_rxid_note_sync(state, state->synctype);
        }
        const uint32_t carrier_seq = state->carrier_seq;
        const int synctype = state->synctype;
        /* The frame's protocol and carrier, for the decoders' MBE capture: a decoder that borrows another protocol's
           vocoder changes state->synctype, not this, and a frame read across a carrier boundary is captured nowhere
           (issue #661). */
        state->frame_dispatch_synctype = state->synctype;
        state->frame_dispatch_carrier_seq = carrier_seq;
        /* And the scan visit the read began on (issue #890): what the frame reports is that visit's, or none's. */
        state->frame_dispatch_scan_visit = state->scan_landing_visit;
        state->frame_dispatch_active = 1;
        state->sps_hunt_last_frame_verdict = (int)handler->handle_frame(opts, state);
        state->frame_dispatch_active = 0;
        process_frame_settle_verdict(state, carrier_seq, unchecked && process_frame_may_be_a_call(synctype));
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
