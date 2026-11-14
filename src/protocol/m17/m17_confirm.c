// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
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
m17_confirm_note_evidence(const dsd_opts* opts, dsd_state* state, m17_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == M17_EVIDENCE_STRONG) {
        state->m17_confirm_frame_evidence = (uint8_t)M17_EVIDENCE_STRONG;
        state->m17_confirm_weak_streak = 0;
        state->m17_confirmed = 1;
        dsd_signal_search_note_evidence(state, "M17", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
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
        dsd_signal_search_note_evidence(state, "M17", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
        return;
    }
    /* A weak frame whose streak the next frame must complete: the quiet check waits for that frame. */
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PARTIAL, M17_CONFIRM_PARTIAL_HOLD_MS);
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
