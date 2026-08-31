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

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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
        dstar_confirm_note_evidence(state, (dstar_evidence)evidence);
    }
    dstar_confirm_end_frame(state);
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
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_end_frame(state);
    expect("weak evidence counts once per frame", dstar_confirm_is_confirmed(state), 0);

    /* A CRC arriving after the superframe was counted still confirms immediately: that is the
     * order the decoder produces them in, since the slow data is read last. */
    dstar_confirm_reset(state);
    run_frame(state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_begin_frame(state);
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_WEAK);
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_STRONG);
    dstar_confirm_end_frame(state);
    expect("strong evidence overrides a pending streak", dstar_confirm_is_confirmed(state), 1);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    dstar_confirm_reset(NULL);
    dstar_confirm_begin_frame(NULL);
    dstar_confirm_note_evidence(NULL, DSTAR_EVIDENCE_STRONG);
    dstar_confirm_end_frame(NULL);
    expect("null state is not confirmed", dstar_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "DSTAR_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
