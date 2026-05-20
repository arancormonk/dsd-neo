// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dmr_confidence.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

#define DMR_CONFIDENCE_UNKNOWN_CC     16U
#define DMR_CONFIDENCE_LOCK_OBSERVES  2U
#define DMR_CONFIDENCE_VOICE_OBSERVES 2U

static int
dmr_confidence_valid_cc(unsigned int color_code) {
    return color_code <= 15U;
}

static void
dmr_confidence_clear_voice(dsd_state* state) {
    memset(state->dmr_confidence_voice_sync_seen, 0, sizeof(state->dmr_confidence_voice_sync_seen));
    memset(state->dmr_confidence_voice_open, 0, sizeof(state->dmr_confidence_voice_open));
    memset(state->dmr_confidence_voice_count, 0, sizeof(state->dmr_confidence_voice_count));
}

void
dmr_confidence_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->dmr_confidence_locked = 0;
    state->dmr_confidence_color_code = DMR_CONFIDENCE_UNKNOWN_CC;
    state->dmr_confidence_candidate_cc = DMR_CONFIDENCE_UNKNOWN_CC;
    state->dmr_confidence_candidate_count = 0;
    state->dmr_confidence_mismatch_count = 0;
    dmr_confidence_clear_voice(state);
}

void
dmr_confidence_reset_slot(dsd_state* state, unsigned int slot) {
    if (!state || slot >= 2U) {
        return;
    }
    state->dmr_confidence_voice_sync_seen[slot] = 0;
    state->dmr_confidence_voice_open[slot] = 0;
    state->dmr_confidence_voice_count[slot] = 0;
}

static dmr_confidence_result
dmr_confidence_observe_cc(dsd_state* state, unsigned int color_code, int may_lock) {
    if (!state || !dmr_confidence_valid_cc(color_code)) {
        return DMR_CONFIDENCE_REJECT;
    }

    if (state->dmr_confidence_locked) {
        if (state->dmr_confidence_color_code == color_code) {
            return DMR_CONFIDENCE_LOCKED;
        }
        if (state->dmr_confidence_mismatch_count < 255U) {
            state->dmr_confidence_mismatch_count++;
        }
        return DMR_CONFIDENCE_REJECT;
    }

    if (state->dmr_confidence_candidate_cc != color_code) {
        state->dmr_confidence_candidate_cc = (uint8_t)color_code;
        state->dmr_confidence_candidate_count = 1;
    } else if (state->dmr_confidence_candidate_count < 255U) {
        state->dmr_confidence_candidate_count++;
    }

    if (may_lock && state->dmr_confidence_candidate_count >= DMR_CONFIDENCE_LOCK_OBSERVES) {
        state->dmr_confidence_locked = 1;
        state->dmr_confidence_color_code = (uint8_t)color_code;
        state->dmr_color_code = color_code;
        return DMR_CONFIDENCE_LOCKED;
    }

    return DMR_CONFIDENCE_PENDING;
}

void
dmr_confidence_note_voice_sync(dsd_state* state, unsigned int slot) {
    if (!state || slot >= 2U) {
        return;
    }
    state->dmr_confidence_voice_sync_seen[slot] = 1;
    if (!state->dmr_confidence_voice_open[slot]) {
        state->dmr_confidence_voice_count[slot] = 0;
    }
}

dmr_confidence_result
dmr_confidence_note_voice_burst(dsd_state* state, unsigned int slot, unsigned int color_code) {
    if (!state || slot >= 2U) {
        return DMR_CONFIDENCE_REJECT;
    }
    if (!state->dmr_confidence_voice_sync_seen[slot] && !state->dmr_confidence_voice_open[slot]) {
        return DMR_CONFIDENCE_PENDING;
    }

    int was_locked = state->dmr_confidence_locked != 0;
    dmr_confidence_result cc_result = dmr_confidence_observe_cc(state, color_code, 1);
    if (cc_result != DMR_CONFIDENCE_LOCKED) {
        return cc_result;
    }

    if (!was_locked && state->dmr_confidence_voice_sync_seen[slot]) {
        state->dmr_confidence_voice_count[slot] = DMR_CONFIDENCE_VOICE_OBSERVES;
    } else if (state->dmr_confidence_voice_count[slot] < 255U) {
        state->dmr_confidence_voice_count[slot]++;
    }

    if (state->dmr_confidence_voice_count[slot] >= DMR_CONFIDENCE_VOICE_OBSERVES) {
        state->dmr_confidence_voice_open[slot] = 1;
        return DMR_CONFIDENCE_LOCKED;
    }

    return DMR_CONFIDENCE_PENDING;
}

dmr_confidence_result
dmr_confidence_note_data_burst(dsd_state* state, unsigned int color_code, unsigned int burst) {
    if (!state || !dmr_confidence_valid_cc(color_code)) {
        return DMR_CONFIDENCE_REJECT;
    }
    if (state->dmr_confidence_locked) {
        return dmr_confidence_observe_cc(state, color_code, 0);
    }
    if (burst == 9U) {
        return dmr_confidence_observe_cc(state, color_code, 1);
    }
    return DMR_CONFIDENCE_PENDING;
}

int
dmr_confidence_voice_slot_open(const dsd_state* state, unsigned int slot) {
    if (!state || slot >= 2U) {
        return 0;
    }
    return state->dmr_confidence_voice_open[slot] != 0;
}

int
dmr_confidence_any_voice_open(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    return state->dmr_confidence_voice_open[0] != 0 || state->dmr_confidence_voice_open[1] != 0;
}
