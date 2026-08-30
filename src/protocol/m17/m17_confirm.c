// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "m17_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

void
m17_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->m17_confirmed = 0;
    state->m17_confirm_weak_streak = 0;
    state->m17_confirm_frame_evidence = 0;
}

void
m17_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->m17_confirm_frame_evidence = 0;
}

void
m17_confirm_note_evidence(dsd_state* state, m17_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == M17_EVIDENCE_STRONG) {
        state->m17_confirm_frame_evidence = (uint8_t)M17_EVIDENCE_STRONG;
        state->m17_confirm_weak_streak = 0;
        state->m17_confirmed = 1;
        return;
    }
    if (evidence != M17_EVIDENCE_WEAK || state->m17_confirm_frame_evidence != 0) {
        /* Several clean LICH reports in one frame are one frame's worth of evidence. */
        return;
    }
    state->m17_confirm_frame_evidence = (uint8_t)M17_EVIDENCE_WEAK;
    if (state->m17_confirm_weak_streak < 0xFFU) {
        state->m17_confirm_weak_streak++;
    }
    if (state->m17_confirm_weak_streak >= M17_CONFIRM_WEAK_OBSERVES) {
        state->m17_confirmed = 1;
    }
}

void
m17_confirm_end_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->m17_confirm_frame_evidence == 0) {
        state->m17_confirm_weak_streak = 0;
    }
}

int
m17_confirm_is_confirmed(const dsd_state* state) {
    return state && state->m17_confirmed != 0;
}
