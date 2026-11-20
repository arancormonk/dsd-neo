// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include "m17_confirm.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
m17_confirm_reset_signature(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(state->m17_signature_digest, 0, sizeof(state->m17_signature_digest));
    DSD_MEMSET(state->m17_signature_fold, 0, sizeof(state->m17_signature_fold));
    DSD_MEMSET(state->m17_signature, 0, sizeof(state->m17_signature));
    state->m17_signature_received_mask = 0U;
    state->m17_signature_complete = 0U;
    state->m17_signature_bad_sequence = 0U;
    state->m17_signature_verification_status = 0U;
    state->m17_signature_frames = 0U;
    state->m17_signature_next_fn = 0U;
    state->m17_signature_window = 0U;
}

void
m17_confirm_reset_link(dsd_state* state) {
    if (!state) {
        return;
    }
    state->m17_lsf_checked = 0U;
    state->m17_signature_advertised = 0U;
    state->m17_ip_stream = DSD_M17_IP_STREAM_NONE;
    state->m17_ip_stream_sid = 0U;
    m17_confirm_reset_signature(state);
}

void
m17_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->m17_confirmed = 0;
    state->m17_confirm_weak_streak = 0;
    state->m17_confirm_frame_evidence = 0;
    m17_confirm_reset_link(state);
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

void
m17_confirm_note_unchecked_frame(const dsd_opts* opts, dsd_state* state) {
    if (!state || state->m17_confirm_frame_evidence != 0) {
        return;
    }
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PARTIAL, M17_CONFIRM_PARTIAL_HOLD_MS);
}

int
m17_confirm_is_confirmed(const dsd_state* state) {
    return state && state->m17_confirmed != 0;
}
