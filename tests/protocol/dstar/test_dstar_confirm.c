// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit coverage for the D-STAR transmission confirmation gate (issue #421).
 *
 * The gate decides whether the 1992 symbols a voice superframe consumes bought the hunt
 * profile anything. A CRC-16/X.25 -- the RF header, the header rebroadcast in slow data, or a
 * whole D-PRS record whose CRC matches (issue #800, tests/protocol/dstar/test_dstar_dprs.c) --
 * is proof by itself; a superframe that carries only filler has to repeat, because one
 * arriving at all means a second exact 24-symbol sync word was matched.
 *
 * Sibling of tests/protocol/provoice/test_provoice_confirm.c and
 * tests/protocol/nxdn/test_nxdn_confirm.c.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dstar_confirm.h"
#include "dstar_sd_state.h"

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

/* A superframe's verdict for a scan quiet check: PROVEN for an RF or slow-data header CRC; SYNC_ONLY (two superframes,
   840 ms) for a superframe that completes or continues a weak streak, whose two exact resync words confirm the audio but
   no content (issue #890); PARTIAL at the superframe's end (840 ms) when only its own sync stands behind it and the
   streak is short; nothing for an empty superframe, or with the setting off. A header CRC in the superframe leaves no
   PARTIAL. */
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
    expect("the second voice superframe is SYNC_ONLY", g_quiet_level, DSD_CONTENT_EVIDENCE_SYNC_ONLY);
    expect("SYNC_ONLY waits two superframes", (int)g_quiet_hold_ms, (int)DSTAR_CONFIRM_PARTIAL_HOLD_MS);
    expect("the third voice superframe: one note", quiet_frame(&opts, state, k_weak, 1), 1);
    expect("the third voice superframe is SYNC_ONLY", g_quiet_level, DSD_CONTENT_EVIDENCE_SYNC_ONLY);
    expect("a resync-confirmed stream still plays", dstar_confirm_is_confirmed(state), 1);
    quiet_notes_install(0);
    dstar_confirm_reset(state);
}

/* Issue #890: a transmission only its resync words confirmed is no checked content to the -Y step, whatever the
   skip-silent setting: the scan_y_content counter, which holds a -Y row for -t, moves only for a CRC. The lone first
   superframe still leaves an open verdict (PARTIAL), which the header repeat in slow data can complete. */
static void
test_sync_only_streak_is_no_y_content(dsd_state* state) {
    static dsd_opts opts;
    for (int skip_silent = 0; skip_silent <= 1; skip_silent++) {
        DSD_MEMSET(&opts, 0, sizeof opts);
        opts.scan_skip_silent = skip_silent;
        quiet_notes_install(1);
        dstar_confirm_reset(state);
        const uint32_t content = state->scan_y_content_seq;
        const uint32_t partial = state->scan_y_partial_seq;
        static const int k_weak[] = {DSTAR_EVIDENCE_WEAK};
        for (int i = 0; i < 4; i++) {
            (void)quiet_frame(&opts, state, k_weak, 1);
        }
        expect(skip_silent ? "skip-silent on: resync-confirmed: confirmed"
                           : "skip-silent off: resync-confirmed: confirmed",
               dstar_confirm_is_confirmed(state), 1);
        expect(skip_silent ? "skip-silent on: resync-only superframes are no -Y content"
                           : "skip-silent off: resync-only superframes are no -Y content",
               (int)(state->scan_y_content_seq - content), 0);
        expect(skip_silent ? "skip-silent on: only the first superframe leaves an open verdict"
                           : "skip-silent off: only the first superframe leaves an open verdict",
               (int)(state->scan_y_partial_seq - partial), 1);
        static const int k_strong[] = {DSTAR_EVIDENCE_STRONG};
        (void)quiet_frame(&opts, state, k_strong, 1);
        expect(skip_silent ? "skip-silent on: a header CRC is -Y content"
                           : "skip-silent off: a header CRC is -Y content",
               (int)(state->scan_y_content_seq - content), 1);
    }
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

    /* The reset forgets the transmission whole: what its slow data was assembling too, and the call its header named
       (issue #800 2.10); the next transmission is another one. */
    dstar_sd_state* sd = dstar_sd_st(state);
    if (!sd) {
        DSD_FPRINTF(stderr, "alloc-failed: slow-data state\n");
        free(state);
        return 1;
    }
    sd->dprs.collecting = 1U;
    sd->dprs.len = 12U;
    sd->dprs.bind.call_epoch = 7U;
    sd->text.places = 3U;
    state->dstar_tx_call_epoch = 7U;
    const uint32_t tx = state->dstar_tx_seq;
    dstar_confirm_reset(state);
    expect("reset clears confirmation", dstar_confirm_is_confirmed(state), 0);
    expect("reset forgets the D-PRS record", sd->dprs.collecting, 0);
    expect("reset empties the D-PRS record", sd->dprs.len, 0);
    expect("reset unbinds the D-PRS record", sd->dprs.bind.call_epoch != 0U, 0);
    expect("reset forgets the text message", sd->text.places, 0);
    expect("reset forgets the named call", state->dstar_tx_call_epoch != 0U, 0);
    expect("reset begins another transmission", state->dstar_tx_seq != tx, 1);

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
    test_sync_only_streak_is_no_y_content(state);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    dstar_confirm_reset(NULL);
    dstar_confirm_begin_frame(NULL);
    dstar_confirm_note_evidence(NULL, NULL, DSTAR_EVIDENCE_STRONG);
    dstar_confirm_end_frame(NULL, NULL);
    expect("null state is not confirmed", dstar_confirm_is_confirmed(NULL), 0);

    dsd_state_ext_free_all(state);
    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "DSTAR_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
