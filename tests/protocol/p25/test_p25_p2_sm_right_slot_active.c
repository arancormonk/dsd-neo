// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P2 trunk SM release gating: ensure right-slot (slot 2) activity
 * defers return-to-CC. Guards against regressions that would reintroduce
 * slot-2 VC/CC thrash.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#undef main

static int g_return_to_cc_called = 0;

// Stubs to capture behavior
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
}
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
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
    opts.trunk_hangtime = 2.0;

    // Treat as P2 VC active on slot 2
    st.p25_p2_active_slot = 1;

    // Case 1: Recent MAC_ACTIVE on right slot should defer release
    g_return_to_cc_called = 0;
    st.p25_p2_last_mac_active[1] = time(NULL);
    int before = st.p25_sm_release_count;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("rel count inc", st.p25_sm_release_count, before + 1);
    rc |= expect_eq_int("deferred due to R ACTIVE", g_return_to_cc_called, 0);

    // Case 2: ring backlog without recent MAC/PTT should NOT defer
    // We intentionally clear last_mac_active and set ring_count to ensure
    // stale jitter alone does not wedge the SM on a dead VC.
    g_return_to_cc_called = 0;
    st.p25_p2_last_mac_active[1] = 0;
    st.p25_p2_audio_ring_count[1] = 5;
    before = st.p25_sm_release_count;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("rel count inc 2", st.p25_sm_release_count, before + 1);
    rc |= expect_eq_int("no defer on stale ring", g_return_to_cc_called, 1);

    // Case 3: forced release ignores gates and calls return_to_cc
    g_return_to_cc_called = 0;
    st.p25_p2_audio_ring_count[1] = 0;
    st.p25_sm_force_release = 1;
    before = st.p25_sm_release_count;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("rel count inc 3", st.p25_sm_release_count, before + 1);
    rc |= expect_eq_int("forced -> CC", g_return_to_cc_called, 1);

    return rc;
}
