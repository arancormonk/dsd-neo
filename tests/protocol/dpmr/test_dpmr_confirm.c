// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit coverage for the dPMR frame-content confirmation gate (issue #407).
 *
 * The gate decides whether a frame has proved itself well enough for the decoder to publish
 * its identities, open a call row, or synthesize its voice. Both CCH halves passing their
 * CRC-7 in one frame is one chance in 16384 and confirms outright; one half is one in 128,
 * so it has to repeat.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dpmr_confirm.h"
#include "dsd-neo/core/state_fwd.h"

static int g_failures;

static void
expect(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

/** @brief One frame whose CCH reported @p evidence, or nothing when @p evidence is 0. */
static void
run_frame(dsd_state* state, int evidence) {
    dpmr_confirm_begin_frame(state);
    if (evidence != 0) {
        dpmr_confirm_note_evidence(state, (dpmr_evidence)evidence);
    }
    dpmr_confirm_end_frame(NULL, state);
}

/* "Consecutive" is in time, where frames say where they begin: the second frame's FS2 one frame after the first's,
 * within a symbol. Two weak frames further apart (other protocols' syncs between them under Auto) are two isolated
 * passes, and the first frame read after a reset runs on from nothing. */
static void
check_weak_frames_must_run(dsd_state* state) {
    static const struct {
        uint32_t gap;
        int confirms;
    } runs[] = {
        {DPMR_CONFIRM_FRAME_SYMBOLS, 1},
        {DPMR_CONFIRM_FRAME_SYMBOLS - 1U, 1},
        {DPMR_CONFIRM_FRAME_SYMBOLS + 1U, 1},
        {DPMR_CONFIRM_FRAME_SYMBOLS - 2U, 0},
        {DPMR_CONFIRM_FRAME_SYMBOLS + 2U, 0},
        {2U * DPMR_CONFIRM_FRAME_SYMBOLS, 0},
        {5000U, 0},
    };

    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        char label[96];
        dpmr_confirm_reset(state);
        dpmr_confirm_note_frame_start(state, 0xFFFFFF00U);
        run_frame(state, DPMR_EVIDENCE_WEAK);
        dpmr_confirm_note_frame_start(state, 0xFFFFFF00U + runs[i].gap);
        run_frame(state, DPMR_EVIDENCE_WEAK);
        DSD_SNPRINTF(label, sizeof label, "two weak frames %u symbols apart", (unsigned)runs[i].gap);
        expect(label, dpmr_confirm_is_confirmed(state), runs[i].confirms);
    }
    /* An isolated weak frame starts the streak over, and the frame that runs on from it completes it. */
    dpmr_confirm_reset(state);
    dpmr_confirm_note_frame_start(state, 1000U);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_note_frame_start(state, 6000U);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("an isolated weak frame is pending", dpmr_confirm_is_confirmed(state), 0);
    dpmr_confirm_note_frame_start(state, 6000U + DPMR_CONFIRM_FRAME_SYMBOLS);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("the frame running on from it confirms", dpmr_confirm_is_confirmed(state), 1);
    /* Both halves of one isolated frame still confirm at once. */
    dpmr_confirm_reset(state);
    dpmr_confirm_note_frame_start(state, 9000U);
    run_frame(state, DPMR_EVIDENCE_STRONG);
    expect("an isolated strong frame confirms", dpmr_confirm_is_confirmed(state), 1);
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

/* Issue #834: what a scan quiet check hears through the content-evidence hook, the last note and how many. */
static int g_quiet_notes;
static int g_quiet_level;
static uint32_t g_quiet_hold_ms;

static void
record_quiet_note(dsd_state* state, int level, uint32_t hold_ms) {
    (void)state;
    g_quiet_notes++;
    g_quiet_level = level;
    g_quiet_hold_ms = hold_ms;
}

static void
quiet_notes_clear(void) {
    g_quiet_notes = 0;
    g_quiet_level = 0;
    g_quiet_hold_ms = 0U;
}

/* The recorder installed (or, with @p on 0, nothing). */
static void
quiet_notes_install(int on) {
    dsd_frame_sync_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.content_evidence = on ? record_quiet_note : NULL;
    dsd_frame_sync_hooks_set(on ? &hooks : NULL);
    quiet_notes_clear();
}

/* One frame as run_frame(), closed with @p opts: the last note's level, 0 for none. */
static int
quiet_frame(const dsd_opts* opts, dsd_state* state, int evidence) {
    quiet_notes_clear();
    dpmr_confirm_begin_frame(state);
    if (evidence != 0) {
        dpmr_confirm_note_evidence(state, (dpmr_evidence)evidence);
    }
    dpmr_confirm_end_frame(opts, state);
    expect("a frame makes one note at most", g_quiet_notes <= 1, 1);
    return g_quiet_notes ? g_quiet_level : 0;
}

/* A frame's verdict for a scan quiet check, where a signal search hears it: PROVEN for both halves, or for the half
   that completes a weak streak; PARTIAL (two FS2 spans, 320 ms) for one half a later frame must complete; nothing for
   a frame that checked nothing, or with the setting off. */
static void
test_quiet_check_hears_the_frame_verdict(dsd_state* state) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    quiet_notes_install(1);
    dpmr_confirm_reset(state);
    expect("setting off: both halves note nothing", quiet_frame(&opts, state, DPMR_EVIDENCE_STRONG), 0);
    opts.scan_skip_silent = 1;
    dpmr_confirm_reset(state);
    expect("both halves are PROVEN", quiet_frame(&opts, state, DPMR_EVIDENCE_STRONG), DSD_CONTENT_EVIDENCE_PROVEN);
    expect("an empty frame notes nothing", quiet_frame(&opts, state, 0), 0);
    expect("one half is PARTIAL", quiet_frame(&opts, state, DPMR_EVIDENCE_WEAK), DSD_CONTENT_EVIDENCE_PARTIAL);
    expect("held for two FS2 spans", (int)g_quiet_hold_ms, (int)DPMR_CONFIRM_PARTIAL_HOLD_MS);
    expect("the half completing it is PROVEN", quiet_frame(&opts, state, DPMR_EVIDENCE_WEAK),
           DSD_CONTENT_EVIDENCE_PROVEN);
    quiet_notes_install(0);
    dpmr_confirm_reset(state);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_state\n");
        return 1;
    }

    /* Nothing is believed until a CRC checks out. */
    expect("fresh state is unconfirmed", dpmr_confirm_is_confirmed(state), 0);

    /* Both halves of one frame are enough. */
    run_frame(state, DPMR_EVIDENCE_STRONG);
    expect("one strong frame confirms", dpmr_confirm_is_confirmed(state), 1);

    /* Confirmation survives later frames that prove nothing: a real transmission fades and
     * recovers, and re-muting it mid-call would be worse than the false detection this
     * guards against. */
    run_frame(state, 0);
    run_frame(state, 0);
    expect("confirmation survives empty frames", dpmr_confirm_is_confirmed(state), 1);

    dpmr_confirm_reset(state);
    expect("reset clears confirmation", dpmr_confirm_is_confirmed(state), 0);

    /* One passing half is not enough; the second frame running is. */
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("one weak frame is pending", dpmr_confirm_is_confirmed(state), 0);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("two weak frames confirm", dpmr_confirm_is_confirmed(state), 1);

    /* The two have to be consecutive: noise produces isolated passes, a transmission
     * produces them continuously. */
    dpmr_confirm_reset(state);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    run_frame(state, 0);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("a gap breaks the weak streak", dpmr_confirm_is_confirmed(state), 0);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("the streak resumes from the gap", dpmr_confirm_is_confirmed(state), 1);

    check_weak_frames_must_run(state);

    /* Noting weak twice inside one frame is one frame's worth of evidence, not two --
     * otherwise a single lucky frame would confirm itself. The voice path cannot reach
     * this (it reports both halves as one strong), but the gate must not depend on that. */
    dpmr_confirm_reset(state);
    dpmr_confirm_begin_frame(state);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_end_frame(NULL, state);
    expect("weak evidence counts once per frame", dpmr_confirm_is_confirmed(state), 0);

    /* Strong evidence after a weak one in the same frame still confirms immediately. */
    dpmr_confirm_reset(state);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_begin_frame(state);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_STRONG);
    dpmr_confirm_end_frame(NULL, state);
    expect("strong evidence overrides a pending streak", dpmr_confirm_is_confirmed(state), 1);

    /* A signal search hears a frame's confirmation when the frame closes, after the frame has published the identity,
     * service and colour code its confirmation unlocked, and once per frame. */
    dsd_signal_search_evidence_set(count_search_report);
    dsd_signal_search_call_set(count_call_note);
    dpmr_confirm_reset(state);
    dpmr_confirm_begin_frame(state);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_STRONG);
    expect("a strong CCH is not reported to search mid-frame", g_search_reports, 0);
    expect("a strong CCH still confirms at once", dpmr_confirm_is_confirmed(state), 1);
    dpmr_confirm_end_frame(NULL, state);
    expect("the confirming frame reports once, at its end", g_search_reports, 1);
    run_frame(state, 0);
    expect("a frame that proved nothing reports nothing", g_search_reports, 1);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    expect("a lone weak frame reports nothing", g_search_reports, 1);
    dpmr_confirm_begin_frame(state);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    expect("the frame completing a weak streak is not reported mid-frame", g_search_reports, 1);
    dpmr_confirm_end_frame(NULL, state);
    expect("the frame completing a weak streak reports at its end", g_search_reports, 2);
    /* Each reported frame notes the call it published, with its evidence; no other frame does. */
    expect("a call note with every report", g_call_notes, g_search_reports);
    dsd_signal_search_evidence_set(NULL);
    dsd_signal_search_call_set(NULL);

    test_quiet_check_hears_the_frame_verdict(state);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    dpmr_confirm_reset(NULL);
    dpmr_confirm_begin_frame(NULL);
    dpmr_confirm_note_evidence(NULL, DPMR_EVIDENCE_STRONG);
    dpmr_confirm_end_frame(NULL, NULL);
    expect("null state is not confirmed", dpmr_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "DPMR_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
