// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dpmr_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

void
dpmr_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->dpmr_confirmed = 0;
    state->dpmr_confirm_weak_streak = 0;
    state->dpmr_confirm_frame_evidence = 0;
}

void
dpmr_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->dpmr_confirm_frame_evidence = 0;
}

void
dpmr_confirm_note_evidence(dsd_state* state, dpmr_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == DPMR_EVIDENCE_STRONG) {
        state->dpmr_confirm_frame_evidence = (uint8_t)DPMR_EVIDENCE_STRONG;
        state->dpmr_confirm_weak_streak = 0;
        state->dpmr_confirmed = 1;
        return;
    }
    if (evidence != DPMR_EVIDENCE_WEAK || state->dpmr_confirm_frame_evidence != 0) {
        /* Both halves of one frame are one frame's worth of evidence, not two. */
        return;
    }
    state->dpmr_confirm_frame_evidence = (uint8_t)DPMR_EVIDENCE_WEAK;
    if (state->dpmr_confirm_weak_streak < 0xFFU) {
        state->dpmr_confirm_weak_streak++;
    }
    if (state->dpmr_confirm_weak_streak >= DPMR_CONFIRM_WEAK_OBSERVES) {
        state->dpmr_confirmed = 1;
    }
}

void
dpmr_confirm_end_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->dpmr_confirm_frame_evidence == 0) {
        state->dpmr_confirm_weak_streak = 0;
    }
}

int
dpmr_confirm_is_confirmed(const dsd_state* state) {
    return state && state->dpmr_confirmed != 0;
}
