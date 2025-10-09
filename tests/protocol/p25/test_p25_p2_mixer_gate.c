// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate P25 Phase 2 stereo mixer gating uses per-slot gates and does not
 * cross-mute the opposite slot.
 */

#include <stdio.h>
#include <string.h>

// Avoid pulling main() from dsd.h in test binary
#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#undef main

// Mixer helper under test (declared in dsd_audio2.c)
int dsd_p25p2_mixer_gate(const dsd_state* state, int* encL, int* encR);

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    dsd_state st;
    memset(&st, 0, sizeof(st));

    int encL = -1, encR = -1;

    // Case A: slot1 muted (enc), slot2 clear → encL=1, encR=0
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 1;
    rc |= expect_eq("gate ret A", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("A encL", encL, 1);
    rc |= expect_eq("A encR", encR, 0);

    // Case B: slot1 clear, slot2 muted → encL=0, encR=1
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0;
    rc |= expect_eq("gate ret B", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("B encL", encL, 0);
    rc |= expect_eq("B encR", encR, 1);

    // Case C: both clear → encL=0, encR=0
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;
    rc |= expect_eq("gate ret C", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("C encL", encL, 0);
    rc |= expect_eq("C encR", encR, 0);

    // Case D: both muted → encL=1, encR=1
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;
    rc |= expect_eq("gate ret D", dsd_p25p2_mixer_gate(&st, &encL, &encR), 0);
    rc |= expect_eq("D encL", encL, 1);
    rc |= expect_eq("D encR", encR, 1);

    return rc;
}
