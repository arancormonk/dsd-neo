// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/dpmr/dpmr.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

// `reason` distinguishes what the frame that ends the call actually said: a checked end frame
// is positive over-the-air evidence the transmission ended (DSD_CALL_END_TERMINATOR), so the
// event layer can keep an audible epoch whose header never decoded, while a new header
// preempting a still-open call says nothing about the previous transmission and stays EXPLICIT.
// The end is offered even when the slot's call has already ended: an end frame decoding inside the
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

    /* Nothing is published before the frame's CCH is read (issue #890, B19): the frame publishes what its checked half
       says, a data CCH as a data call (dpmr_publish_call()). */

    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " VOICE        ");

    if (processdPMRvoice(opts, state) > 0) {
        state->dpmr_cch_evidence = 1;
        state->dpmr_cch_evidence_symbolcnt = state->symbolcnt;
        /* The same pass, told to the matchers on the profile a hunt step lands on: dPMR is the
         * other 2400/4 tenant, and a live dPMR transmission is offered to the weaker 4800/4
         * matchers exactly as an NXDN48 one is (#445). */
        dsd_frame_sync_note_profile_proof(state);
        return DSD_FRAME_VERDICT_PROFILE_PROVEN;
    }
    if (state->dpmr_cch_evidence != 0
        && (uint32_t)(state->symbolcnt - state->dpmr_cch_evidence_symbolcnt) < DPMR_CCH_EVIDENCE_WINDOW_SYMBOLS) {
        return DSD_FRAME_VERDICT_PROFILE_PROVEN;
    }
    return DSD_FRAME_VERDICT_UNPRODUCTIVE;
}

/* The frame sync accepts FS1 and FS3 only behind a header or end frame that passed its check, and the hunt read it
 * (src/dsp/frame_sync_dpmr.c), so the handler reads nothing, and what proves the profile is that check. */
static dsd_frame_verdict
dpmr_checked_frame_verdict(dsd_state* state) {
    state->dpmr_cch_evidence = 1;
    state->dpmr_cch_evidence_symbolcnt = state->symbolcnt;
    dsd_frame_sync_note_profile_proof(state);
    return DSD_FRAME_VERDICT_PROFILE_PROVEN;
}

/* A frame sync whose frame checked nothing (FS4, whose packet data header this decoder does not read, or an FS1/FS3
 * synctype no checked frame stands behind) changes nothing: protocol_dispatch.h reserves UNPRODUCTIVE for a check that
 * ran and failed, and to a scan quiet check it is a frame waiting for its first verdict (issue #834). */
static dsd_frame_verdict
dpmr_unchecked_frame(const dsd_opts* opts, dsd_state* state, const char* label) {
    DSD_FPRINTF(stderr, "%s\n", label);
    dpmr_confirm_note_unchecked_frame(opts, state);
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}

/* A checked end frame is a proven end of the transmission: it ends the call as a terminator, and its confirmation goes
 * with it (dpmr_confirm_reset(), which forgets the scrambling too): a false FS2 on the carrier's tail must prove itself
 * again before it can open a row. */
static void
dpmr_handle_checked_end(dsd_opts* opts, dsd_state* state, const dpmr_end_frame* end) {
    DSD_FPRINTF(stderr, "dPMR Frame Sync 3 ");
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " END          ");
    dpmr_process_end(opts, state, end);
    dpmr_end_call(opts, state, DSD_CALL_END_TERMINATOR);
    dpmr_confirm_reset(state);
    dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_DPMR_FS1_POS);
}

/* FS1: a header (ETSI TS 102 658 V2.6.1 5.2). Its CRC and its message type come before every side effect (issue #890,
 * A6): only a Communications_Start header for voice or data begins a transmission, so only it ends the call before it,
 * forgets that call's scrambling and closes its capture; and not when it repeats the header that opened the open call
 * in the same key-up (the powersave headers before the normal one, dpmr_header_continues_call()). */
static dsd_frame_verdict
dpmr_handle_header_frame(dsd_opts* opts, dsd_state* state) {
    dpmr_header header;
    if (!dpmr_take_checked_header(state, &header)) {
        return dpmr_unchecked_frame(opts, state, "dPMR Frame Sync 1 (unchecked)");
    }
    DSD_FPRINTF(stderr, "dPMR Frame Sync 1 ");
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " HEADER       ");
    const int continues = dpmr_header_continues_call(state, &header);
    if (dpmr_header_opens_call(&header) && !continues) {
        dpmr_end_call(opts, state, DSD_CALL_END_EXPLICIT);
        dpmr_scrambling_reset(state);
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_DPMR_FS1_POS);
    }
    dpmr_process_header(opts, state, &header, continues);
    /* An END the frame sync attached (the one that decided a held header) is handled now, in the same dispatch, before
       any more input is read: an input that ends at that terminator still ends the call with it. */
    dpmr_end_frame end;
    if (dpmr_take_checked_end_after_header(state, &end)) {
        dpmr_handle_checked_end(opts, state, &end);
    }
    return dpmr_checked_frame_verdict(state);
}

/* FS3: an end frame (5.3). Only one whose END passed its CRC-7 ends the call, as a terminator; an unchecked FS3 leaves
 * the call as it is. */
static dsd_frame_verdict
dpmr_handle_end_frame(dsd_opts* opts, dsd_state* state) {
    dpmr_end_frame end;
    if (!dpmr_take_checked_end(state, &end)) {
        return dpmr_unchecked_frame(opts, state, "dPMR Frame Sync 3 (unchecked)");
    }
    dpmr_handle_checked_end(opts, state, &end);
    return dpmr_checked_frame_verdict(state);
}

dsd_frame_verdict
dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state) {
    /* FDMA voice is slot 0. The shared vocoder, record and audio paths read
     * currentslot, which a DMR, P25 Phase 2 or X2-TDMA decode may have left at 1. */
    state->currentslot = 0;

    if ((state->synctype == DSD_SYNC_DPMR_FS1_POS) || (state->synctype == DSD_SYNC_DPMR_FS1_NEG)) {
        return dpmr_handle_header_frame(opts, state);
    }
    if ((state->synctype == DSD_SYNC_DPMR_FS2_POS) || (state->synctype == DSD_SYNC_DPMR_FS2_NEG)) {
        return dpmr_handle_voice_frame(opts, state);
    }
    if ((state->synctype == DSD_SYNC_DPMR_FS3_POS) || (state->synctype == DSD_SYNC_DPMR_FS3_NEG)) {
        return dpmr_handle_end_frame(opts, state);
    }
    return dpmr_unchecked_frame(opts, state, "dPMR Frame Sync 4 ");
}
