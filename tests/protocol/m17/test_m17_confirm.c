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

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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
        m17_confirm_note_evidence(state, (m17_evidence)evidence);
    }
    m17_confirm_end_frame(state);
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
    m17_confirm_note_evidence(state, M17_EVIDENCE_WEAK);
    m17_confirm_note_evidence(state, M17_EVIDENCE_WEAK);
    m17_confirm_note_evidence(state, M17_EVIDENCE_WEAK);
    m17_confirm_end_frame(state);
    expect("weak evidence counts once per frame", m17_confirm_is_confirmed(state), 0);

    /* A CRC arriving after a clean LICH in the same frame still confirms immediately. */
    m17_confirm_reset(state);
    run_frame(state, M17_EVIDENCE_WEAK);
    m17_confirm_begin_frame(state);
    m17_confirm_note_evidence(state, M17_EVIDENCE_WEAK);
    m17_confirm_note_evidence(state, M17_EVIDENCE_STRONG);
    m17_confirm_end_frame(state);
    expect("strong evidence overrides a pending streak", m17_confirm_is_confirmed(state), 1);

    /* NULL is tolerated: the gate is called from paths that run before state exists. */
    m17_confirm_reset(NULL);
    m17_confirm_begin_frame(NULL);
    m17_confirm_note_evidence(NULL, M17_EVIDENCE_STRONG);
    m17_confirm_end_frame(NULL);
    expect("null state is not confirmed", m17_confirm_is_confirmed(NULL), 0);

    free(state);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "M17_CONFIRM: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
