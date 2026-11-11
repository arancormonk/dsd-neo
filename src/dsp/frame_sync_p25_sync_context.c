// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* What the P25 Phase 1 and Phase 2 sync anchors share (dsd_p25p1_sync_anchor, dsd_p25p2_sync_anchor): the sync-time
 * context they measure the stream by, and the distance a sync word is scored by. Nothing here reads the off-thread tune
 * notices (frame_sync_p25p1_compare.c), which only the frame sync consults: processP2() reaches the Phase 2 anchor's
 * window functions without the frame sync. */

#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "frame_sync_internal.h"

int
frame_sync_p25_sync_bit_distance(const char* window, const char* pattern, int dibits, uint8_t map_idx) {
    if (window == NULL) {
        return 2 * dibits;
    }
    int bits = 0;
    for (int i = 0; i < dibits; i++) {
        const char c = window[i];
        if (c == '\0') {
            return 2 * dibits;
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

void
frame_sync_p25_sync_context_now(const dsd_state* state, uint64_t sync_clock, dsd_p25_sync_context* out) {
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

int
frame_sync_p25_sync_context_holds(const dsd_state* state, const dsd_p25_sync_context* ctx) {
    return ctx->symbol_clock_epoch == state->symbol_clock_epoch && ctx->symbol_clock_input == state->symbol_clock_input
           && ctx->carrier_seq == state->carrier_seq && ctx->sync_history_drops == state->frame_sync_carry.drops
           && ctx->sps_hunt_idx == state->sps_hunt_idx && ctx->rf_mod == state->rf_mod
           && ctx->tune_generation == dsd_trunk_tuning_generation();
}
