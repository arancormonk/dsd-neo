// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Seeding rules for the user-supplied P25 band plan (dsd_state_p25_bandplan_seed):
 * typed entries replace untyped fallbacks, scoped seeds replace global seeds,
 * and typed over-the-air entries survive. TDMA suppresses untyped FDMA before
 * any fill, while the stored row remains available for another system.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
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

static int
check_value(const char* tag, long got, long want) {
    if (got == want) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
    return 1;
}

static int
test_typed_seed_replaces_untyped(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(state);
    int rc = 0;
    for (int order = 0; order < 2; order++) {
        for (int scoped_fdma = 0; scoped_fdma < 2; scoped_fdma++) {
            for (int seeded = 0; seeded < 2; seeded++) {
                DSD_MEMSET(state, 0, sizeof *state);
                state->p2_wacn = 1;
                state->p2_sysid = 1;
                add_row(state, 4, 0, 100, 100, 1, 0, 0);
                state->p25_iden_fdma[4] = state->p25_bandplan_rows[0].entry;
                state->p25_iden_fdma[4].flags = P25_IDEN_F_UNTYPED | (seeded ? P25_IDEN_F_SEEDED : 0);
                state->p25_bandplan_row_count = 0;
                for (int row = 0; row < 2; row++) {
                    const int tdma = row == order;
                    const int scoped = tdma ? !scoped_fdma : scoped_fdma;
                    add_row(state, 4, tdma, tdma ? 300 : 200, 100, tdma ? 3 : 1, scoped, scoped);
                }
                state->trunk_chan_map[0x4119] = 999;
                (void)dsd_state_p25_bandplan_seed(state);
                rc |= check_value("T11 typed row replaces fallback", state->p25_iden_fdma[4].base_freq, 200);
                rc |= check_value("T11 both typed", state->p25_chan_tdma_explicit[4], 3);
                rc |= check_value("T11 replacement purges map", state->trunk_chan_map[0x4119], 0);
            }
        }
    }
    DSD_MEMSET(state->p25_iden_tdma, 0, sizeof state->p25_iden_tdma);
    // A global untyped fallback (the loop left a system's seed, which a global row must not displace).
    state->p25_iden_fdma[4].flags |= P25_IDEN_F_UNTYPED;
    state->p25_iden_fdma[4].wacn = 0ULL;
    state->p25_iden_fdma[4].sysid = 0ULL;
    state->p25_bandplan_row_count = 0;
    add_row(state, 4, 0, 400, 100, 1, 0, 0);
    (void)dsd_state_p25_bandplan_seed(state);
    rc |= check_value("T11 typed row alone replaces fallback", state->p25_iden_fdma[4].base_freq, 400);
    state->p25_iden_fdma[4].flags |= P25_IDEN_F_UNTYPED;
    state->p25_bandplan_row_count = 0;
    add_row(state, 4, 1, 300, 100, 3, 0, 0);
    (void)dsd_state_p25_bandplan_seed(state);
    rc |= check_value("T11 TDMA alone drops fallback", state->p25_iden_fdma[4].populated, 0);
    free(state);
    return rc;
}

static int
test_untyped_rows_wait_for_tdma(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(state);
    int rc = 0;
    for (int order = 0; order < 2; order++) {
        for (int scoped_fdma = 0; scoped_fdma < 2; scoped_fdma++) {
            DSD_MEMSET(state, 0, sizeof *state);
            state->p2_wacn = state->p2_sysid = 1;
            for (int row = 0; row < 2; row++) {
                const int tdma = row == order;
                const int scoped = tdma ? !scoped_fdma : scoped_fdma;
                add_row(state, 4, tdma, 100, 100, tdma ? 3 : 1, scoped, scoped);
                if (!tdma) {
                    state->p25_bandplan_rows[row].entry.flags = P25_IDEN_F_UNTYPED;
                }
            }
            state->trunk_chan_map[0x4119] = 12345;
            (void)dsd_state_p25_bandplan_seed(state);
            rc |= check_value("T11 r5 untyped row suppressed", state->p25_iden_fdma[4].populated, 0);
            rc |= check_value("T11 r5 fill keeps user map", state->trunk_chan_map[0x4119], 12345);
            rc |= check_value("T11 r5 stored rows survive", state->p25_bandplan_row_count, 2);
            if (!scoped_fdma) {
                (void)p25_update_system_identity(state, 2, 2);
                rc |= check_value("T11 r5 other system fallback", state->p25_iden_fdma[4].populated, 1);
            }
        }
    }
    free(state);
    return rc;
}

