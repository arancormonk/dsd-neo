// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit coverage for the D-STAR transmission confirmation gate (issue #421).
 *
 * The gate decides whether the 1992 symbols a voice superframe consumes bought the hunt
 * profile anything. A CRC-16/X.25 -- the RF header, or the header rebroadcast in slow data --
 * is proof by itself; a superframe that carries only filler has to repeat, because one
 * arriving at all means a second exact 24-symbol sync word was matched.
 *
 * Sibling of tests/protocol/provoice/test_provoice_confirm.c and
 * tests/protocol/nxdn/test_nxdn_confirm.c.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"
#include "dstar_confirm.h"

static int g_failures;

static void
expect(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

/** @brief One superframe that reported @p evidence, or nothing when @p evidence is 0. */
static void
run_frame(dsd_state* state, int evidence) {
    dstar_confirm_begin_frame(state);
    if (evidence != 0) {
        dstar_confirm_note_evidence(NULL, state, (dstar_evidence)evidence);
    }
    dstar_confirm_end_frame(NULL, state);
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

/* One superframe noting each of @p evidence[0..count) with @p opts: the notes made (the last one's level in
   g_quiet_level). */
static int
quiet_frame(const dsd_opts* opts, dsd_state* state, const int* evidence, int count) {
    quiet_notes_clear();
    dstar_confirm_begin_frame(state);
    for (int i = 0; i < count; i++) {
        dstar_confirm_note_evidence(opts, state, (dstar_evidence)evidence[i]);
    }
    dstar_confirm_end_frame(opts, state);
    return g_quiet_notes;
}

/* A superframe's verdict for a scan quiet check: PROVEN where a signal search hears the confirmation (an RF or
   slow-data header CRC, the superframe completing a weak streak); PARTIAL at the superframe's end (two superframes, 840
   ms) when only its own sync stands behind it and the streak is short; nothing for an empty superframe, or with the
   setting off. A header CRC in the superframe leaves no PARTIAL. */
static void
test_quiet_check_hears_the_frame_verdict(dsd_state* state) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    static const int k_weak[] = {DSTAR_EVIDENCE_WEAK};
    static const int k_weak_strong[] = {DSTAR_EVIDENCE_WEAK, DSTAR_EVIDENCE_STRONG};
    quiet_notes_install(1);
    dstar_confirm_reset(state);
    expect("setting off: a header CRC notes nothing", quiet_frame(&opts, state, k_weak_strong, 2), 0);
    opts.scan_skip_silent = 1;
    dstar_confirm_reset(state);
    expect("a header CRC: one note", quiet_frame(&opts, state, k_weak_strong, 2), 1);
    expect("a header CRC is PROVEN, no PARTIAL", g_quiet_level, DSD_CONTENT_EVIDENCE_PROVEN);
    expect("an empty superframe notes nothing", quiet_frame(&opts, state, NULL, 0), 0);
    expect("a lone voice superframe: one note", quiet_frame(&opts, state, k_weak, 1), 1);
    expect("a lone voice superframe is PARTIAL", g_quiet_level, DSD_CONTENT_EVIDENCE_PARTIAL);
    expect("held for two superframes", (int)g_quiet_hold_ms, (int)DSTAR_CONFIRM_PARTIAL_HOLD_MS);
    expect("the second voice superframe: one note", quiet_frame(&opts, state, k_weak, 1), 1);
    expect("the second voice superframe is PROVEN", g_quiet_level, DSD_CONTENT_EVIDENCE_PROVEN);
    quiet_notes_install(0);
    dstar_confirm_reset(state);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_state\n");
        return 1;
    }

    /* Nothing is believed until something checks out. */
    expect("fresh state is unconfirmed", dstar_confirm_is_confirmed(state), 0);

    /* One CRC-16 is enough. */
    run_frame(state, DSTAR_EVIDENCE_STRONG);
    expect("one strong frame confirms", dstar_confirm_is_confirmed(state), 1);

    /* Confirmation survives later superframes that prove nothing: a real call whose slow data
     * carries only filler is still decoding, and withdrawing the verdict mid-transmission
     * would rotate the hunt off live traffic -- the risk #391 names. */
    run_frame(state, 0);
    run_frame(state, 0);
    expect("confirmation survives empty frames", dstar_confirm_is_confirmed(state), 1);

    dstar_confirm_reset(state);
    expect("reset clears confirmation", dstar_confirm_is_confirmed(state), 0);

    /* One unchecked superframe is not enough; the second running is. */
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    expect("one weak frame is pending", dstar_confirm_is_confirmed(state), 0);
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    expect("two weak frames confirm", dstar_confirm_is_confirmed(state), 1);

    /* The two have to be consecutive. */
    dstar_confirm_reset(state);
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    run_frame(state, 0);
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    expect("a gap breaks the weak streak", dstar_confirm_is_confirmed(state), 0);
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    expect("the streak resumes from the gap", dstar_confirm_is_confirmed(state), 1);

    /* Reporting the same superframe twice is one frame's worth of evidence, not two --
     * otherwise a single false match would confirm itself. */
    dstar_confirm_reset(state);
    dstar_confirm_begin_frame(state);
    dstar_confirm_note_evidence(NULL, state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_note_evidence(NULL, state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_note_evidence(NULL, state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_end_frame(NULL, state);
    expect("weak evidence counts once per frame", dstar_confirm_is_confirmed(state), 0);

    /* A CRC arriving after the superframe was counted still confirms immediately: that is the
     * order the decoder produces them in, since the slow data is read last. */
    dstar_confirm_reset(state);
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_begin_frame(state);
    dstar_confirm_note_evidence(NULL, state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_note_evidence(NULL, state, DSTAR_EVIDENCE_STRONG);
    dstar_confirm_end_frame(NULL, state);
    expect("strong evidence overrides a pending streak", dstar_confirm_is_confirmed(state), 1);

    test_quiet_check_hears_the_frame_verdict(state);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    dstar_confirm_reset(NULL);
    dstar_confirm_begin_frame(NULL);
    dstar_confirm_note_evidence(NULL, NULL, DSTAR_EVIDENCE_STRONG);
    dstar_confirm_end_frame(NULL, NULL);
    expect("null state is not confirmed", dstar_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "DSTAR_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
