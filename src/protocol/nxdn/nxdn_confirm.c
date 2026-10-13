// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include "nxdn_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

void
nxdn_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->nxdn_confirmed = 0;
    state->nxdn_confirm_weak_streak = 0;
    state->nxdn_confirm_frame_evidence = 0;
}

void
nxdn_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->nxdn_confirm_frame_evidence = 0;
}

void
nxdn_confirm_note_evidence(dsd_state* state, nxdn_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == NXDN_EVIDENCE_STRONG) {
        state->nxdn_confirm_frame_evidence = (uint8_t)NXDN_EVIDENCE_STRONG;
        state->nxdn_confirm_weak_streak = 0;
        state->nxdn_confirmed = 1;
        return;
    }
    if (evidence != NXDN_EVIDENCE_WEAK || state->nxdn_confirm_frame_evidence != 0) {
        /* Several short CRCs in one frame are one frame's worth of evidence, not several. */
        return;
    }
    state->nxdn_confirm_frame_evidence = (uint8_t)NXDN_EVIDENCE_WEAK;
    if (state->nxdn_confirm_weak_streak < 0xFFU) {
        state->nxdn_confirm_weak_streak++;
    }
    if (state->nxdn_confirm_weak_streak >= NXDN_CONFIRM_WEAK_OBSERVES) {
        state->nxdn_confirmed = 1;
    }
}

void
nxdn_confirm_end_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->nxdn_confirm_frame_evidence == 0) {
        state->nxdn_confirm_weak_streak = 0;
        return;
    }
    /* The frame's own CRCs confirmed the transmission: a strong one, or the short one that completes or continues a weak
     * streak. A signal search hears that only now, once the frame's channels have published the RAN and call their
     * confirmation unlocked, since it reads them from state when it hears it. */
    if (state->nxdn_confirm_frame_evidence == (uint8_t)NXDN_EVIDENCE_STRONG
        || state->nxdn_confirm_weak_streak >= NXDN_CONFIRM_WEAK_OBSERVES) {
        dsd_signal_search_note_evidence(state, "NXDN", 0, 0, NULL);
    }
}

int
nxdn_confirm_is_confirmed(const dsd_state* state) {
    return state && state->nxdn_confirmed != 0;
}

int
nxdn_confirm_frame_proved(const dsd_state* state) {
    return state && state->nxdn_confirmed != 0 && state->nxdn_confirm_frame_evidence != 0;
}
