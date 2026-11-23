// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include "nxdn_confirm.h"

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
nxdn_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    state->nxdn_confirmed = 0;
    state->nxdn_confirm_weak_streak = 0;
    state->nxdn_confirm_frame_evidence = 0;
    state->nxdn_confirm_scch_streak = 0;
    state->nxdn_confirm_frame_scch = 0;
    state->nxdn_confirm_frame_isolated = 0;
    state->nxdn_confirm_strong_waits = 0;
    state->nxdn_confirm_frame_checks = 0;
    state->nxdn_confirm_strong_pending = 0;
    state->nxdn_confirm_spacing_due = 0;
    state->nxdn_confirm_frame_spaced = 0;
    state->nxdn_confirm_last_frame_valid = 0;
    state->nxdn_confirm_last_frame_foreign = 0;
}

/* Whether a frame that began @p since symbols after the last one read began one frame after it, within the slip. */
static int
nxdn_confirm_one_frame_after(uint32_t since) {
    return since + NXDN_CONFIRM_FRAME_SYMBOLS_SLIP >= NXDN_CONFIRM_FRAME_SYMBOLS
           && since <= NXDN_CONFIRM_FRAME_SYMBOLS + NXDN_CONFIRM_FRAME_SYMBOLS_SLIP;
}

void
nxdn_confirm_note_frame_start(dsd_state* state, uint32_t frame_symbol, int sync_runs, int sync_corroborated) {
    if (!state) {
        return;
    }
    const uint32_t since = frame_symbol - state->nxdn_confirm_last_frame_symbol;
    const int last_valid = state->nxdn_confirm_last_frame_valid != 0;
    const int last_foreign = last_valid && state->nxdn_confirm_last_frame_foreign != 0;
    const int one_frame_on = last_valid && nxdn_confirm_one_frame_after(since);
    /* Another channel's frame (dsd_state::nxdn_frame_foreign, committed before its start is noted) is no part of the
       followed channel's run, either way. It runs on from nothing and is no frame to run on from, and a sync word the
       frame sync vouched for from within its reach of it is that foreign frame's: NXDN48's corroboration (an FSW one
       frame before) only when the foreign frame was read exactly one frame before, an NXDN96 run (a lock its LICH
       moved and flywheels on) for as long as the flywheel lasts. */
    const int foreign = state->nxdn_frame_foreign != 0;
    const uint32_t run_reach =
        (DSD_FRAME_SYNC_NXDN96_FLYWHEEL_FRAMES * NXDN_CONFIRM_FRAME_SYMBOLS) + NXDN_CONFIRM_FRAME_SYMBOLS_SLIP;
    const int run_from_foreign = last_foreign && since <= run_reach;
    const int corroboration_from_foreign = last_foreign && one_frame_on;
    const int spaced = one_frame_on && !last_foreign;
    const int corroborated = sync_corroborated && !corroboration_from_foreign;
    const int runs_on = !foreign && ((sync_runs && !run_from_foreign) || spaced);
    state->nxdn_confirm_spacing_due = 0;
    state->nxdn_confirm_last_frame_symbol = frame_symbol;
    state->nxdn_confirm_last_frame_valid = 1;
    state->nxdn_confirm_last_frame_foreign = (uint8_t)foreign;
    state->nxdn_confirm_frame_isolated = (uint8_t)!runs_on;
    state->nxdn_confirm_strong_waits = (uint8_t)(!runs_on && !corroborated);
    if (!runs_on) {
        state->nxdn_confirm_weak_streak = 0;
        state->nxdn_confirm_scch_streak = 0;
        state->nxdn_confirm_strong_pending = 0;
        return;
    }
    if (state->nxdn_confirm_strong_pending) {
        /* The frame before this one passed a strong check, and this one was read a frame after it: a transmission. A
           frame the frame sync vouches for on its own run (NXDN96) says nothing of the frame before: the check waited
           for the followed transmission's next frame, and this is not one. */
        state->nxdn_confirm_strong_pending = 0;
        if (spaced) {
            state->nxdn_confirmed = 1;
            state->nxdn_confirm_spacing_due = 1;
        }
    }
}

void
nxdn_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->nxdn_confirm_frame_evidence = 0;
    state->nxdn_confirm_frame_scch = 0;
    state->nxdn_confirm_frame_checks = 0;
    /* Only the frame whose start confirmed the check counts it: one begun without a start of its own does not. */
    state->nxdn_confirm_frame_spaced = state->nxdn_confirm_spacing_due;
    state->nxdn_confirm_spacing_due = 0;
}

void
nxdn_confirm_note_scch(dsd_state* state) {
    if (!state || state->nxdn_confirm_frame_scch) {
        return;
    }
    state->nxdn_confirm_frame_scch = 1;
    /* A grant, a probe or a return the tuner completed put the receiver on another carrier, whose SCCHs start their own
       run: no pass of the no-carrier code need come between. */
    const uint64_t generation = dsd_trunk_tuning_generation();
    if (generation != state->nxdn_confirm_scch_generation) {
        state->nxdn_confirm_scch_generation = generation;
        state->nxdn_confirm_scch_streak = 0;
    }
    if (state->nxdn_confirm_scch_streak < 0xFFU) {
        state->nxdn_confirm_scch_streak++;
    }
}

