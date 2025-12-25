// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 2 audio jitter ring helpers.
 *
 * Provides small inline helpers for managing the per-slot fixed-size audio
 * jitter buffer stored in `dsd_state`.
 */

#pragma once

#include <dsd-neo/core/state.h>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset Phase 2 audio jitter ring for one or both slots.
 *
 * @param state Decoder state containing jitter rings.
 * @param slot  Slot index (0/1) or negative to reset both.
 */
static inline void
p25_p2_audio_ring_reset(dsd_state* state, int slot) {
    if (!state) {
        return;
    }

    if (slot < 0 || slot > 1) {
        for (int s = 0; s < 2; s++) {
            state->p25_p2_audio_ring_head[s] = 0;
            state->p25_p2_audio_ring_tail[s] = 0;
            state->p25_p2_audio_ring_count[s] = 0;
        }
        memset(state->p25_p2_audio_ring, 0, sizeof state->p25_p2_audio_ring);
        return;
    }

    state->p25_p2_audio_ring_head[slot] = 0;
    state->p25_p2_audio_ring_tail[slot] = 0;
    state->p25_p2_audio_ring_count[slot] = 0;
    memset(state->p25_p2_audio_ring[slot], 0, sizeof state->p25_p2_audio_ring[slot]);
}

/**
 * @brief Push one 160-sample float frame into the Phase 2 jitter ring.
 *
 * Drops the oldest frame when the ring is full to keep latency bounded.
 *
 * @return 1 on success, 0 on invalid input.
 */
static inline int
p25_p2_audio_ring_push(dsd_state* state, int slot, const float* frame160) {
    if (!state || !frame160 || slot < 0 || slot > 1) {
        return 0;
    }

    if (state->p25_p2_audio_ring_count[slot] >= 3) {
        state->p25_p2_audio_ring_head[slot] = (state->p25_p2_audio_ring_head[slot] + 1) % 3;
        state->p25_p2_audio_ring_count[slot]--;
    }

    int idx = state->p25_p2_audio_ring_tail[slot];
    memcpy(state->p25_p2_audio_ring[slot][idx], frame160, 160U * sizeof(*frame160));
    state->p25_p2_audio_ring_tail[slot] = (state->p25_p2_audio_ring_tail[slot] + 1) % 3;
    state->p25_p2_audio_ring_count[slot]++;
    return 1;
}

/**
 * @brief Pop one 160-sample float frame from the Phase 2 jitter ring.
 *
 * When empty, fills out160 with zeros and returns 0.
 *
 * @return 1 when a frame was returned; 0 when empty/invalid.
 */
static inline int
p25_p2_audio_ring_pop(dsd_state* state, int slot, float* out160) {
    if (!state || !out160 || slot < 0 || slot > 1) {
        return 0;
    }

    if (state->p25_p2_audio_ring_count[slot] <= 0) {
        memset(out160, 0, 160U * sizeof(*out160));
        return 0;
    }

    int idx = state->p25_p2_audio_ring_head[slot];
    memcpy(out160, state->p25_p2_audio_ring[slot][idx], 160U * sizeof(*out160));
    state->p25_p2_audio_ring_head[slot] = (state->p25_p2_audio_ring_head[slot] + 1) % 3;
    state->p25_p2_audio_ring_count[slot]--;
    return 1;
}

#ifdef __cplusplus
}
#endif
