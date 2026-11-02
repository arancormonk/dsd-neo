// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 next CC candidate edge cases.
 * - Returns 0 when empty list
 * - Returns 0 when only current CC, zeros, or generic neighbors are present
 * - The previous candidate (issue #666) refuses the same, and walks current-site candidates backward
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Stubs

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    long out = -1;
    // Empty list
    rc |= expect_eq_int("empty", p25_cc_next_candidate(&st, &out), 0);

    // Only current CC and zeros
    st.p25_cc_freq = 851000000;
    long neigh[4] = {st.p25_cc_freq, 0, 0, 0};
    p25_cc_record_neighbor_frequencies(&opts, &st, neigh, 4);
    rc |= expect_eq_int("cc-only", p25_cc_next_candidate(&st, &out), 0);
    rc |= expect_eq_int("cc-only-lcn-count", st.lcn_freq_count, 1);

    long neighbor_only[1] = {852000000};
    p25_cc_record_neighbor_frequencies(&opts, &st, neighbor_only, 1);
    rc |= expect_eq_int("neighbor-only", p25_cc_next_candidate(&st, &out), 0);
    rc |= expect_eq_int("prev-null-out", p25_cc_prev_candidate(&st, NULL), 0);
    rc |= expect_eq_int("prev-neighbor-only", p25_cc_prev_candidate(&st, &out), 0);
    rc |= expect_eq_int("prev-keeps-out", out == -1, 1);

    rc |= expect_eq_int("site-a", p25_cc_add_candidate(&st, 853000000, 0), 1);
    rc |= expect_eq_int("site-b", p25_cc_add_candidate(&st, 854000000, 0), 1);
    rc |= expect_eq_int("prev-first", p25_cc_prev_candidate(&st, &out), 1);
    rc |= expect_eq_int("prev-first-is-the-last", out == 854000000, 1);
    rc |= expect_eq_int("prev-again", p25_cc_prev_candidate(&st, &out), 1);
    rc |= expect_eq_int("prev-again-steps-back", out == 853000000, 1);
    rc |= expect_eq_int("next-after-prev", p25_cc_next_candidate(&st, &out), 1);
    rc |= expect_eq_int("next-goes-on-from-it", out == 854000000, 1);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
