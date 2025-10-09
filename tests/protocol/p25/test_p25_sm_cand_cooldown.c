// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify CC candidate cooldown: after tuning a failing candidate, it is
 * cooled down and skipped on the next hunt in favor of another candidate. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

static long g_last_tuned_cc = 0;

void
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)opts;
    (void)state;
    g_last_tuned_cc = freq;
}

static void
init_basic(dsd_opts* o, dsd_state* s) {
    memset(o, 0, sizeof(*o));
    memset(s, 0, sizeof(*s));
    o->p25_trunk = 1;
    o->trunk_hangtime = 0.2; // short for test
    o->p25_prefer_candidates = 1;
    s->p25_cc_freq = 851000000;
    p25_sm_init(o, s);
}

int
main(void) {
    dsd_opts o;
    dsd_state st;
    init_basic(&o, &st);
    // Two candidates A, B
    long A = 852000000;
    long B = 853000000;
    st.p25_cc_cand_count = 2;
    st.p25_cc_candidates[0] = A;
    st.p25_cc_candidates[1] = B;
    // Force CC hunt
    st.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;

    // First tick: should tune to A
    g_last_tuned_cc = 0;
    p25_sm_tick(&o, &st);
    assert(g_last_tuned_cc == A);

    // Simulate evaluation window expiry with no CC activity to trigger cooldown for A
    st.p25_cc_eval_freq = A;
    st.p25_cc_eval_start_m = dsd_time_now_monotonic_s() - 5.0;
    st.last_cc_sync_time_m = 0.0; // no CC activity

    // Next tick: cooldown applied; next hunt should pick B
    g_last_tuned_cc = 0;
    p25_sm_tick(&o, &st);
    assert(g_last_tuned_cc == B);
    return 0;
}