void
nxdn_confirm_frame_rejected(dsd_state* state) {
    if (state) {
        state->nxdn_confirm_scch_streak = 0;
        state->nxdn_confirm_frame_isolated = 0;
        state->nxdn_confirm_strong_waits = 0;
    }
}

int
nxdn_confirm_scch_confirmed(const dsd_state* state) {
    return state && state->nxdn_confirm_scch_streak >= NXDN_CONFIRM_WEAK_OBSERVES;
}

void
nxdn_confirm_note_evidence(dsd_state* state, nxdn_evidence evidence) {
    if (!state) {
        return;
    }
    if (state->nxdn_frame_foreign) {
        /* Another channel's control traffic on the followed traffic channel proves that channel, not the call followed:
           it is noted as heard, and confirms nothing. */
        state->nxdn_site.foreign_control_m = dsd_decode_now_mono_s();
        /* A frame its own CAC showed foreign part way through anchors the followed channel's run no more than one
           committed foreign (nxdn_confirm_note_frame_start()). */
        state->nxdn_confirm_last_frame_foreign = 1;
        return;
    }
    if (state->nxdn_confirm_strong_waits) {
        /* An isolated frame's strong check waits for the next frame to run on from it, and counts as a weak one
           meanwhile, unless another check of the same frame passed too: two CRCs of one frame are a transmission. */
        if (state->nxdn_confirm_frame_checks < 0xFFU) {
            state->nxdn_confirm_frame_checks++;
        }
        if (evidence == NXDN_EVIDENCE_STRONG) {
            state->nxdn_confirm_strong_pending = 1;
        }
        const int holds = state->nxdn_confirm_strong_pending && state->nxdn_confirm_frame_checks >= 2U;
        if (holds) {
            state->nxdn_confirm_strong_pending = 0;
        }
        evidence = holds ? NXDN_EVIDENCE_STRONG : NXDN_EVIDENCE_WEAK;
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

/* Two frames of the variant on air: NXDN96's only when the decoder set runs NXDN96 alone, else NXDN48's, the longer. */
static uint32_t
nxdn_confirm_partial_hold_ms(const dsd_opts* opts) {
    return (opts && opts->frame_nxdn96 == 1 && opts->frame_nxdn48 != 1) ? NXDN_CONFIRM_PARTIAL_HOLD_MS_96
                                                                        : NXDN_CONFIRM_PARTIAL_HOLD_MS_48;
}

void
nxdn_confirm_end_frame(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    state->nxdn_confirm_frame_isolated = 0;
    state->nxdn_confirm_strong_waits = 0;
    if (!state->nxdn_confirm_frame_scch) {
        state->nxdn_confirm_scch_streak = 0;
    }
    if (state->nxdn_confirm_frame_evidence == 0) {
        state->nxdn_confirm_weak_streak = 0;
    }
    /* The frame's own CRCs confirmed the transmission: a strong one, or the short one that completes or continues a weak
     * streak. A signal search hears that only now, once the frame's channels have published the RAN and call their
     * confirmation unlocked, since it reads them from state when it hears it. */
    if (nxdn_confirm_frame_content_proven(state)) {
        /* Validated NXDN content: the frequency is not dead, even when it carries no control channel. */
        state->nxdn_confirm_lock_m = dsd_decode_now_mono_s();
        dsd_signal_search_note_evidence(state, "NXDN", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
        return;
    }
    if (state->nxdn_confirm_frame_spaced) {
        /* This frame's spacing confirmed the strong check the frame before left waiting (issue #834): proof of that
           frame's content, which a scan quiet check hears once, here, whatever this frame checked itself. The frames
           after it prove nothing by running on. */
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
        return;
    }
    if (state->nxdn_confirm_frame_evidence == 0) {
        return;
    }
    /* A weak frame whose streak the next frame must complete: the quiet check waits for that frame. */
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PARTIAL, nxdn_confirm_partial_hold_ms(opts));
}

int
nxdn_confirm_is_confirmed(const dsd_state* state) {
    return state && state->nxdn_confirmed != 0;
}

int
nxdn_confirm_frame_content_proven(const dsd_state* state) {
    return state && state->nxdn_confirm_frame_evidence != 0
           && (state->nxdn_confirm_frame_evidence == (uint8_t)NXDN_EVIDENCE_STRONG
               || state->nxdn_confirm_weak_streak >= NXDN_CONFIRM_WEAK_OBSERVES);
}

int
nxdn_confirm_frame_content_counted(const dsd_state* state) {
    return nxdn_confirm_frame_content_proven(state) || (state && state->nxdn_confirm_frame_spaced != 0);
}

int
nxdn_confirm_frame_proved(const dsd_state* state) {
    return state && state->nxdn_confirmed != 0 && state->nxdn_confirm_frame_evidence != 0;
}
