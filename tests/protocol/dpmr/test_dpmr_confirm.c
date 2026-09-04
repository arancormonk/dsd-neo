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

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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
    dpmr_confirm_end_frame(state);
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

    /* Noting weak twice inside one frame is one frame's worth of evidence, not two --
     * otherwise a single lucky frame would confirm itself. The voice path cannot reach
     * this (it reports both halves as one strong), but the gate must not depend on that. */
    dpmr_confirm_reset(state);
    dpmr_confirm_begin_frame(state);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_end_frame(state);
    expect("weak evidence counts once per frame", dpmr_confirm_is_confirmed(state), 0);

    /* Strong evidence after a weak one in the same frame still confirms immediately. */
    dpmr_confirm_reset(state);
    run_frame(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_begin_frame(state);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_WEAK);
    dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_STRONG);
    dpmr_confirm_end_frame(state);
    expect("strong evidence overrides a pending streak", dpmr_confirm_is_confirmed(state), 1);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    dpmr_confirm_reset(NULL);
    dpmr_confirm_begin_frame(NULL);
    dpmr_confirm_note_evidence(NULL, DPMR_EVIDENCE_STRONG);
    dpmr_confirm_end_frame(NULL);
    expect("null state is not confirmed", dpmr_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "DPMR_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
