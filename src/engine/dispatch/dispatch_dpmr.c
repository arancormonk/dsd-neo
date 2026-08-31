// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

// `reason` distinguishes what the sync that ends the call actually said: the FS3 end-frame sync
// is positive over-the-air evidence the transmission ended (DSD_CALL_END_TERMINATOR), so the
// event layer can keep an audible epoch whose header never decoded, while a new header sync
// preempting a still-open call says nothing about the previous transmission and stays EXPLICIT.
// The end is offered even when the slot's call has already ended: an FS3 decoding inside the
// reacquisition window after a sync-loss end must reach dsd_call_state_end_ex()'s retract path
// so the recoverable end tightens to its final reason -- otherwise the audible epoch's row is
// dropped despite positive end evidence, and a second PTT inside the window folds into the
// ended call's row. end_ex() itself no-ops on everything else.
static void
dpmr_end_call(dsd_opts* opts, dsd_state* state, dsd_call_end_reason reason) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || !DSD_SYNC_IS_DPMR(call.protocol)) {
        return;
    }
    if (dsd_call_state_end_ex(state, 0U, 0.0, reason) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

int
dsd_dispatch_matches_dpmr(int synctype) {
    return DSD_SYNC_IS_DPMR(synctype);
}

/*
 * Always productive. FS1/FS3/FS4 consume nothing beyond the sync itself, and the FS2 voice path
 * (processdPMRvoice) has no *sound* verdict to report (#391). Checks do run there and their
 * results reach dsd_state -- dpmr_decode_cch_frames() leaves a CCH CRC-7 in CCHDataCrcOk and a
 * Hamming(12,8) verdict in CCHDataHammingOk -- but neither can carry a verdict: the CRC-7 fails
 * on every superframe of the committed dpmr fixture, the one capture in the tree that decodes,
 * and the Hamming fallback accepts most random data. Reporting either would rotate the hunt off
 * live traffic, which is the risk direction protocol_dispatch.h names. Left productive until
 * dPMR has an integrity check that works; that is #407, and a verdict here follows from it.
 */
dsd_frame_verdict
dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state) {

    //dPMR
    if ((state->synctype == DSD_SYNC_DPMR_FS1_POS) || (state->synctype == DSD_SYNC_DPMR_FS1_NEG)) {
        /* dPMR Frame Sync 1 */
        dpmr_end_call(opts, state, DSD_CALL_END_EXPLICIT);
        DSD_FPRINTF(stderr, "dPMR Frame Sync 1 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else if ((state->synctype == DSD_SYNC_DPMR_FS2_POS) || (state->synctype == DSD_SYNC_DPMR_FS2_NEG)) {
        /* dPMR Frame Sync 2 */
        DSD_FPRINTF(stderr, "dPMR Frame Sync 2 ");

        state->nac = 0;

        dsd_call_observation observation = {
            .protocol = state->synctype,
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
        };
        if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }

        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");
        processdPMRvoice(opts, state);

        return DSD_FRAME_VERDICT_PRODUCTIVE;

    } else if ((state->synctype == DSD_SYNC_DPMR_FS3_POS) || (state->synctype == DSD_SYNC_DPMR_FS3_NEG)) {
        /* dPMR Frame Sync 3 */
        dpmr_end_call(opts, state, DSD_CALL_END_TERMINATOR);
        DSD_FPRINTF(stderr, "dPMR Frame Sync 3 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else if ((state->synctype == DSD_SYNC_DPMR_FS4_POS) || (state->synctype == DSD_SYNC_DPMR_FS4_NEG)) {
        /* dPMR Frame Sync 4 */
        dpmr_end_call(opts, state, DSD_CALL_END_EXPLICIT);
        DSD_FPRINTF(stderr, "dPMR Frame Sync 4 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    }
    //dPMR
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}
