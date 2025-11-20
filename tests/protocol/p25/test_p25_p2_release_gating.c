// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 trunk SM release gating tests.
 * Verifies deferral when audio gates are active, hangtime delay for recent
 * voice, and forced release clearing of state and return_to_cc invocation.
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

// Stubs to capture release behavior side-effects
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
    opts.trunk_hangtime = 3; // seconds

    // Treat as P2 VC active (slot assigned)
    st.p25_p2_active_slot = 0;

    // Case 1: audio gates active → release deferred (within hangtime)
    g_return_to_cc_called = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0;
    // Mark recent voice so that audio gates are considered "active" under
    // current semantics (stale gates alone should not defer post-hangtime).
    st.last_vc_sync_time = time(NULL);
    int before_rel = st.p25_sm_release_count;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("rel count inc", st.p25_sm_release_count, before_rel + 1);
    rc |= expect_eq_int("deferred (no rtc)", g_return_to_cc_called, 0);
    rc |= expect_eq_int("gate L stays", st.p25_p2_audio_allowed[0], 1);

    // Clear gates for next cases
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;

    // Case 2: recent voice within hangtime and not forced → deferred
    g_return_to_cc_called = 0;
    st.last_vc_sync_time = time(NULL);
    before_rel = st.p25_sm_release_count;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("rel count inc 2", st.p25_sm_release_count, before_rel + 1);
    rc |= expect_eq_int("deferred recent", g_return_to_cc_called, 0);

    // Case 3: forced release clears state and calls return_to_cc
    g_return_to_cc_called = 0;
    st.last_vc_sync_time = time(NULL);
    st.payload_algid = 0x84;
    st.payload_algidR = 0x84;
    st.payload_keyid = 0x12;
    st.payload_keyidR = 0x34;
    st.payload_miP = 0xAAAAAAAAAAAAAAAAULL;
    st.payload_miN = 0xBBBBBBBBBBBBBBBBULL;
    st.p25_sm_force_release = 1;
    before_rel = st.p25_sm_release_count;
    p25_sm_on_release(&opts, &st);
    rc |= expect_eq_int("rel count inc 3", st.p25_sm_release_count, before_rel + 1);
    rc |= expect_eq_int("rtc called", g_return_to_cc_called, 1);
    rc |= expect_eq_int("gate L cleared", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq_int("gate R cleared", st.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq_int("alg cleared L", st.payload_algid, 0);
    rc |= expect_eq_int("alg cleared R", st.payload_algidR, 0);
    rc |= expect_eq_int("kid cleared L", st.payload_keyid, 0);
    rc |= expect_eq_int("kid cleared R", st.payload_keyidR, 0);
    rc |= expect_eq_int("miP cleared", st.payload_miP == 0ULL, 1);
    rc |= expect_eq_int("miN cleared", st.payload_miN == 0ULL, 1);

    return rc;
}
