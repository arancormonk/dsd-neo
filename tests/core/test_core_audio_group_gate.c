// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test for talkgroup/whitelist/TG-hold audio gating.
 *
 * Ensures dual-slot gating keeps allowed traffic audible while muting blocked
 * or non-held traffic.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
set_group(dsd_state* st, unsigned int idx, unsigned long tg, const char* mode, const char* name) {
    st->group_array[idx].groupNumber = tg;
    snprintf(st->group_array[idx].groupMode, sizeof(st->group_array[idx].groupMode), "%s", mode);
    snprintf(st->group_array[idx].groupName, sizeof(st->group_array[idx].groupName), "%s", name);
}

int
main(void) {
    int rc = 0;

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));

    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        fprintf(stderr, "alloc-failed: dsd_state\n");
        return 1;
    }

    // Case 1: Explicit block list on slot R while slot L remains allowed.
    st->group_tally = 2;
    set_group(st, 0, 100UL, "A", "ALLOW");
    set_group(st, 1, 200UL, "B", "BLOCK");
    {
        int outL = -1, outR = -1;
        rc |= expect_eq("case1-ret", dsd_audio_group_gate_dual(&opts, st, 100UL, 200UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case1-outL", outL, 0);
        rc |= expect_eq("case1-outR", outR, 1);
    }

    // Case 2: Allow-list mode defaults unknown TGs to blocked.
    memset(&opts, 0, sizeof(opts));
    memset(st, 0, sizeof(*st));
    opts.trunk_use_allow_list = 1;
    st->group_tally = 1;
    set_group(st, 0, 300UL, "A", "ONLY");
    {
        int outL = -1, outR = -1;
        rc |= expect_eq("case2-ret", dsd_audio_group_gate_dual(&opts, st, 300UL, 301UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case2-outL", outL, 0);
        rc |= expect_eq("case2-outR", outR, 1);
    }

    // Case 2b: "DE" lockout mode should be treated as blocked by audio gate.
    memset(&opts, 0, sizeof(opts));
    memset(st, 0, sizeof(*st));
    st->group_tally = 1;
    set_group(st, 0, 310UL, "DE", "ENC-LOCKOUT");
    {
        int outL = -1;
        rc |= expect_eq("case2b-ret", dsd_audio_group_gate_mono(&opts, st, 310UL, 0, &outL), 0);
        rc |= expect_eq("case2b-outL", outL, 1);
    }

    // Case 3: TG hold mutes non-matching slot and force-unmutes matching slot.
    memset(&opts, 0, sizeof(opts));
    memset(st, 0, sizeof(*st));
    st->group_tally = 2;
    set_group(st, 0, 400UL, "A", "LEFT");
    set_group(st, 1, 401UL, "B", "RIGHT-BLOCKED");
    st->tg_hold = 401U;
    {
        int outL = -1, outR = -1;
        rc |= expect_eq("case3-ret", dsd_audio_group_gate_dual(&opts, st, 400UL, 401UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case3-outL", outL, 1);
        rc |= expect_eq("case3-outR", outR, 0);
    }

    // Defensive API contract checks.
    {
        int out = 0;
        rc |= expect_eq("null-mono", dsd_audio_group_gate_mono(NULL, st, 0UL, 0, &out), -1);
        rc |= expect_eq("null-dual", dsd_audio_group_gate_dual(&opts, st, 0UL, 0UL, 0, 0, NULL, &out), -1);
    }

    if (rc == 0) {
        printf("CORE_AUDIO_GROUP_GATE: OK\n");
    }
    free(st);
    return rc;
}
