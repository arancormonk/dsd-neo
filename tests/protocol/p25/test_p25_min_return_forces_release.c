// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify that when the minimal P25p2 follower decides to return to CC, it
 * forces a trunk SM release even if TDMA post-hang gating would normally
 * defer the release (e.g., due to audio_allowed/ring/MAC hints).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/p25/p25_p2_sm_min.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#undef main

// --- IO control stubs (rigctl/RTL) ---

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

struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

// Count return-to-CC invocations
static int g_return_to_cc_called = 0;

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
}

// --- Simple expectation helper ---
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

    // Configure trunking and emulate being tuned to a P25p2 VC
    opts.trunk_hangtime = 2.0; // seconds
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    st.p25_cc_freq = 851000000; // non-zero CC so release path calls return_to_cc
    st.p25_p2_active_slot = 0;  // TDMA voice context
    st.lastsynctype = 35;       // P25p2

    // Arrange gating that would defer a non-forced release: set audio_allowed
    // and recent voice activity within hangtime.
    time_t now = time(NULL);
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_ring_count[0] = 0;
    st.p25_p2_last_mac_active[0] = now; // recent MAC (optional for this case)
    st.last_vc_sync_time = now;         // recent voice (dt < hangtime)

    // Baseline: calling release without force should be deferred by gating
    g_return_to_cc_called = 0;
    st.p25_sm_force_release = 0;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("non-forced release deferred (no return)", g_return_to_cc_called, 0);

    // Now use the minimal follower to request a return. Its callback should
    // force release so gating cannot defer the return.
    dsd_p25p2_min_sm* sm = dsd_p25p2_min_get();
    dsd_p25p2_min_configure_ex(sm, /*hang*/ 0.1, /*grace*/ 0.05, /*dwell*/ 0.01, /*gvt*/ 0.1, /*backoff*/ 0.1);
    sm->state = DSD_P25P2_MIN_HANG;
    sm->t_hang_start = now - 1; // beyond hang -> tick should request return
    g_return_to_cc_called = 0;
    dsd_p25p2_min_tick(sm, &opts, &st);
    rc |= expect_eq_int("minSM forced return invoked", g_return_to_cc_called > 0, 1);

    return rc;
}
