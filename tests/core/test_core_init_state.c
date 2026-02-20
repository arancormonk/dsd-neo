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

    freeState(state);
    free(state);
    return 0;
}
