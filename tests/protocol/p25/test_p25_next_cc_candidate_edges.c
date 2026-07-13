// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 next CC candidate edge cases.
 * - Returns 0 when empty list
 * - Returns 0 when only current CC, zeros, or generic neighbors are present
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

// Stubs
bool
SetFreq(int sockfd, long int freq) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}
struct RtlSdrContext* g_rtl_ctx = 0; // NOLINT(misc-use-internal-linkage)

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) { // NOLINT(misc-use-internal-linkage)
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

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
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
