// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 neighbor update spam test: stress p25_sm_on_neighbor_update with
 * many updates and assert CC candidate list remains bounded and
 * iteration via p25_sm_next_cc_candidate stays consistent.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#undef main

// Stubs for radio control
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return true;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s: failed\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    dsd_opts opts;
    dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    opts.p25_trunk = 1;
    // Set system identity so SM can label candidates; disable on-disk cache via env
    st.p2_wacn = 0xABCDE;
    st.p2_sysid = 0x123;
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    setenv("DSD_NEO_CC_CACHE", "0", 1);
#endif

    // Timing start for rough performance guard
    double elapsed_ms = 0.0;
#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
#else
    clock_t c0 = clock();
#endif

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
        p25_sm_on_neighbor_update(&opts, &st, f, n);
        // Count should never exceed 16
        rc |= expect_true("cand<=16", st.p25_cc_cand_count >= 0 && st.p25_cc_cand_count <= 16);
    }

    // Timing end and guard: ensure this remains snappy
#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    elapsed_ms = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
#else
    clock_t c1 = clock();
    if (c1 > c0) {
        elapsed_ms = 1000.0 * (double)(c1 - c0) / (double)CLOCKS_PER_SEC;
    }
#endif
    // Allow a generous envelope to avoid CI flakiness, but detect regressions
    rc |= expect_true("neighbor-spam-fast", elapsed_ms < 200.0);

    // Next-candidate iteration should cycle through at most count entries and
    // never return 0 or the current CC.
    int count = st.p25_cc_cand_count;
    if (count > 0) {
        int seen_nonzero = 0;
        long last = -1;
        for (int i = 0; i < count * 3; i++) {
            long out = 0;
            int ok = p25_sm_next_cc_candidate(&st, &out);
            rc |= expect_true("next->ok", ok == 1);
            rc |= expect_true("next->nonzero", out != 0);
            rc |= expect_true("next->neq-cc", out != st.p25_cc_freq);
            // do a weak monotonicity sanity: not all calls must differ, but should
            // make progress across multiple calls
            if (last != out) {
                seen_nonzero = 1;
            }
            last = out;
        }
        rc |= expect_true("progress", seen_nonzero);
    }

    return rc;
}
