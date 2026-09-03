// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Seeding rules for the user-supplied P25 band plan (dsd_state_p25_bandplan_seed):
 * only empty slots are filled, an over-the-air entry is never touched, a row that
 * names a WACN/SYS waits for that identity and then beats a global row, and the
 * explicit FDMA/TDMA bit follows the table the row landed in.
 */

#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
add_row(dsd_state* state, int iden, int is_tdma, long base_5hz, int spac, int type, unsigned long long wacn,
        unsigned long long sysid) {
    p25_bandplan_row_t* row = &state->p25_bandplan_rows[state->p25_bandplan_row_count++];
    DSD_MEMSET(row, 0, sizeof *row);
    row->iden = (uint8_t)iden;
    row->is_tdma = (uint8_t)is_tdma;
    row->entry.base_freq = base_5hz;
    row->entry.chan_spac = spac;
    row->entry.chan_type = type;
    row->entry.trust = 1;
    row->entry.populated = 1;
    row->entry.wacn = wacn;
    row->entry.sysid = sysid;
}

static int
test_seed_fills_only_empty_slots(void) {
    int failed = 0;
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    // An over-the-air entry already owns fdma[1]; the plan must leave it alone.
    state->p25_iden_fdma[1].populated = 1;
    state->p25_iden_fdma[1].base_freq = 999;
    state->p25_iden_fdma[1].chan_spac = 50;
    state->p25_iden_fdma[1].trust = 2;
    state->p25_chan_tdma_explicit[1] = 1;

    add_row(state, 1, 0, 170201250L, 50, 1, 0ULL, 0ULL);
    add_row(state, 3, 0, 170201250L, 50, 1, 0ULL, 0ULL);
    add_row(state, 3, 1, 152401250L, 50, 3, 0ULL, 0ULL);

    if (dsd_state_p25_bandplan_seed(state) != 2) {
        DSD_FPRINTF(stderr, "seed: expected 2 slots filled\n");
        failed = 1;
    }
    if (state->p25_iden_fdma[1].base_freq != 999 || state->p25_iden_fdma[1].trust != 2) {
        DSD_FPRINTF(stderr, "seed: overwrote an OTA entry\n");
        failed = 1;
    }
    if (!state->p25_iden_fdma[3].populated || state->p25_iden_fdma[3].trust != 1
        || state->p25_iden_fdma[3].base_freq != 170201250L) {
        DSD_FPRINTF(stderr, "seed: fdma[3] not seeded\n");
        failed = 1;
    }
    if (!state->p25_iden_tdma[3].populated || state->p25_iden_tdma[3].chan_type != 3) {
        DSD_FPRINTF(stderr, "seed: tdma[3] not seeded\n");
        failed = 1;
    }
    if (state->p25_chan_tdma_explicit[3] != 3 || state->p25_chan_tdma_explicit[1] != 1) {
        DSD_FPRINTF(stderr, "seed: explicit bits wrong (%u, %u)\n", state->p25_chan_tdma_explicit[3],
                    state->p25_chan_tdma_explicit[1]);
        failed = 1;
    }
    // Seeding again is a no-op.
    if (dsd_state_p25_bandplan_seed(state) != 0) {
        DSD_FPRINTF(stderr, "seed: second pass filled something\n");
        failed = 1;
    }
    free(state);
    return failed;
}

static int
test_seed_system_rows_follow_identity(void) {
    int failed = 0;
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    add_row(state, 0, 0, 100, 50, 1, 0ULL, 0ULL);           // global
    add_row(state, 0, 0, 200, 50, 1, 0xBEE00ULL, 0x3A1ULL); // system A
    add_row(state, 2, 0, 300, 50, 1, 0xBEE00ULL, 0x3A1ULL); // system A
    add_row(state, 4, 0, 400, 50, 1, 0x11111ULL, 0x222ULL); // system B

    // Identity unknown: only the global row seeds.
    if (dsd_state_p25_bandplan_seed(state) != 1 || state->p25_iden_fdma[0].base_freq != 100
        || state->p25_iden_fdma[2].populated || state->p25_iden_fdma[4].populated) {
        DSD_FPRINTF(stderr, "seed: identity unknown seeded system rows\n");
        failed = 1;
    }

    // Identity A learned: the system row replaces the seeded global row on iden 0, iden 2 fills,
    // system B stays out.
    state->p2_wacn = 0xBEE00ULL;
    state->p2_sysid = 0x3A1ULL;
    if (dsd_state_p25_bandplan_seed(state) != 2) {
        DSD_FPRINTF(stderr, "seed: identity A expected 2 changes\n");
        failed = 1;
    }
    if (state->p25_iden_fdma[0].base_freq != 200 || state->p25_iden_fdma[0].wacn != 0xBEE00ULL) {
        DSD_FPRINTF(stderr, "seed: system row did not replace the global row\n");
        failed = 1;
    }
    if (state->p25_iden_fdma[2].base_freq != 300 || state->p25_iden_fdma[4].populated) {
        DSD_FPRINTF(stderr, "seed: identity A rows wrong\n");
        failed = 1;
    }

    // An OTA entry on iden 0 with all-zero provenance but different numbers is not "our" global
    // row, so the system row must not replace it.
    DSD_MEMSET(state->p25_iden_fdma, 0, sizeof state->p25_iden_fdma);
    DSD_MEMSET(state->p25_chan_tdma_explicit, 0, sizeof state->p25_chan_tdma_explicit);
    state->p25_iden_fdma[0].populated = 1;
    state->p25_iden_fdma[0].base_freq = 100;
    state->p25_iden_fdma[0].chan_spac = 40;
    state->p25_iden_fdma[0].trust = 1;
    if (dsd_state_p25_bandplan_seed(state) != 1 || state->p25_iden_fdma[0].base_freq != 100
        || state->p25_iden_fdma[0].chan_spac != 40) {
        DSD_FPRINTF(stderr, "seed: replaced an OTA entry that merely looked global\n");
        failed = 1;
    }
    free(state);
    return failed;
}

static int
test_seed_with_empty_plan_is_noop(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;
    if (dsd_state_p25_bandplan_seed(state) != 0 || dsd_state_p25_bandplan_seed(NULL) != 0) {
        DSD_FPRINTF(stderr, "seed: empty plan or NULL state changed something\n");
        failed = 1;
    }
    for (int i = 0; i < 16; i++) {
        if (state->p25_iden_fdma[i].populated || state->p25_iden_tdma[i].populated) {
            failed = 1;
        }
    }
    free(state);
    return failed;
}

int
main(void) {
    if (test_seed_fills_only_empty_slots() != 0) {
        return 1;
    }
    if (test_seed_system_rows_follow_identity() != 0) {
        return 1;
    }
    if (test_seed_with_empty_plan_is_noop() != 0) {
        return 1;
    }
    return 0;
}
