// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit coverage for the ProVoice transmission confirmation gate (issue #421).
 *
 * The gate decides whether the 736 dibits a frame consumes bought the hunt profile anything.
 * Nothing inside a ProVoice frame can fail a check, so the only evidence is that a frame
 * arrived at all -- behind its own exact 32-symbol sync word -- and it has to repeat. The
 * strong tier has no producer in the decoder today and is exercised here so the module keeps
 * reading the same as its siblings.
 *
 * Sibling of tests/protocol/dstar/test_dstar_confirm.c and
 * tests/protocol/nxdn/test_nxdn_confirm.c.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"
#include "provoice_confirm.h"

static int g_failures;

static void
expect(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

/** @brief One frame that reported @p evidence, or nothing when @p evidence is 0. */
static void
run_frame(dsd_state* state, int evidence) {
    provoice_confirm_begin_frame(state);
    if (evidence != 0) {
        provoice_confirm_note_evidence(state, (provoice_evidence)evidence);
    }
    provoice_confirm_end_frame(state);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_state\n");
        return 1;
    }

    /* Nothing is believed until something checks out. */
    expect("fresh state is unconfirmed", provoice_confirm_is_confirmed(state), 0);

    /* One CRC-16 is enough. */
    run_frame(state, PROVOICE_EVIDENCE_STRONG);
    expect("one strong frame confirms", provoice_confirm_is_confirmed(state), 1);

    /* Confirmation survives later frames that prove nothing: a real call is still decoding,
     * and withdrawing the verdict mid-transmission would rotate the hunt off live traffic --
     * the risk #391 names. */
    run_frame(state, 0);
    run_frame(state, 0);
    expect("confirmation survives empty frames", provoice_confirm_is_confirmed(state), 1);

    provoice_confirm_reset(state);
    expect("reset clears confirmation", provoice_confirm_is_confirmed(state), 0);

    /* One frame is not enough; the second running is. */
    run_frame(state, PROVOICE_EVIDENCE_WEAK);
    expect("one weak frame is pending", provoice_confirm_is_confirmed(state), 0);
    run_frame(state, PROVOICE_EVIDENCE_WEAK);
    expect("two weak frames confirm", provoice_confirm_is_confirmed(state), 1);

    /* The two have to be consecutive. */
    provoice_confirm_reset(state);
    run_frame(state, PROVOICE_EVIDENCE_WEAK);
    run_frame(state, 0);
    run_frame(state, PROVOICE_EVIDENCE_WEAK);
    expect("a gap breaks the weak streak", provoice_confirm_is_confirmed(state), 0);
    run_frame(state, PROVOICE_EVIDENCE_WEAK);
    expect("the streak resumes from the gap", provoice_confirm_is_confirmed(state), 1);

    /* Reporting the same frame twice is one frame's worth of evidence, not two -- otherwise
     * a single false match would confirm itself. */
    provoice_confirm_reset(state);
    provoice_confirm_begin_frame(state);
    provoice_confirm_note_evidence(state, PROVOICE_EVIDENCE_WEAK);
    provoice_confirm_note_evidence(state, PROVOICE_EVIDENCE_WEAK);
    provoice_confirm_note_evidence(state, PROVOICE_EVIDENCE_WEAK);
    provoice_confirm_end_frame(state);
    expect("weak evidence counts once per frame", provoice_confirm_is_confirmed(state), 0);

    /* Strong evidence still confirms immediately where a frame has already been counted. */
    provoice_confirm_reset(state);
    run_frame(state, PROVOICE_EVIDENCE_WEAK);
    provoice_confirm_begin_frame(state);
    provoice_confirm_note_evidence(state, PROVOICE_EVIDENCE_WEAK);
    provoice_confirm_note_evidence(state, PROVOICE_EVIDENCE_STRONG);
    provoice_confirm_end_frame(state);
    expect("strong evidence overrides a pending streak", provoice_confirm_is_confirmed(state), 1);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    provoice_confirm_reset(NULL);
    provoice_confirm_begin_frame(NULL);
    provoice_confirm_note_evidence(NULL, PROVOICE_EVIDENCE_STRONG);
    provoice_confirm_end_frame(NULL);
    expect("null state is not confirmed", provoice_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "PROVOICE_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
