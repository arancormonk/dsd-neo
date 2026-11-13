// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25 Phase 2 sync anchor (dsd_p25p2_sync_anchor, <dsd-neo/dsp/p25p2_sync_anchor.h>): its sync-time context, its
 * installation, the position the hunt scores a tolerant word at, and the distance it scores. The matcher that accepts a
 * tolerant word is frame_sync_try_tolerant_p25p2() in dsd_frame_sync.c; the verdict on the window behind it is
 * processP2()'s. processP2() calls in here without the frame sync, so nothing here reads the off-thread tune notices:
 * the frame sync checks them where it captures and scores, and a tune the receiver made while a window was read moves
 * the tuning generation or processP2()'s retune token. No off-thread tune can slip past that: the engine runs
 * processFrame(), so processP2(), under the P25 SM tick guard the watchdog takes for its tunes, so none lands while a
 * window is read; and it dispatches a frame only while dsd_trunk_tuning_frame_is_current() finds no tune request
 * unresolved and the generation unmoved since the hunt began, so one between the sync and the dispatch withholds the
 * window. Linking the frame sync's notices into processP2() would add nothing. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p2_sync_anchor.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "frame_sync_internal.h"

enum {
    P25P2_SYNC_DIBITS = 20,
    /* The four timeslots processP2() reads after a sync, less the sync the fourth one's ISCH is. */
    P25P2_WINDOW_DIBITS = 700,
    P25P2_SUPERFRAME_TIMESLOTS = 12,
};

int
dsd_p25p2_sync_bit_distance(const char* window, uint8_t map_idx, int inverted) {
    return frame_sync_p25_sync_bit_distance(window, inverted ? INV_P25P2_SYNC : P25P2_SYNC, P25P2_SYNC_DIBITS, map_idx);
}

void
frame_sync_p25p2_anchor_note_sync(dsd_state* state, int synctype, int tolerant_start, int bit_errors,
                                  int tune_pending) {
    dsd_p25p2_sync_anchor* rec = &state->p25_p2_sync_anchor;
    /* Whether the sync was tolerant stands whatever becomes of the context: such a window dispatches only past its
       verdict, which nothing may skip. */
    rec->frame_tolerant = tolerant_start >= 0 ? 1U : 0U;
    rec->frame_start = tolerant_start >= 0 ? (uint8_t)tolerant_start : 0U;
    rec->sync_bit_errors = (uint8_t)bit_errors;
    rec->frame_valid = 0U;
    /* The window that matched ends on the latest symbol. */
    if (state->symbol_clock < (uint64_t)P25P2_SYNC_DIBITS || tune_pending) {
        return;
    }
    frame_sync_p25_sync_context_now(state, state->symbol_clock - (uint64_t)P25P2_SYNC_DIBITS, &rec->frame);
    rec->frame.inverted = synctype == DSD_SYNC_P25P2_NEG ? 1U : 0U;
    rec->frame_valid = 1U;
}

void
frame_sync_p25p2_anchor_note_accept(dsd_state* state, int synctype) {
    /* The window the anchor scores is the next sync's: any sync accepted before it reached that position says the stream
       is not the one the anchor measured, and one accepted at or after it is that window's. */
    state->p25_p2_sync_anchor.valid = 0U;
    if (!DSD_SYNC_IS_P25P2(synctype)) {
        state->p25_p2_sync_anchor.frame_valid = 0U;
        state->p25_p2_sync_anchor.frame_tolerant = 0U;
    }
}

