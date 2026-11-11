// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25 Phase 1 sync anchor (dsd_p25p1_sync_anchor, <dsd-neo/dsp/p25p1_sync_anchor.h>): its sync-time context, its
 * installation, the position the hunt scores a tolerant word at, and the distance it scores. The matcher that accepts
 * a tolerant word is frame_sync_try_p25p1() in dsd_frame_sync.c. */

#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

enum {
    P25P1_SYNC_DIBITS = 24,
};

int
dsd_p25p1_sync_bit_distance(const char* window, uint8_t map_idx, int inverted) {
    const char* pattern = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
    if (window == NULL) {
        return 2 * P25P1_SYNC_DIBITS;
    }
    int bits = 0;
    for (int i = 0; i < P25P1_SYNC_DIBITS; i++) {
        const char c = window[i];
        if (c == '\0') {
            return 2 * P25P1_SYNC_DIBITS;
        }
        if (c < '0' || c > '3') {
            bits += 2;
            continue;
        }
        const unsigned int got = dsd_p25_cqpsk_correct_dibit(map_idx, (uint8_t)(c - '0'));
        const unsigned int diff = got ^ (unsigned int)(pattern[i] - '0');
        bits += (int)((diff & 1U) + ((diff >> 1) & 1U));
    }
    return bits;
}

/* The stream and receiver setup as they stand, for a sync word whose first dibit is @p sync_clock. */
static void
p25p1_anchor_context_now(const dsd_state* state, uint64_t sync_clock, dsd_p25p1_sync_context* out) {
    out->sync_clock = sync_clock;
    out->tune_generation = dsd_trunk_tuning_generation();
    out->symbol_clock_epoch = state->symbol_clock_epoch;
    out->symbol_clock_input = state->symbol_clock_input;
    out->carrier_seq = state->carrier_seq;
    out->sync_history_drops = state->frame_sync_carry.drops;
    out->sps_hunt_idx = state->sps_hunt_idx;
    out->rf_mod = state->rf_mod;
    out->map_idx = state->p25_cqpsk_dibit_map_idx;
    out->inverted = 0U;
}

/* Whether @p ctx still describes the stream: nothing that ends the history carry has happened since
   (dsd_frame_sync_history_carry), nor a profile or modulation change, and no off-thread tune awaits reconciling. */
static int
p25p1_anchor_context_current(const dsd_state* state, const dsd_p25p1_sync_context* ctx) {
    return ctx->symbol_clock_epoch == state->symbol_clock_epoch && ctx->symbol_clock_input == state->symbol_clock_input
           && ctx->carrier_seq == state->carrier_seq && ctx->sync_history_drops == state->frame_sync_carry.drops
           && ctx->sps_hunt_idx == state->sps_hunt_idx && ctx->rf_mod == state->rf_mod
           && ctx->tune_generation == dsd_trunk_tuning_generation() && !dsd_frame_sync_off_thread_tune_pending();
}

void
frame_sync_p25p1_anchor_note_sync(dsd_state* state, int synctype, int tolerant_nac, int bit_errors) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    /* Whether the sync was tolerant stands whatever becomes of the context: such a frame decodes only past the strict
       check, which nothing may skip. */
    rec->frame_tolerant = tolerant_nac >= 0 ? 1U : 0U;
    rec->frame_nac = tolerant_nac >= 0 ? (uint16_t)tolerant_nac : 0U;
    rec->sync_bit_errors = (uint8_t)bit_errors;
    rec->frame_valid = 0U;
    /* The window that matched ends on the latest symbol. */
    if (state->symbol_clock < (uint64_t)P25P1_SYNC_DIBITS || dsd_frame_sync_off_thread_tune_pending()) {
        return;
    }
    p25p1_anchor_context_now(state, state->symbol_clock - (uint64_t)P25P1_SYNC_DIBITS, &rec->frame);
    rec->frame.inverted = synctype == DSD_SYNC_P25P1_NEG ? 1U : 0U;
    rec->frame_valid = 1U;
}

void
frame_sync_p25p1_anchor_note_accept(dsd_state* state, int synctype) {
    /* The window the anchor scores is the next data unit's first: any sync accepted before it reached that position
       says the stream is not the one the anchor measured, and one accepted at or after it is that window's. */
    state->p25_p1_sync_anchor.valid = 0U;
    if (!DSD_SYNC_IS_P25P1(synctype)) {
        state->p25_p1_sync_anchor.frame_valid = 0U;
        state->p25_p1_sync_anchor.frame_tolerant = 0U;
    }
}

int
frame_sync_p25p1_anchor_take(dsd_state* state, uint8_t* out_map_idx, int* out_inverted, int* out_nac) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    /* The cheap gate first: an integer compare a window. */
    if (!rec->valid || state->symbol_clock < rec->expected_clock + (uint64_t)P25P1_SYNC_DIBITS) {
        return 0;
    }
    /* The hunt has reached the expected position (or passed it unscored): one window per frame is scored, so the
       anchor is spent either way, and the rest of the hunt is exact. */
    rec->valid = 0U;
    if (state->symbol_clock != rec->expected_clock + (uint64_t)P25P1_SYNC_DIBITS
        || state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4
        || !p25p1_anchor_context_current(state, &rec->anchor)) {
        return 0;
    }
    *out_map_idx = rec->anchor.map_idx;
    *out_inverted = rec->anchor.inverted;
    *out_nac = rec->nac;
    return 1;
}

int
dsd_frame_sync_p25p1_tolerant_nac(const dsd_state* state) {
    const dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    return rec->frame_tolerant ? (int)rec->frame_nac : -1;
}

void
dsd_frame_sync_p25p1_frame_decoded(dsd_state* state, int nac, uint32_t unit_dibits) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    const int captured = rec->frame_valid != 0U;
    rec->frame_valid = 0U;
    rec->frame_tolerant = 0U;
    if (!captured || unit_dibits == 0U || nac <= 0x000 || nac >= 0xFFF) {
        return;
    }
    /* The sync-time context must still hold: a tune the frame's decoder made, a flush, a profile step since its sync
       all move the stream off the position measured. So does a decoder that read past the unit's end, after which the
       hunt starts beyond the expected position. */
    const uint64_t end = rec->frame.sync_clock + (uint64_t)unit_dibits;
    if (!p25p1_anchor_context_current(state, &rec->frame) || state->symbol_clock > end) {
        return;
    }
    rec->anchor = rec->frame;
    rec->expected_clock = end;
    rec->nac = (uint16_t)nac;
    rec->valid = 1U;
}
