// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit coverage for the NXDN frame-content confirmation gate (issue #398).
 *
 * The gate decides whether a frame has proved itself well enough for the decoder to stop a
 * scan on it, synthesize its voice, or publish its RAN. A CRC of 12 bits or more is proof
 * by itself; the 6- and 7-bit CRCs on SACCH and SCCH have to repeat, because one noise
 * frame in 64 clears a CRC-6.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"
#include "nxdn_confirm.h"

static int g_failures;

static void
expect(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

/** @brief One frame whose channels reported @p evidence, or nothing when @p evidence is 0. */
static void
run_frame(dsd_state* state, int evidence) {
    nxdn_confirm_begin_frame(state);
    if (evidence != 0) {
        nxdn_confirm_note_evidence(state, (nxdn_evidence)evidence);
    }
    nxdn_confirm_end_frame(state);
}

static int g_search_reports;
static int g_call_notes;

static void
count_call_note(const dsd_state* state, uint8_t slot) {
    (void)state;
    expect("a single-channel call is noted on slot 0", slot, 0);
    g_call_notes++;
}

static void
count_search_report(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target, uint32_t source,
                    const char* access) {
    (void)sync;
    (void)slot;
    (void)state;
    (void)protocol;
    (void)target;
    (void)source;
    (void)access;
    g_search_reports++;
}

/** @brief One frame as run_frame(), answering what the SPS hunt reads back from it. */
static int
run_frame_proved(dsd_state* state, int evidence) {
    run_frame(state, evidence);
    return nxdn_confirm_frame_proved(state);
}

/** @brief One frame beginning at symbol @p at, accepted on a run of the frame sync's own when @p sync_runs. */
static void
run_frame_at(dsd_state* state, uint32_t at, int sync_runs, int evidence) {
    nxdn_confirm_note_frame_start(state, at, sync_runs, 0);
    run_frame(state, evidence);
}

/* Where the frame sync does not vouch for a run (NXDN48), a frame runs on only from a frame read one frame
 * (NXDN_CONFIRM_FRAME_SYMBOLS, within the slip) before it: two short checks frames apart prove nothing, and a strong
 * check in an isolated frame waits for the next frame to run on from it. */
static void
test_frames_run_only_at_frame_spacing(dsd_state* state) {
    nxdn_confirm_reset(state);
    run_frame_at(state, 1000U, 0, NXDN_EVIDENCE_WEAK);
    run_frame_at(state, 1384U, 0, NXDN_EVIDENCE_WEAK);
    expect("weak checks two frames apart confirm nothing", nxdn_confirm_is_confirmed(state), 0);
    run_frame_at(state, 1384U + NXDN_CONFIRM_FRAME_SYMBOLS + NXDN_CONFIRM_FRAME_SYMBOLS_SLIP, 0, NXDN_EVIDENCE_WEAK);
    expect("weak checks of frames that run confirm", nxdn_confirm_is_confirmed(state), 1);

    nxdn_confirm_reset(state);
    run_frame_at(state, 5000U, 0, NXDN_EVIDENCE_STRONG);
    expect("an isolated strong check waits", nxdn_confirm_is_confirmed(state), 0);
    expect("an isolated strong check proves no frame", nxdn_confirm_frame_proved(state), 0);
    run_frame_at(state, 5000U + NXDN_CONFIRM_FRAME_SYMBOLS - NXDN_CONFIRM_FRAME_SYMBOLS_SLIP, 0, 0);
    expect("the next frame running on confirms it", nxdn_confirm_is_confirmed(state), 1);
    expect("the frame that confirmed it by running proves nothing itself", nxdn_confirm_frame_proved(state), 0);

    nxdn_confirm_reset(state);
    run_frame_at(state, 9000U, 0, NXDN_EVIDENCE_STRONG);
    run_frame_at(state, 9000U + NXDN_CONFIRM_FRAME_SYMBOLS + 2U, 0, 0);
    expect("a frame past the slip drops the waiting check", nxdn_confirm_is_confirmed(state), 0);
    run_frame_at(state, 9000U + (2U * NXDN_CONFIRM_FRAME_SYMBOLS) + 2U, 0, 0);
    expect("and a dropped check stays dropped", nxdn_confirm_is_confirmed(state), 0);

    /* Two checks of one isolated frame, a strong one among them, are a transmission whichever comes first; two short
     * ones are still one frame's worth. */
    static const nxdn_evidence k_pairs[3][2] = {
        {NXDN_EVIDENCE_WEAK, NXDN_EVIDENCE_STRONG},
        {NXDN_EVIDENCE_STRONG, NXDN_EVIDENCE_WEAK},
        {NXDN_EVIDENCE_WEAK, NXDN_EVIDENCE_WEAK},
    };
    for (int p = 0; p < 3; p++) {
        nxdn_confirm_reset(state);
        nxdn_confirm_note_frame_start(state, 8000U, 0, 0);
        nxdn_confirm_begin_frame(state);
        nxdn_confirm_note_evidence(state, k_pairs[p][0]);
        nxdn_confirm_note_evidence(state, k_pairs[p][1]);
        expect(p < 2 ? "two checks of one frame, one strong, confirm" : "two short checks of one frame do not",
               nxdn_confirm_is_confirmed(state), p < 2 ? 1 : 0);
        nxdn_confirm_end_frame(state);
    }

    /* A waiting strong check is the frame before's: only a frame read a frame after it confirms it, never one the frame
     * sync vouches for on its own run (an NXDN96 frame after a step from 2400/4). */
    nxdn_confirm_reset(state);
    run_frame_at(state, 11000U, 0, NXDN_EVIDENCE_STRONG);
    run_frame_at(state, 11700U, 1, 0);
    expect("an nxdn96 frame does not confirm an nxdn48 frame's waiting check", nxdn_confirm_is_confirmed(state), 0);

    nxdn_confirm_reset(state);
    run_frame_at(state, 3000U, 0, NXDN_EVIDENCE_STRONG);
    run_frame_at(state, 3192U, 0, NXDN_EVIDENCE_STRONG);
    expect("a strong check in a frame that runs confirms at once", nxdn_confirm_is_confirmed(state), 1);

    /* The SCCH run, too, is of frames that run. */
    nxdn_confirm_reset(state);
    nxdn_confirm_note_frame_start(state, 7000U, 0, 0);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_scch(state);
    nxdn_confirm_end_frame(state);
    nxdn_confirm_note_frame_start(state, 7500U, 0, 0);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_scch(state);
    expect("SCCHs frames apart are no run", nxdn_confirm_scch_confirmed(state), 0);
    nxdn_confirm_end_frame(state);

    /* An isolated frame whose sync word followed another one frame before it (NXDN48's corroborated accept, a call's
     * first frame read): its strong check holds at once, though its weak one starts the streak over. */
    nxdn_confirm_reset(state);
    run_frame_at(state, 2000U, 0, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_note_frame_start(state, 2500U, 0, 1);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("a corroborated sync word runs no weak streak on", nxdn_confirm_is_confirmed(state), 0);
    nxdn_confirm_note_frame_start(state, 6000U, 0, 1);
    run_frame(state, NXDN_EVIDENCE_STRONG);
    expect("a corroborated sync word's strong check holds at once", nxdn_confirm_is_confirmed(state), 1);

    /* A frame the frame sync accepted on a run of its own (NXDN96) runs on whatever came before it. */
    nxdn_confirm_reset(state);
    run_frame_at(state, 100U, 1, NXDN_EVIDENCE_STRONG);
    expect("a strong check behind a run the sync vouched for confirms at once", nxdn_confirm_is_confirmed(state), 1);

    /* A reset forgets where the last frame was: the next one runs on from nothing. */
    nxdn_confirm_reset(state);
    run_frame_at(state, 100U + NXDN_CONFIRM_FRAME_SYMBOLS, 0, NXDN_EVIDENCE_STRONG);
    expect("after a reset the first frame is isolated", nxdn_confirm_is_confirmed(state), 0);
    nxdn_confirm_reset(state);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_state\n");
        return 1;
    }

    /* Nothing is believed until something checks out. */
    expect("fresh state is unconfirmed", nxdn_confirm_is_confirmed(state), 0);

    /* One strong CRC is enough. */
    run_frame(state, NXDN_EVIDENCE_STRONG);
    expect("one strong frame confirms", nxdn_confirm_is_confirmed(state), 1);

    /* Confirmation survives later frames that prove nothing: a real call fades and recovers,
     * and re-muting it mid-transmission would be worse than the false detection this guards. */
    run_frame(state, 0);
    run_frame(state, 0);
    expect("confirmation survives empty frames", nxdn_confirm_is_confirmed(state), 1);

    nxdn_confirm_reset(state);
    expect("reset clears confirmation", nxdn_confirm_is_confirmed(state), 0);

    /* One short CRC is not enough; the second frame running is. */
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("one weak frame is pending", nxdn_confirm_is_confirmed(state), 0);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("two weak frames confirm", nxdn_confirm_is_confirmed(state), 1);

    /* The two have to be consecutive. */
    nxdn_confirm_reset(state);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    run_frame(state, 0);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("a gap breaks the weak streak", nxdn_confirm_is_confirmed(state), 0);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("the streak resumes from the gap", nxdn_confirm_is_confirmed(state), 1);

    /* Several short CRCs inside one frame are one frame's worth of evidence, not several --
     * otherwise a single lucky frame would confirm itself. */
    nxdn_confirm_reset(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_end_frame(state);
    expect("weak evidence counts once per frame", nxdn_confirm_is_confirmed(state), 0);

    /* A strong CRC arriving after a weak one still confirms immediately. */
    nxdn_confirm_reset(state);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_STRONG);
    nxdn_confirm_end_frame(state);
    expect("strong evidence overrides a pending streak", nxdn_confirm_is_confirmed(state), 1);

    /* What the SPS hunt reads back: which frames carried a passing CRC of their own, as
     * opposed to which transmissions have been confirmed at some point. Only the former
     * proves the profile the frame was read on (#445), so the two answers have to come
     * apart on a confirmed transmission's empty frames. */
    nxdn_confirm_reset(state);
    expect("a fresh frame has proved nothing", nxdn_confirm_frame_proved(state), 0);
    expect("one weak frame has not proved the profile", run_frame_proved(state, NXDN_EVIDENCE_WEAK), 0);
    expect("the frame completing a weak streak proves it", run_frame_proved(state, NXDN_EVIDENCE_WEAK), 1);
    expect("a confirmed transmission's empty frame proves nothing", run_frame_proved(state, 0), 0);
    expect("a later weak frame proves it again", run_frame_proved(state, NXDN_EVIDENCE_WEAK), 1);

    nxdn_confirm_reset(state);
    expect("one strong frame proves the profile", run_frame_proved(state, NXDN_EVIDENCE_STRONG), 1);
    expect("the frame after it proves nothing", run_frame_proved(state, 0), 0);

    /* A signal search hears a frame's confirmation when the frame closes, after the channels that confirmed it have
     * published what they decoded (the RAN, a call), and once per frame however many CRCs passed in it. */
    dsd_signal_search_evidence_set(count_search_report);
    dsd_signal_search_call_set(count_call_note);
    nxdn_confirm_reset(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_STRONG);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_STRONG);
    expect("a strong CRC is not reported to search mid-frame", g_search_reports, 0);
    expect("a strong CRC still confirms at once", nxdn_confirm_is_confirmed(state), 1);
    nxdn_confirm_end_frame(state);
    expect("the confirming frame reports once, at its end", g_search_reports, 1);
    run_frame(state, 0);
    expect("a frame that proved nothing reports nothing", g_search_reports, 1);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("a lone weak frame reports nothing", g_search_reports, 1);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    expect("the frame completing a weak streak is not reported mid-frame", g_search_reports, 1);
    nxdn_confirm_end_frame(state);
    expect("the frame completing a weak streak reports at its end", g_search_reports, 2);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("a frame continuing the streak reports", g_search_reports, 3);
    /* Each reported frame notes the call it published, with its evidence; no other frame does. */
    expect("a call note with every report", g_call_notes, g_search_reports);
    dsd_signal_search_evidence_set(NULL);
    dsd_signal_search_call_set(NULL);

    /* The control-channel hunt's evidence stamp: written exactly when a frame's own CRCs confirmed the transmission (as
     * a signal search hears it), on the decode clock, and never cleared by a reset. */
    dsd_decode_clock_use_test(5000000000ULL); // 5 s
    nxdn_confirm_reset(state);
    state->nxdn_confirm_lock_m = 0.0;
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("a lone weak frame stamps nothing", state->nxdn_confirm_lock_m > 0.0, 0);
    run_frame(state, NXDN_EVIDENCE_WEAK);
    expect("the frame completing a weak streak stamps",
           state->nxdn_confirm_lock_m > 4.999 && state->nxdn_confirm_lock_m < 5.001, 1);
    dsd_decode_clock_test_set_ns(7000000000ULL); // 7 s
    run_frame(state, 0);
    expect("a confirmed transmission's empty frame keeps the stamp", state->nxdn_confirm_lock_m < 5.001, 1);
    run_frame(state, NXDN_EVIDENCE_STRONG);
    expect("a strong frame stamps", state->nxdn_confirm_lock_m > 6.999, 1);
    nxdn_confirm_reset(state);
    expect("a reset keeps the stamp", state->nxdn_confirm_lock_m > 6.999, 1);
    dsd_decode_clock_use_system();

    /* The SCCH run the Type-D site test reads: SCCHs in frames running, which a SACCH frame or a frame rejected at its
     * LICH ends, and a strong CRC beside an SCCH does not. */
    nxdn_confirm_reset(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_note_scch(state);
    nxdn_confirm_end_frame(state);
    nxdn_confirm_frame_rejected(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK);
    nxdn_confirm_note_scch(state);
    expect("a rejected frame ends the SCCH run", nxdn_confirm_scch_confirmed(state), 0);
    nxdn_confirm_end_frame(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_scch(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_STRONG);
    expect("two SCCH frames running confirm, a strong CRC beside", nxdn_confirm_scch_confirmed(state), 1);
    nxdn_confirm_end_frame(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_WEAK); // a SACCH
    nxdn_confirm_end_frame(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_scch(state);
    expect("a SACCH frame ends the SCCH run", nxdn_confirm_scch_confirmed(state), 0);
    nxdn_confirm_end_frame(state);
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_scch(state); // the second of a run ...
    nxdn_confirm_end_frame(state);
    dsd_trunk_tuning_generation_advance(); // ... cut by a grant's completed tune to another carrier
    nxdn_confirm_begin_frame(state);
    nxdn_confirm_note_scch(state);
    expect("a completed tune ends the SCCH run", nxdn_confirm_scch_confirmed(state), 0);
    nxdn_confirm_end_frame(state);

    test_frames_run_only_at_frame_spacing(state);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    nxdn_confirm_reset(NULL);
    nxdn_confirm_note_frame_start(NULL, 0U, 0, 0);
    nxdn_confirm_begin_frame(NULL);
    nxdn_confirm_note_evidence(NULL, NXDN_EVIDENCE_STRONG);
    nxdn_confirm_end_frame(NULL);
    expect("null state is not confirmed", nxdn_confirm_is_confirmed(NULL), 0);
    expect("null state has proved nothing", nxdn_confirm_frame_proved(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "NXDN_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
