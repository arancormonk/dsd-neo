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

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    nxdn_confirm_reset(NULL);
    nxdn_confirm_begin_frame(NULL);
    nxdn_confirm_note_evidence(NULL, NXDN_EVIDENCE_STRONG);
    nxdn_confirm_end_frame(NULL);
    expect("null state is not confirmed", nxdn_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "NXDN_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
