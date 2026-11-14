// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit coverage for the M17 frame-content confirmation gate (issue #399).
 *
 * Under AUTO the M17 sync chain starts from a preamble candidate that is nothing more than an
 * alternating symbol run, so the decoder needs the frame body to say something checkable before
 * it opens a call, synthesizes voice, or tells the SPS hunt the profile is carrying traffic. The
 * LSF and packet CRC-16s are proof by themselves; a LICH that clears its six Golay(24,12) blocks
 * is not, because a whole LICH clears on random bits often enough to happen on noise, so it has
 * to repeat.
 *
 * Sibling of tests/protocol/nxdn/test_nxdn_confirm.c.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"
#include "m17_confirm.h"

static int g_failures;

static void
expect(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

/** @brief One frame whose decode reported @p evidence, or nothing when @p evidence is 0. */
static void
run_frame(dsd_state* state, int evidence) {
    m17_confirm_begin_frame(state);
    if (evidence != 0) {
        m17_confirm_note_evidence(NULL, state, (m17_evidence)evidence);
    }
    m17_confirm_end_frame(state);
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

/* One frame noting each of @p evidence[0..count) with @p opts: the last note's level, 0 for none. */
static int
quiet_frame(const dsd_opts* opts, dsd_state* state, const int* evidence, int count) {
    quiet_notes_clear();
    m17_confirm_begin_frame(state);
    for (int i = 0; i < count; i++) {
        m17_confirm_note_evidence(opts, state, (m17_evidence)evidence[i]);
    }
    m17_confirm_end_frame(state);
    expect("a frame makes one note at most", g_quiet_notes <= 1, 1);
    return g_quiet_notes ? g_quiet_level : 0;
}

/* A check's verdict for a scan quiet check, where a signal search hears it (inside m17_confirm_note_evidence()):
   PROVEN for an LSF or packet CRC or a BERT lock, and for the LICH completing a weak streak; PARTIAL (two stream
   frames, 80 ms) for one clean LICH a later frame must complete; nothing for an empty frame, or with the setting off.
   A LICH after a CRC in the same frame adds nothing. */
static void
test_quiet_check_hears_the_frame_verdict(dsd_state* state) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    static const int k_strong[] = {M17_EVIDENCE_STRONG};
    static const int k_weak[] = {M17_EVIDENCE_WEAK};
    static const int k_strong_weak[] = {M17_EVIDENCE_STRONG, M17_EVIDENCE_WEAK};
    quiet_notes_install(1);
    m17_confirm_reset(state);
    expect("setting off: an LSF CRC notes nothing", quiet_frame(&opts, state, k_strong, 1), 0);
    opts.scan_skip_silent = 1;
    m17_confirm_reset(state);
    expect("an LSF CRC is PROVEN", quiet_frame(&opts, state, k_strong, 1), DSD_CONTENT_EVIDENCE_PROVEN);
    expect("an empty frame notes nothing", quiet_frame(&opts, state, NULL, 0), 0);
    expect("one LICH is PARTIAL", quiet_frame(&opts, state, k_weak, 1), DSD_CONTENT_EVIDENCE_PARTIAL);
    expect("held for two stream frames", (int)g_quiet_hold_ms, (int)M17_CONFIRM_PARTIAL_HOLD_MS);
    expect("the second LICH is PROVEN", quiet_frame(&opts, state, k_weak, 1), DSD_CONTENT_EVIDENCE_PROVEN);
    m17_confirm_reset(state);
    expect("a LICH after a CRC adds nothing", quiet_frame(&opts, state, k_strong_weak, 2), DSD_CONTENT_EVIDENCE_PROVEN);
    quiet_notes_install(0);
    m17_confirm_reset(state);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_state\n");
        return 1;
    }

    /* Nothing is believed until something checks out. */
    expect("fresh state is unconfirmed", m17_confirm_is_confirmed(state), 0);

    /* One CRC-16 -- an LSF, a packet, or a PRBS9 lock -- is enough. */
    run_frame(state, M17_EVIDENCE_STRONG);
    expect("one strong frame confirms", m17_confirm_is_confirmed(state), 1);

    /* Confirmation survives later frames that prove nothing: a real transmission fades and
     * recovers, and muting it mid-call would be worse than the false detection this guards. */
    run_frame(state, 0);
    run_frame(state, 0);
    expect("confirmation survives empty frames", m17_confirm_is_confirmed(state), 1);

    m17_confirm_reset(state);
    expect("reset clears confirmation", m17_confirm_is_confirmed(state), 0);

    /* One clean LICH is not enough; the second frame running is. */
    run_frame(state, M17_EVIDENCE_WEAK);
    expect("one weak frame is pending", m17_confirm_is_confirmed(state), 0);
    run_frame(state, M17_EVIDENCE_WEAK);
    expect("two weak frames confirm", m17_confirm_is_confirmed(state), 1);

    /* The two have to be consecutive. */
    m17_confirm_reset(state);
    run_frame(state, M17_EVIDENCE_WEAK);
    run_frame(state, 0);
    run_frame(state, M17_EVIDENCE_WEAK);
    expect("a gap breaks the weak streak", m17_confirm_is_confirmed(state), 0);
    run_frame(state, M17_EVIDENCE_WEAK);
    expect("the streak resumes from the gap", m17_confirm_is_confirmed(state), 1);

    /* Repeated weak reports inside one frame are one frame's worth of evidence, not several --
     * otherwise a single lucky frame would confirm itself. */
    m17_confirm_reset(state);
    m17_confirm_begin_frame(state);
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_WEAK);
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_WEAK);
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_WEAK);
    m17_confirm_end_frame(state);
    expect("weak evidence counts once per frame", m17_confirm_is_confirmed(state), 0);

    /* A CRC arriving after a clean LICH in the same frame still confirms immediately. */
    m17_confirm_reset(state);
    run_frame(state, M17_EVIDENCE_WEAK);
    m17_confirm_begin_frame(state);
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_WEAK);
    m17_confirm_note_evidence(NULL, state, M17_EVIDENCE_STRONG);
    m17_confirm_end_frame(state);
    expect("strong evidence overrides a pending streak", m17_confirm_is_confirmed(state), 1);

    test_quiet_check_hears_the_frame_verdict(state);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    m17_confirm_reset(NULL);
    m17_confirm_begin_frame(NULL);
    m17_confirm_note_evidence(NULL, NULL, M17_EVIDENCE_STRONG);
    m17_confirm_end_frame(NULL);
    expect("null state is not confirmed", m17_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "M17_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
