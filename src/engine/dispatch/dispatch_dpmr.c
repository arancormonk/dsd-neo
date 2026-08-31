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
#include <dsd-neo/protocol/dpmr/dpmr.h>
#include <stdint.h>
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
 * How long a passing CCH CRC-7 vouches for the syncs that follow it, in symbols. Two seconds at
 * 2400 symbols/s, which is 12 superframe parts. The same wall-clock span as
 * P25P1_NID_EVIDENCE_WINDOW_SYMBOLS, deliberately not the same constant -- that one holds a
 * 4800-baud profile and this one a 2400-baud profile, and neither should move because the other
 * did. Wide enough to bridge the gaps a fading carrier leaves between CRC-clean parts, narrow
 * enough that a channel which stops proving gives the profile up within a dwell.
 */
#define DPMR_CCH_EVIDENCE_WINDOW_SYMBOLS 4800U

/*
 * One FS2 voice frame, and what it proved.
 *
 * A CCH CRC-7 covers the 41 payload bits behind all six Hamming blocks, so a passing half means
 * the half decoded -- one chance in 128 of happening on noise, against the 44% of noise
 * superframes the old Hamming predicate accepted (#407). That is worth a proof: at 2400 baud
 * nothing but dPMR produces one. A frame that decodes nothing still reports PROVEN while a
 * recent one did, because a sync arriving where dPMR was decoding a moment ago is evidence
 * about the profile even when this frame's bits were lost. Only real passes move the window,
 * so it cannot ratchet.
 */
static dsd_frame_verdict
dpmr_handle_voice_frame(dsd_opts* opts, dsd_state* state) {
    DSD_FPRINTF(stderr, "dPMR Frame Sync 2 ");

    state->nac = 0;

    /* Only once something has decoded: a sync word on its own is not a call (#407). */
    if (dpmr_confirm_is_confirmed(state)) {
        dsd_call_observation observation = {
            .protocol = state->synctype,
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
        };
        if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }
    }

    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");

    if (processdPMRvoice(opts, state) > 0) {
        state->dpmr_cch_evidence = 1;
        state->dpmr_cch_evidence_symbolcnt = state->symbolcnt;
        return DSD_FRAME_VERDICT_PROFILE_PROVEN;
    }
    if (state->dpmr_cch_evidence != 0
        && (uint32_t)(state->symbolcnt - state->dpmr_cch_evidence_symbolcnt) < DPMR_CCH_EVIDENCE_WINDOW_SYMBOLS) {
        return DSD_FRAME_VERDICT_PROFILE_PROVEN;
    }
    return DSD_FRAME_VERDICT_UNPRODUCTIVE;
}

/*
 * FS1/FS3/FS4 consume nothing beyond the sync itself and run no check, so they stay productive:
 * protocol_dispatch.h reserves UNPRODUCTIVE for a check that actually ran and actually failed.
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
        return dpmr_handle_voice_frame(opts, state);
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
