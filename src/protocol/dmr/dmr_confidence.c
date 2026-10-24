// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "dmr_confidence.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define DMR_CONFIDENCE_UNKNOWN_CC      16U
#define DMR_CONFIDENCE_LOCK_OBSERVES   2U
#define DMR_CONFIDENCE_VOICE_OBSERVES  2U
#define DMR_CONFIDENCE_RELOCK_OBSERVES 4U

/* DMR content passed the gate: signal search's evidence, and the stamp a startup control-channel hunt reads on the
   decoder thread (issue #703). Never cleared: a carrier boundary does not undo content that was decoded. The carrier
   is two-slot TDMA from here, until an acquisition reset or a carrier boundary (dsd_state::tdma_carrier). The locked
   @p color_code is the carrier's received access code, noted before the burst it came with is dispatched, so whatever
   that burst publishes already carries it. */
static void
dmr_confidence_note_evidence(dsd_state* state, unsigned int color_code) {
    state->dmr_confidence_lock_m = dsd_decode_now_mono_s();
    state->tdma_carrier = 1U;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, (uint16_t)color_code);
    dsd_signal_search_note_evidence(state, "DMR", 0, 0, NULL);
}

static int
dmr_confidence_valid_cc(unsigned int color_code) {
    return color_code <= 15U;
}

static void
dmr_confidence_clear_voice(dsd_state* state) {
    DSD_MEMSET(state->dmr_confidence_voice_sync_seen, 0, sizeof(state->dmr_confidence_voice_sync_seen));
    DSD_MEMSET(state->dmr_confidence_voice_open, 0, sizeof(state->dmr_confidence_voice_open));
    DSD_MEMSET(state->dmr_confidence_voice_count, 0, sizeof(state->dmr_confidence_voice_count));
    state->dmr_confidence_voice_m[0] = 0.0;
    state->dmr_confidence_voice_m[1] = 0.0;
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
    state->dmr_confidence_voice_m[slot] = 0.0;
}

static dmr_confidence_result
dmr_confidence_observe_cc(dsd_state* state, unsigned int color_code, int may_lock) {
    if (!state || !dmr_confidence_valid_cc(color_code)) {
        return DMR_CONFIDENCE_REJECT;
    }

    if (state->dmr_confidence_locked) {
        if (state->dmr_confidence_color_code == color_code) {
            /* A restored target retains its acquisition hint, but must receive matching evidence again. */
            state->dmr_color_code = color_code;
            // Matching evidence restarts any pending re-lock streak so sporadic
            // co-channel interference never accumulates into a re-lock.
            state->dmr_confidence_candidate_cc = DMR_CONFIDENCE_UNKNOWN_CC;
            state->dmr_confidence_candidate_count = 0;
            dmr_confidence_note_evidence(state, color_code);
            return DMR_CONFIDENCE_LOCKED;
        }
        if (state->dmr_confidence_mismatch_count < 255U) {
            state->dmr_confidence_mismatch_count++;
        }
        // Sustained consistent evidence of a different colour code re-locks the
        // gate (the channel identity changed without a carrier drop) instead of
        // rejecting every burst until the next no-carrier reset. The candidate
        // fields are unused while locked, so they carry the mismatch streak.
        if (state->dmr_confidence_candidate_cc != color_code) {
            state->dmr_confidence_candidate_cc = (uint8_t)color_code;
            state->dmr_confidence_candidate_count = 1;
        } else if (state->dmr_confidence_candidate_count < 255U) {
            state->dmr_confidence_candidate_count++;
        }
        if (state->dmr_confidence_candidate_count >= DMR_CONFIDENCE_RELOCK_OBSERVES) {
            state->dmr_confidence_color_code = (uint8_t)color_code;
            state->dmr_color_code = color_code;
            state->dmr_confidence_candidate_cc = DMR_CONFIDENCE_UNKNOWN_CC;
            state->dmr_confidence_candidate_count = 0;
            state->dmr_confidence_mismatch_count = 0;
            // The previous system identity is gone; voice slots must
            // re-qualify under the new colour code before audio resumes,
            // and RAS is the new system's to prove.
            dmr_confidence_clear_voice(state);
            dmr_ras_reset(state);
            dmr_confidence_note_evidence(state, color_code);
            return DMR_CONFIDENCE_LOCKED;
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
        dmr_confidence_note_evidence(state, color_code);
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
        dmr_confidence_note_evidence(state, state->dmr_confidence_color_code);
        state->dmr_confidence_voice_m[slot] = state->dmr_confidence_lock_m;
        return DMR_CONFIDENCE_LOCKED;
    }

    return DMR_CONFIDENCE_PENDING;
}

dmr_confidence_result
dmr_confidence_note_data_burst(dsd_state* state, unsigned int color_code) {
    // Any data burst with a valid slot type may contribute to the colour-code
    // lock, not just IDLE: a dedicated TIII TSCC transmits continuous CSBK
    // signalling and may never carry IDLE bursts or voice (issue #348).
    if (!state || !dmr_confidence_valid_cc(color_code)) {
        return DMR_CONFIDENCE_REJECT;
    }
    if (state->dmr_confidence_locked) {
        return dmr_confidence_observe_cc(state, color_code, 0);
    }
    return dmr_confidence_observe_cc(state, color_code, 1);
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
