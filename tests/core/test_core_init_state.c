// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/state.h>

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>

#undef DSD_NEO_MAIN

#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    // Ensure initState explicitly resets this field even if caller pre-seeded it.
    state->rc2_context = state;
    initState(state);

    if (state->rc2_context != NULL) {
        fprintf(stderr, "expected rc2_context to be NULL after initState\n");
        // Avoid invalid free if initialization regresses.
        state->rc2_context = NULL;
        freeState(state);
        free(state);
        return 2;
    }

    for (int i = 0; i < 4; i++) {
        state->minbuf[i] = (float)(-10 - i);
        state->maxbuf[i] = (float)(10 + i);
    }
    state->midx = 1;
    dsd_state_invalidate_minmax_sums(state);
    dsd_state_push_minmax_window(state, 4, -20.0f, 20.0f);
    if (state->midx != 2 || state->minmax_sum_window != 4 || state->min != -13.75f || state->max != 13.75f) {
        fprintf(stderr, "min/max rolling window mismatch: midx=%d window=%d min=%.2f max=%.2f\n", state->midx,
                state->minmax_sum_window, (double)state->min, (double)state->max);
        freeState(state);
        free(state);
        return 3;
    }

    freeState(state);
    free(state);
    return 0;
}
