// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include "dstar_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
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
dstar_confirm_note_evidence(const dsd_opts* opts, dsd_state* state, dstar_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == DSTAR_EVIDENCE_STRONG) {
        state->dstar_confirm_frame_evidence = (uint8_t)DSTAR_EVIDENCE_STRONG;
        state->dstar_confirm_weak_streak = 0;
        state->dstar_confirmed = 1;
        dsd_signal_search_note_evidence(state, "D-STAR", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
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
        dsd_signal_search_note_evidence(state, "D-STAR", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
    }
}

void
dstar_confirm_end_frame(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->dstar_confirm_frame_evidence == 0) {
        state->dstar_confirm_weak_streak = 0;
        return;
    }
    /* Only the superframe's own sync behind it, its streak still short: the quiet check waits for the next one. */
    if (state->dstar_confirm_frame_evidence == (uint8_t)DSTAR_EVIDENCE_WEAK
        && state->dstar_confirm_weak_streak < DSTAR_CONFIRM_WEAK_OBSERVES) {
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PARTIAL, DSTAR_CONFIRM_PARTIAL_HOLD_MS);
    }
}

int
dstar_confirm_is_confirmed(const dsd_state* state) {
    return state && state->dstar_confirmed != 0;
}