int
frame_sync_p25p2_anchor_take(dsd_state* state, uint8_t* out_map_idx, int* out_inverted, int* out_start) {
    dsd_p25p2_sync_anchor* rec = &state->p25_p2_sync_anchor;
    /* The cheap gate first: an integer compare a window. */
    if (!rec->valid || state->symbol_clock < rec->expected_clock + (uint64_t)P25P2_SYNC_DIBITS) {
        return 0;
    }
    /* The hunt has reached the expected position (or passed it unscored): one window per anchor is scored, so the
       anchor is spent either way, and the rest of the hunt is exact. */
    rec->valid = 0U;
    if (state->symbol_clock != rec->expected_clock + (uint64_t)P25P2_SYNC_DIBITS
        || state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_6000_4
        || !frame_sync_p25_sync_context_holds(state, &rec->anchor)) {
        return 0;
    }
    *out_map_idx = rec->anchor.map_idx;
    *out_inverted = rec->anchor.inverted;
    /* On a continuous carrier the next window starts four timeslots on. */
    *out_start = (rec->start + 4) % P25P2_SUPERFRAME_TIMESLOTS;
    return 1;
}

int
dsd_frame_sync_p25p2_tolerant_start(const dsd_state* state, unsigned int* out_bit_errors) {
    const dsd_p25p2_sync_anchor* rec = &state->p25_p2_sync_anchor;
    if (out_bit_errors != NULL) {
        *out_bit_errors = rec->sync_bit_errors;
    }
    return rec->frame_tolerant ? (int)rec->frame_start : -1;
}

int
dsd_frame_sync_p25p2_window_context_holds(const dsd_state* state) {
    const dsd_p25p2_sync_anchor* rec = &state->p25_p2_sync_anchor;
    return rec->frame_valid && frame_sync_p25_sync_context_holds(state, &rec->frame);
}

void
dsd_frame_sync_p25p2_window_decoded(dsd_state* state, int dispatched, int anchor_start) {
    dsd_p25p2_sync_anchor* rec = &state->p25_p2_sync_anchor;
    const int captured = rec->frame_valid != 0U;
    if (rec->frame_tolerant && dispatched) {
        rec->tolerant_windows++;
    }
    rec->frame_valid = 0U;
    rec->frame_tolerant = 0U;
    /* A window that did not dispatch anchors nothing; one opened by a tolerant sync whose verdict failed leaves no anchor
       standing either. */
    if (!dispatched) {
        rec->valid = 0U;
        return;
    }
    /* Only a window whose sync is an S-ISCH by position (it starts at timeslot 2 or 3 of a group of four) has its next
       window's sync 20 dibits after it. */
    if (!captured || anchor_start < 0 || anchor_start >= P25P2_SUPERFRAME_TIMESLOTS || (anchor_start & 3) < 2) {
        return;
    }
    /* The sync-time context must still hold: a tune the window's bursts made, a flush, a profile step since its sync all
       move the stream off the position measured. So does a decoder that read past the window's end, after which the
       hunt starts beyond the expected position. */
    const uint64_t end = rec->frame.sync_clock + (uint64_t)(P25P2_SYNC_DIBITS + P25P2_WINDOW_DIBITS);
    if (!frame_sync_p25_sync_context_holds(state, &rec->frame) || state->symbol_clock > end) {
        return;
    }
    rec->anchor = rec->frame;
    rec->expected_clock = end;
    rec->start = (uint8_t)anchor_start;
    rec->valid = 1U;
}

void
dsd_frame_sync_p25p2_note_sync_activity(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    const int voice_tuned = (opts && opts->trunk_enable == 1 && (opts->trunk_is_tuned == 1)) ? 1 : 0;

    /*
     * Exact P25P2 sync means the channel is present, but while following a VC it
     * does not necessarily mean voice is still active. TDMA VCs can keep sending
     * LCCH/idle after a call ends; refreshing last_vc_sync_time here holds the
     * trunk release path open and delays return to the CC. Voice/MAC handlers
     * update last_vc_sync_time when the call is actually active.
    */
    if (voice_tuned) {
        dsd_frame_sync_hook_p25_sm_vc_sync(opts, state);
        return;
    }

    const time_t now = dsd_decode_time();
    const double nowm = dsd_decode_now_mono_s();
    state->last_cc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
}
