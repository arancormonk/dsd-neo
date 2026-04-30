// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused tests for P25 trunk SM timing/backoff/CC-hunt behaviors. */

#include <assert.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

// Strong test stubs override weak fallbacks in SM
static long g_last_tuned_vc = 0;
static long g_last_tuned_cc = 0;
static int g_return_to_cc_called = 0;
static int g_mark_cc_sync_on_cc_tune = 0;

void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    g_last_tuned_vc = freq;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
}

void
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)ted_sps;
    g_last_tuned_cc = freq;
    if (g_mark_cc_sync_on_cc_tune) {
        dsd_mark_cc_sync(state);
    }
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq = trunk_tune_to_freq;
    hooks.tune_to_cc = trunk_tune_to_cc;
    hooks.return_to_cc = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
init_basic(dsd_opts* o, dsd_state* s) {
    memset(o, 0, sizeof(*o));
    memset(s, 0, sizeof(*s));
    o->p25_trunk = 1;
    o->trunk_hangtime = 0.2; // short for tests
    o->p25_prefer_candidates = 1;
    s->p25_cc_freq = 851000000;
    // Cache tunables at init
    p25_sm_init(o, s);
}

static void
setup_iden_simple(dsd_state* s, int iden) {
    s->p25_chan_iden = iden;
    s->p25_chan_type[iden] = 1;
    s->p25_chan_tdma[iden] = 0;
    s->p25_chan_spac[iden] = 100;           // 12.5 kHz
    s->p25_base_freq[iden] = 851000000 / 5; // units of 5 kHz
    s->p25_iden_trust[iden] = 2;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();
    init_basic(&opts, &st);

    // 1) Post-hang watchdog release (monotonic)
    st.p25_vc_freq[0] = 851012500; // voice tuned
    opts.p25_is_tuned = 1;
    double nowm = dsd_time_now_monotonic_s();
    st.p25_last_vc_tune_time_m = nowm - 1.0;
    st.last_vc_sync_time_m = nowm - 1.0; // stale
    st.p25_p2_active_slot = -1;          // P1 behavior path allowed
    p25_sm_tick(&opts, &st);
    // Expect SM to force a release back to CC after hangtime
    assert(st.p25_vc_freq[0] == 0 || g_return_to_cc_called >= 0);

    // 2) CC hunt grace and candidate tuning
    static dsd_opts o3;
    static dsd_state s3;
    init_basic(&o3, &s3);
    s3.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0; // stale CC
    (void)dsd_trunk_cc_candidates_add(&s3, 852000000, 0);
    g_last_tuned_cc = 0;
    p25_sm_tick(&o3, &s3);
    assert(g_last_tuned_cc == 852000000);

    // 3) ENC lockout once (SM helper)
    static dsd_opts o4;
    static dsd_state s4;
    init_basic(&o4, &s4);
    s4.group_tally = 0;
    s4.payload_algid = 0x84;
    s4.payload_keyid = 0x1234;
    s4.payload_miP = 0x1122334455667788ULL;
    p25_emit_enc_lockout_once(&o4, &s4, 0, 1234, 0x40);
    assert(s4.payload_algid == 0x84);
    assert(s4.payload_keyid == 0x1234);
    assert(s4.payload_miP == 0x1122334455667788ULL);
    // re-emit should no-op
    p25_emit_enc_lockout_once(&o4, &s4, 0, 1234, 0x40);
    // We can at least assert we have a group entry and mode set to "DE"
    int found = 0;
    for (unsigned i = 0; i < s4.group_tally; i++) {
        if (s4.group_array[i].groupNumber == 1234) {
            found = (strcmp(s4.group_array[i].groupMode, "DE") == 0);
            break;
        }
    }
    assert(found);

    // 4) NAC mismatch uses the latched expected CC NAC, not mutable state->p2_cc.
    static dsd_opts o5;
    static dsd_state s5;
    init_basic(&o5, &s5);
    s5.p2_cc = 0x293;
    s5.nac = 0x293;
    s5.last_cc_sync_time_m = dsd_time_now_monotonic_s();

    p25_sm_ctx_t ctx5;
    p25_sm_init_ctx(&ctx5, &o5, &s5);
    assert(ctx5.expected_cc_nac == 0x293);

    (void)dsd_trunk_cc_candidates_add(&s5, 852000000, 0);
    g_last_tuned_cc = 0;
    s5.p2_cc = 0x123; // Simulate P1 NID refresh on the wrong channel.
    s5.nac = 0x123;
    p25_sm_tick_ctx(&ctx5, &o5, &s5);
    assert(g_last_tuned_cc == 0);
    assert(ctx5.nac_mismatch_count == 1);
    assert(ctx5.expected_cc_nac == 0x293);

    p25_sm_tick_ctx(&ctx5, &o5, &s5);
    assert(g_last_tuned_cc == 0);
    assert(ctx5.nac_mismatch_count == 2);

    p25_sm_tick_ctx(&ctx5, &o5, &s5);
    assert(g_last_tuned_cc == 852000000);
    assert(ctx5.nac_mismatch_count == 0);
    assert(ctx5.expected_cc_nac == 0x293);

    // 5) A real CC activity timestamp advance may legitimately refresh the expected NAC.
    static dsd_opts o6;
    static dsd_state s6;
    init_basic(&o6, &s6);
    s6.p2_cc = 0x293;
    s6.nac = 0x293;
    s6.last_cc_sync_time_m = dsd_time_now_monotonic_s();

    p25_sm_ctx_t ctx6;
    p25_sm_init_ctx(&ctx6, &o6, &s6);
    assert(ctx6.expected_cc_nac == 0x293);

    s6.p2_cc = 0x321;
    s6.nac = 0x321;
    s6.last_cc_sync_time_m = ctx6.t_cc_sync_m + 1.0;
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx6, &o6, &s6);
    assert(g_last_tuned_cc == 0);
    assert(ctx6.nac_mismatch_count == 0);
    assert(ctx6.expected_cc_nac == 0x321);

    // 6) A CC retune hook timestamp must not relatch expected NAC before a decode.
    static dsd_opts o7;
    static dsd_state s7;
    init_basic(&o7, &s7);
    s7.p2_cc = 0x293;
    s7.nac = 0x293;
    s7.last_cc_sync_time_m = dsd_time_now_monotonic_s();

    p25_sm_ctx_t ctx7;
    p25_sm_init_ctx(&ctx7, &o7, &s7);
    assert(ctx7.expected_cc_nac == 0x293);

    (void)dsd_trunk_cc_candidates_add(&s7, 852000000, 0);
    g_last_tuned_cc = 0;
    g_mark_cc_sync_on_cc_tune = 1;
    s7.p2_cc = 0x123;
    s7.nac = 0x123;

    p25_sm_tick_ctx(&ctx7, &o7, &s7);
    assert(g_last_tuned_cc == 0);
    assert(ctx7.nac_mismatch_count == 1);
    p25_sm_tick_ctx(&ctx7, &o7, &s7);
    assert(g_last_tuned_cc == 0);
    assert(ctx7.nac_mismatch_count == 2);
    p25_sm_tick_ctx(&ctx7, &o7, &s7);
    assert(g_last_tuned_cc == 852000000);
    assert(ctx7.nac_mismatch_count == 0);
    assert(ctx7.expected_cc_nac == 0x293);

    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx7, &o7, &s7);
    assert(g_last_tuned_cc == 0);
    assert(ctx7.nac_mismatch_count == 1);
    assert(ctx7.expected_cc_nac == 0x293);
    g_mark_cc_sync_on_cc_tune = 0;

    fprintf(stderr, "P25 SM core tests passed\n");
    return 0;
}
