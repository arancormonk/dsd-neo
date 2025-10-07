// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Updated: The P25 SM releases unconditionally after hangtime (and grace),
 * with no extra safety window and no post-hang gating. This test verifies
 * that behavior: no release before hangtime; release after hangtime.
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

// Minimal stubs for IO control used by SM release/tune paths
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

    // Configure hangtime and mark as voice tuned
    opts.trunk_hangtime = 2.0; // seconds
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    st.p25_cc_freq = 851000000; // non-zero CC for release path

    // Treat as P2 VC active on slot 1
    st.p25_p2_active_slot = 0;

    // Seed tune time well in the past to bypass VC_GRACE
    time_t now = time(NULL);
    st.p25_last_vc_tune_time = now - 10;

    // Recent MAC on left slot (within default mac_hold=3s) to trigger post-hang gate
    st.p25_p2_last_mac_active[0] = now - 1; // recent
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;
    st.p25_p2_audio_ring_count[0] = 0;
    st.p25_p2_audio_ring_count[1] = 0;

    // Case 1: dt below hangtime → should NOT release
    g_return_to_cc_called = 0;
    st.last_vc_sync_time = now - 1; // 1s < 2.0s
    p25_sm_tick(&opts, &st);
    rc |= expect_eq_int("no release before hangtime", g_return_to_cc_called, 0);

    // Case 2: dt past hangtime → MUST release
    g_return_to_cc_called = 0;
    now = time(NULL);
    st.p25_last_vc_tune_time = now - 10;
    st.last_vc_sync_time = now - 3; // 3s > 2.0s
    p25_sm_tick(&opts, &st);
    rc |= expect_eq_int("forced release after hangtime", g_return_to_cc_called, 1);

    return rc;
}
