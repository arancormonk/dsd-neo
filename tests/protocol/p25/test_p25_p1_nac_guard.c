// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 NAC guard tests.
 *
 * Verifies that corrupted NID values (BCH artifacts 0x0 and 0xFFF) do not
 * overwrite a known-good NAC, while valid NAC updates, BCH failure paths,
 * p2_cc hardset guards, and first-NAC-after-init all behave correctly.
 *
 * The test simulates the NID processing logic from dispatch_p25p1.c inline
 * (we cannot call the full dispatcher because it requires a dibit stream).
 */

#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got 0x%X want 0x%X\n", tag, got, want);
        return 1;
    }
    fprintf(stderr, "PASS %s\n", tag);
    return 0;
}

static int
expect_eq_uint(const char* tag, unsigned int got, unsigned int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", tag, got, want);
        return 1;
    }
    fprintf(stderr, "PASS %s\n", tag);
    return 0;
}

static int
expect_eq_ull(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got 0x%llX want 0x%llX\n", tag, got, want);
        return 1;
    }
    fprintf(stderr, "PASS %s\n", tag);
    return 0;
}

/*
 * Inline simulation of the FIXED NID processing logic from dispatch_p25p1.c.
 * The guard rejects new_nac == 0 and new_nac == 0xFFF as BCH artifacts.
 */
static void
simulate_nid_processing(dsd_state* state, int new_nac, int check_result) {
    if (check_result == 1) {
        if (new_nac != state->nac) {
            if (new_nac != 0 && new_nac != 0xFFF) {
                state->nac = new_nac;
            }
            if (state->p2_hardset == 0 && new_nac != 0 && new_nac != 0xFFF) {
                state->p2_cc = new_nac;
            }
            state->debug_header_errors++;
        }
    }
}

int
main(void) {
    int rc = 0;
    dsd_state state;

    /* ---------------------------------------------------------------
     * Corrupted NID rejection
     * --------------------------------------------------------------- */

    /* NAC zeroed by corrupted NID (new_nac = 0x0) */
    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    simulate_nid_processing(&state, 0x0, 1);
    rc |= expect_eq_int("nac_preserved_after_zero", state.nac, 0x2AA);

    /* NAC set to 0xFFF by corrupted NID */
    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    simulate_nid_processing(&state, 0xFFF, 1);
    rc |= expect_eq_int("nac_preserved_after_0xFFF", state.nac, 0x2AA);

    /* p2_cc already guarded — existing guard prevents overwrite */
    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    state.p2_cc = 0x2AA;
    state.p2_hardset = 0;
    simulate_nid_processing(&state, 0x0, 1);
    rc |= expect_eq_ull("p2_cc_preserved_after_zero", state.p2_cc, 0x2AA);

    /* ---------------------------------------------------------------
     * Valid NAC updates
     * --------------------------------------------------------------- */

    {
        const int valid_nacs[] = {0x001, 0x1B5, 0x2AA, 0xFFE};
        const int num_nacs = sizeof(valid_nacs) / sizeof(valid_nacs[0]);

        for (int i = 0; i < num_nacs; i++) {
            int new_nac = valid_nacs[i];
            int initial_nac = (new_nac == 0x2AA) ? 0x1B5 : 0x2AA;

            memset(&state, 0, sizeof(state));
            state.nac = initial_nac;
            state.p2_hardset = 0;
            state.p2_cc = initial_nac;

            simulate_nid_processing(&state, new_nac, 1);

            char tag[128];
            snprintf(tag, sizeof(tag), "valid_nac_update_0x%X_to_0x%X", initial_nac, new_nac);
            rc |= expect_eq_int(tag, state.nac, new_nac);

            snprintf(tag, sizeof(tag), "valid_p2cc_update_0x%X_to_0x%X", initial_nac, new_nac);
            rc |= expect_eq_ull(tag, state.p2_cc, (unsigned long long)new_nac);
        }
    }

    /* Same NAC no-op — identical NAC short-circuits */
    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    state.debug_header_errors = 0;
    simulate_nid_processing(&state, 0x2AA, 1);
    rc |= expect_eq_int("same_nac_noop", state.nac, 0x2AA);
    rc |= expect_eq_uint("same_nac_no_error_inc", state.debug_header_errors, 0);

    /* ---------------------------------------------------------------
     * BCH failure path — check_result != 1 leaves state unchanged
     * --------------------------------------------------------------- */

    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    simulate_nid_processing(&state, 0x0, 0);
    rc |= expect_eq_int("bch_fail_check0_nac_preserved", state.nac, 0x2AA);

    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    simulate_nid_processing(&state, 0x0, -1);
    rc |= expect_eq_int("bch_fail_checkn1_nac_preserved", state.nac, 0x2AA);

    /* ---------------------------------------------------------------
     * p2_cc hardset guard — p2_cc preserved when p2_hardset == 1
     * --------------------------------------------------------------- */

    memset(&state, 0, sizeof(state));
    state.nac = 0x2AA;
    state.p2_hardset = 1;
    state.p2_cc = 0x2AA;
    simulate_nid_processing(&state, 0x1B5, 1);
    rc |= expect_eq_ull("p2cc_hardset_preserved", state.p2_cc, 0x2AA);

    /* ---------------------------------------------------------------
     * First NAC after engine init — state.nac == 0 accepts first valid NAC
     * --------------------------------------------------------------- */

    memset(&state, 0, sizeof(state));
    state.nac = 0;
    simulate_nid_processing(&state, 0x2AA, 1);
    rc |= expect_eq_int("first_nac_after_init", state.nac, 0x2AA);

    return rc;
}
