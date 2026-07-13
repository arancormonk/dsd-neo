// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 neighbor update spam test: stress p25_cc_record_neighbor_frequencies with
 * many updates and assert neighbors do not become tuneable CC candidates.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

#define setenv dsd_test_setenv

// Stubs for radio control
bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return true;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
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
    opts.p25_trunk = 1;
    // Set system identity so SM can label candidates; disable on-disk cache via env
    st.p2_wacn = 0xABCDE;
    st.p2_sysid = 0x123;
    setenv("DSD_NEO_CC_CACHE", "0", 1);
    dsd_neo_config_init(NULL);

    // Timing start for rough performance guard
    double elapsed_ms = 0.0;
    uint64_t t0_ns = dsd_time_monotonic_ns();

    // Spam with pseudo-random neighbors around 851 MHz
    // Ensure we include some duplicates and the current CC at times.
    const int rounds = 2000;
    for (int i = 0; i < rounds; i++) {
        long f[4];
        int n = (i % 4) + 1;
        for (int k = 0; k < n; k++) {
            long step = (long)((i * 13 + k * 7) % 80) * 12500; // 12.5 kHz steps over ~1 MHz span
            f[k] = 851000000 + step;
            if ((i % 97) == 0 && k == 0) {
                st.p25_cc_freq = f[k]; // sometimes match current CC to test dedup
            }
        }
        p25_cc_record_neighbor_frequencies(&opts, &st, f, n);
        rc |= expect_true("neighbor<=max", st.p25_nb_count >= 0 && st.p25_nb_count <= P25_NB_MAX);
    }

    // Timing end and guard: ensure this remains snappy
    uint64_t t1_ns = dsd_time_monotonic_ns();
    elapsed_ms = (double)(t1_ns - t0_ns) / 1e6;
    // Allow a generous envelope to avoid CI flakiness, but detect regressions
#if DSD_PLATFORM_WIN_NATIVE
    rc |= expect_true("neighbor-spam-fast", elapsed_ms < 1000.0);
#else
    rc |= expect_true("neighbor-spam-fast", elapsed_ms < 200.0);
#endif

    // Generic neighbor updates must not seed the CC hunt list. Only validated
    // Current-site candidates should be tuneable by p25_cc_next_candidate().
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&st);
    const int count = cc ? cc->count : 0;
    rc |= expect_true("neighbor-not-candidates", count == 0);

    long out = 0;
    rc |= expect_true("neighbor-next-empty", p25_cc_next_candidate(&st, &out) == 0);

    (void)p25_cc_add_candidate(&st, 852500000L, 1);
    out = 0;
    rc |= expect_true("validated-next-ok", p25_cc_next_candidate(&st, &out) == 1);
    rc |= expect_true("validated-next-freq", out == 852500000L);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