static int
test_seed_cache_and_origin(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    assert(state && opts);
    int rc = 0;
    for (int iden = 4; iden <= 15; iden += 11) {
        DSD_MEMSET(state, 0, sizeof *state);
        add_row(state, iden, 0, 769006250 / 5, 100, 1, 0, 0);
        add_row(state, iden, 0, 850000000 / 5, 50, 1, 1, 1);
        const int ch = (iden << 12) | 281;
        state->trunk_chan_map[ch] = 111;
        (void)dsd_state_p25_bandplan_seed(state);
        rc |= check_value("T12 seed fill keeps user map", state->trunk_chan_map[ch], 111);
        state->trunk_chan_map[ch] = 0;
        rc |= check_value("T12 global frequency", process_channel_to_freq(opts, state, ch), 772518750L);
        state->trunk_chan_map[0x3001] = 222;
        state->trunk_chan_map[0xFFFE] = 333;
        (void)p25_update_system_identity(state, 1, 1);
        rc |= check_value("T12 scoped frequency", process_channel_to_freq(opts, state, ch), 851756250L);
        rc |= check_value("T12 unrelated cache", state->trunk_chan_map[0x3001], 222);
        rc |= check_value("T12 last channel clamp", state->trunk_chan_map[0xFFFE], iden == 15 ? 0 : 333);
    }
    for (int offset_units = 0; offset_units < 2; offset_units++) {
        DSD_MEMSET(state, 0, sizeof *state);
        add_row(state, 4, 0, 769006250 / 5, 100, 1, 0, 0);
        add_row(state, 4, 0, 850000000 / 5, 50, 1, 1, 1);
        state->p25_bandplan_rows[0].entry.trans_off = 80;
        state->p25_iden_fdma[4] = state->p25_bandplan_rows[0].entry;
        state->p25_iden_fdma[4].flags = offset_units ? P25_IDEN_F_OFF_SPACING : 0;
        (void)p25_update_system_identity(state, 1, 1);
        rc |= check_value("T13 OTA matching global numbers stays", state->p25_iden_fdma[4].base_freq, 769006250 / 5);
        rc |= check_value("T13 offset units preserved", state->p25_iden_fdma[4].flags,
                          offset_units ? P25_IDEN_F_OFF_SPACING : 0);
    }
    free(opts);
    free(state);
    return rc;
}

/*
 * Among seeded rows, scope alone decides, whether or not a row gives a type: a row with an empty `type` cell that
 * names this system takes over the slot a typed global row seeded, and a typed global row never displaces it.
 */
static int
test_seed_scope_beats_typedness(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    add_row(state, 2, 0, 170201250L, 50, 1, 0ULL, 0ULL);
    add_row(state, 2, 0, 170401250L, 50, 1, 0xBEE00ULL, 0x3A1ULL);
    state->p25_bandplan_rows[1].entry.flags = P25_IDEN_F_UNTYPED;
    (void)dsd_state_p25_bandplan_seed(state);
    int rc = check_value("scope global seed first", state->p25_iden_fdma[2].base_freq, 170201250L);
    state->p2_wacn = 0xBEE00ULL;
    state->p2_sysid = 0x3A1ULL;
    (void)dsd_state_p25_bandplan_seed(state);
    rc |= check_value("scope untyped system row wins", state->p25_iden_fdma[2].base_freq, 170401250L);
    rc |=
        check_value("scope seed stays untyped", state->p25_iden_fdma[2].flags & P25_IDEN_F_UNTYPED, P25_IDEN_F_UNTYPED);

    // Identity known from the start: the system's untyped row seeds first, and the typed global row leaves it.
    DSD_MEMSET(state->p25_iden_fdma, 0, sizeof state->p25_iden_fdma);
    DSD_MEMSET(state->p25_chan_tdma_explicit, 0, sizeof state->p25_chan_tdma_explicit);
    (void)dsd_state_p25_bandplan_seed(state);
    rc |= check_value("scope typed global does not displace", state->p25_iden_fdma[2].base_freq, 170401250L);
    free(state);
    return rc;
}

int
main(void) {
    int rc = test_typed_seed_replaces_untyped();
    rc |= test_untyped_rows_wait_for_tdma();
    rc |= test_seed_cache_and_origin();
    rc |= test_seed_scope_beats_typedness();
    if (test_seed_fills_only_empty_slots() != 0) {
        return 1;
    }
    if (test_seed_system_rows_follow_identity() != 0) {
        return 1;
    }
    if (test_seed_with_empty_plan_is_noop() != 0) {
        return 1;
    }
    return rc;
}
