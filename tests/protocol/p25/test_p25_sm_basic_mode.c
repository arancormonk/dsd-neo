// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify basic mode: immediate release after hangtime+grace without post-hang gating. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#undef main

static int g_return_to_cc_called = 0;

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
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s: expected true\n", tag);
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
    opts.trunk_hangtime = 1.0;
    opts.p25_sm_basic_mode = 1; // enable basic mode
    st.p25_cc_freq = 851000000;

    // Emulate being tuned with no recent voice (past hangtime)
    opts.p25_is_tuned = 1;
    st.last_vc_sync_time_m = dsd_time_now_monotonic_s() - 2.0;
    st.p25_last_vc_tune_time_m = dsd_time_now_monotonic_s() - 2.0;

    g_return_to_cc_called = 0;
    p25_sm_tick(&opts, &st);
    rc |= expect_true("basic mode released", g_return_to_cc_called > 0);
    return rc;
}
