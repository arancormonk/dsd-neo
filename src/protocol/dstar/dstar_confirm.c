// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dstar_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

void
dstar_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->dstar_confirmed = 0;
    state->dstar_confirm_weak_streak = 0;
    state->dstar_confirm_frame_evidence = 0;
}

void
dstar_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->dstar_confirm_frame_evidence = 0;
}

void
dstar_confirm_note_evidence(dsd_state* state, dstar_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == DSTAR_EVIDENCE_STRONG) {
        state->dstar_confirm_frame_evidence = (uint8_t)DSTAR_EVIDENCE_STRONG;
        state->dstar_confirm_weak_streak = 0;
        state->dstar_confirmed = 1;
        return;
    }
    if (evidence != DSTAR_EVIDENCE_WEAK || state->dstar_confirm_frame_evidence != 0) {
        /* One superframe is one frame's worth of evidence however often it is reported. */
        return;
    }
    state->dstar_confirm_frame_evidence = (uint8_t)DSTAR_EVIDENCE_WEAK;
    if (state->dstar_confirm_weak_streak < 0xFFU) {
        state->dstar_confirm_weak_streak++;
    }
    if (state->dstar_confirm_weak_streak >= DSTAR_CONFIRM_WEAK_OBSERVES) {
        state->dstar_confirmed = 1;
    }
}

void
dstar_confirm_end_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->dstar_confirm_frame_evidence == 0) {
        state->dstar_confirm_weak_streak = 0;
    }
}

int
dstar_confirm_is_confirmed(const dsd_state* state) {
    return state && state->dstar_confirmed != 0;
}
