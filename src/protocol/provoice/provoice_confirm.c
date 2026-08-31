// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "provoice_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

void
provoice_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->provoice_confirmed = 0;
    state->provoice_confirm_weak_streak = 0;
    state->provoice_confirm_frame_evidence = 0;
}

void
provoice_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->provoice_confirm_frame_evidence = 0;
}

void
provoice_confirm_note_evidence(dsd_state* state, provoice_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == PROVOICE_EVIDENCE_STRONG) {
        state->provoice_confirm_frame_evidence = (uint8_t)PROVOICE_EVIDENCE_STRONG;
        state->provoice_confirm_weak_streak = 0;
        state->provoice_confirmed = 1;
        return;
    }
    if (evidence != PROVOICE_EVIDENCE_WEAK || state->provoice_confirm_frame_evidence != 0) {
        /* One frame is one frame's worth of evidence however often it is reported. */
        return;
    }
    state->provoice_confirm_frame_evidence = (uint8_t)PROVOICE_EVIDENCE_WEAK;
    if (state->provoice_confirm_weak_streak < 0xFFU) {
        state->provoice_confirm_weak_streak++;
    }
    if (state->provoice_confirm_weak_streak >= PROVOICE_CONFIRM_WEAK_OBSERVES) {
        state->provoice_confirmed = 1;
    }
}

void
provoice_confirm_end_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->provoice_confirm_frame_evidence == 0) {
        state->provoice_confirm_weak_streak = 0;
    }
}

int
provoice_confirm_is_confirmed(const dsd_state* state) {
    return state && state->provoice_confirmed != 0;
}
