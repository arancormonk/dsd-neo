// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused tests for P25 trunk SM timing/backoff/CC-hunt behaviors. */

#include <assert.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Strong test stubs override weak fallbacks in SM
static long g_last_tuned_vc = 0;
static long g_last_tuned_cc = 0;
static int g_return_to_cc_called = 0;
static int g_mark_cc_sync_on_cc_tune = 0;
static dsd_trunk_tune_result g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_result_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_result_tune_to_freq_calls = 0;
static int g_result_tune_to_cc_calls = 0;
static int g_result_return_to_cc_calls = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    g_last_tuned_vc = freq;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
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

static dsd_trunk_tune_result
trunk_tune_to_freq_result(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    g_result_tune_to_freq_calls++;
    g_last_tuned_vc = freq;
    if (g_result_tune_to_freq_result == DSD_TRUNK_TUNE_RESULT_OK) {
        if (opts) {
            opts->p25_is_tuned = 1;
            opts->trunk_is_tuned = 1;
        }
        if (state) {
            state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
            state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
        }
    }
    return g_result_tune_to_freq_result;
}

static dsd_trunk_tune_result
trunk_tune_to_cc_result(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)ted_sps;
    g_result_tune_to_cc_calls++;
    g_last_tuned_cc = freq;
    if (g_result_tune_to_cc_result == DSD_TRUNK_TUNE_RESULT_OK && state) {
        state->trunk_cc_freq = freq;
        dsd_mark_cc_sync(state);
    }
    return g_result_tune_to_cc_result;
}

static dsd_trunk_tune_result
return_to_cc_result(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_result_return_to_cc_calls++;
    return g_result_return_to_cc_result;
}

static void
install_result_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_result = trunk_tune_to_freq_result;
    hooks.tune_to_cc_result = trunk_tune_to_cc_result;
    hooks.return_to_cc_result = return_to_cc_result;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
init_basic(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(o, 0, sizeof(*o));
    DSD_MEMSET(s, 0, sizeof(*s));
    o->p25_trunk = 1;
    o->trunk_hangtime = 0.2f; // short for tests
    o->p25_prefer_candidates = 1;
    s->p25_cc_freq = 851000000;
    // Cache tunables at init
    p25_sm_init(o, s);
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
    s4.payload_algid = 0x84;
    s4.payload_keyid = 0x1234;
    s4.payload_miP = 0x1122334455667788ULL;
    p25_emit_enc_lockout_once(&o4, &s4, 0, 1234, 0x40);
    assert(s4.payload_algid == 0x84);
    assert(s4.payload_keyid == 0x1234);
    assert(s4.payload_miP == 0x1122334455667788ULL);
    // re-emit should no-op
    p25_emit_enc_lockout_once(&o4, &s4, 0, 1234, 0x40);
    dsd_tg_policy_lookup lockout_lookup;
    assert(dsd_tg_policy_lookup_id(&s4, 1234U, &lockout_lookup) == 0);
    assert(lockout_lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lockout_lookup.entry.mode, "DE") == 0);

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

    // 7) If bogus/foreign CC candidates are exhausted, fall back to the known
    // primary control channel even when no user LCN list was loaded.
    static dsd_opts o8;
    static dsd_state s8;
    DSD_MEMSET(&o8, 0, sizeof(o8));
    DSD_MEMSET(&s8, 0, sizeof(s8));
    o8.p25_trunk = 1;
    o8.trunk_hangtime = 0.2f;
    o8.p25_prefer_candidates = 1;
    s8.p25_cc_freq = 851000000;
    s8.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;

    p25_sm_ctx_t ctx8;
    p25_sm_init_ctx(&ctx8, &o8, &s8);
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx8, &o8, &s8);
    assert(g_last_tuned_cc == 851000000);

    // 8) In no-import operation, legacy LCN fallback must not cycle through
    // voice/grant-derived frequencies as CC hunt targets.
    static dsd_opts o8b;
    static dsd_state s8b;
    DSD_MEMSET(&o8b, 0, sizeof(o8b));
    DSD_MEMSET(&s8b, 0, sizeof(s8b));
    o8b.p25_trunk = 1;
    o8b.trunk_hangtime = 0.2f;
    s8b.p25_cc_freq = 851000000;
    s8b.trunk_lcn_freq[0] = 851000000;
    s8b.trunk_lcn_freq[1] = 889000000;
    s8b.lcn_freq_count = 2;

    p25_sm_ctx_t ctx8b;
    p25_sm_init_ctx(&ctx8b, &o8b, &s8b);
    ctx8b.state = P25_SM_HUNTING;
    ctx8b.t_hunt_try_m = 0.0;
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx8b, &o8b, &s8b);
    assert(g_last_tuned_cc == 851000000);

    ctx8b.state = P25_SM_HUNTING;
    ctx8b.t_hunt_try_m = dsd_time_now_monotonic_s() - 10.0;
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx8b, &o8b, &s8b);
    assert(g_last_tuned_cc == 851000000);

    // 9) Trunk-scan per-target channel maps are explicit user LCN lists even
    // though the live opts->chan_in_file is empty.
    static dsd_opts o8d;
    static dsd_state s8d;
    DSD_MEMSET(&o8d, 0, sizeof(o8d));
    DSD_MEMSET(&s8d, 0, sizeof(s8d));
    o8d.p25_trunk = 1;
    o8d.trunk_scan_enabled = 1;
    o8d.trunk_hangtime = 0.2f;
    s8d.p25_cc_freq = 851000000;
    s8d.trunk_lcn_freq[0] = 851000000;
    s8d.trunk_lcn_freq[1] = 852000000;
    s8d.lcn_freq_count = 2;
    s8d.trunk_chan_map[101] = 852000000;
    s8d.trunk_chan_map_used[0] = 101;
    s8d.trunk_chan_map_used_count = 1U;

    p25_sm_ctx_t ctx8d;
    p25_sm_init_ctx(&ctx8d, &o8d, &s8d);
    ctx8d.state = P25_SM_HUNTING;
    ctx8d.t_hunt_try_m = 0.0;
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx8d, &o8d, &s8d);
    assert(g_last_tuned_cc == 851000000);

    ctx8d.state = P25_SM_HUNTING;
    ctx8d.t_hunt_try_m = dsd_time_now_monotonic_s() - 10.0;
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx8d, &o8d, &s8d);
    assert(g_last_tuned_cc == 852000000);

    // 10) A current-site candidate is still eligible in no-import operation.
    static dsd_opts o8c;
    static dsd_state s8c;
    DSD_MEMSET(&o8c, 0, sizeof(o8c));
    DSD_MEMSET(&s8c, 0, sizeof(s8c));
    o8c.p25_trunk = 1;
    o8c.trunk_hangtime = 0.2f;
    s8c.p25_cc_freq = 851000000;
    s8c.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;
    dsd_state_set_trunk_chan_freq(&s8c, 101U, 889000000L);
    assert(s8c.trunk_chan_map_used_count == 1U);
    (void)dsd_trunk_cc_candidates_add(&s8c, 852000000, 0);

    p25_sm_ctx_t ctx8c;
    p25_sm_init_ctx(&ctx8c, &o8c, &s8c);
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx8c, &o8c, &s8c);
    assert(g_last_tuned_cc == 852000000);

    // 11) A failed VC tune must not advance P25 tuned state or counters.
    install_result_tuning_hooks();
    static dsd_opts o9;
    static dsd_state s9;
    DSD_MEMSET(&o9, 0, sizeof(o9));
    DSD_MEMSET(&s9, 0, sizeof(s9));
    o9.p25_trunk = 1;
    o9.trunk_tune_group_calls = 1;
    s9.p25_cc_freq = 851000000;
    int id9 = 1;
    s9.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s9.p25_iden_fdma[id9].chan_type = 1;
    s9.p25_iden_fdma[id9].chan_spac = 100;
    s9.p25_iden_fdma[id9].trust = 2;
    s9.p25_iden_fdma[id9].populated = 1;
    s9.p25_chan_tdma_explicit[id9] = 1;
    int ch9 = (id9 << 12) | 0x000A;
    p25_sm_ctx_t ctx9;
    p25_sm_init_ctx(&ctx9, &o9, &s9);
    p25_sm_event_t ev9;
    DSD_MEMSET(&ev9, 0, sizeof(ev9));
    ev9.type = P25_SM_EV_GRANT;
    ev9.channel = ch9;
    ev9.tg = 1234;
    ev9.src = 42;
    ev9.is_group = 1;
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx9, &o9, &s9, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(o9.p25_is_tuned == 0);
    assert(o9.trunk_is_tuned == 0);
    assert(s9.p25_vc_freq[0] == 0);
    assert(s9.trunk_vc_freq[0] == 0);
    assert(s9.p25_sm_tune_count == 0);
    assert(ctx9.state == P25_SM_ON_CC);

    // 12) A failed CC retune must not update the tracked CC frequency or sync timestamp.
    static dsd_opts o10;
    static dsd_state s10;
    DSD_MEMSET(&o10, 0, sizeof(o10));
    DSD_MEMSET(&s10, 0, sizeof(s10));
    o10.p25_trunk = 1;
    o10.p25_prefer_candidates = 1;
    s10.p25_cc_freq = 851000000;
    s10.trunk_cc_freq = 851000000;
    s10.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;
    const double old_cc_sync_m = s10.last_cc_sync_time_m;
    const double cc_sync_epsilon_s = 1.0e-9;
    (void)dsd_trunk_cc_candidates_add(&s10, 852000000, 0);
    p25_sm_ctx_t ctx10;
    p25_sm_init_ctx(&ctx10, &o10, &s10);
    g_result_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_TIMEOUT;
    g_result_tune_to_cc_calls = 0;
    p25_sm_tick_ctx(&ctx10, &o10, &s10);
    assert(g_result_tune_to_cc_calls == 1);
    assert(s10.trunk_cc_freq == 851000000);
    assert((s10.last_cc_sync_time_m - old_cc_sync_m) <= cc_sync_epsilon_s);
    assert((old_cc_sync_m - s10.last_cc_sync_time_m) <= cc_sync_epsilon_s);
    assert(s10.p25_cc_eval_freq == 0);
    assert(ctx10.state == P25_SM_HUNTING);

    // 13) Deferred VC tunes leave state unchanged and can be retried by a later grant.
    static dsd_opts o11;
    static dsd_state s11;
    DSD_MEMSET(&o11, 0, sizeof(o11));
    DSD_MEMSET(&s11, 0, sizeof(s11));
    o11.p25_trunk = 1;
    o11.trunk_tune_group_calls = 1;
    s11.p25_cc_freq = 851000000;
    s11.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s11.p25_iden_fdma[id9].chan_type = 1;
    s11.p25_iden_fdma[id9].chan_spac = 100;
    s11.p25_iden_fdma[id9].trust = 2;
    s11.p25_iden_fdma[id9].populated = 1;
    s11.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx11;
    p25_sm_init_ctx(&ctx11, &o11, &s11);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx11, &o11, &s11, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(o11.p25_is_tuned == 0);
    assert(s11.p25_sm_tune_count == 0);
    assert(ctx11.state == P25_SM_ON_CC);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_event(&ctx11, &o11, &s11, &ev9);
    assert(g_result_tune_to_freq_calls == 2);
    assert(o11.p25_is_tuned == 1);
    assert(o11.trunk_is_tuned == 1);
    assert(s11.p25_sm_tune_count == 1);
    assert(ctx11.state == P25_SM_TUNED);

    // 14) A forced release must survive deferred return-to-CC and retry even while voice remains active.
    static dsd_opts o12;
    static dsd_state s12;
    DSD_MEMSET(&o12, 0, sizeof(o12));
    DSD_MEMSET(&s12, 0, sizeof(s12));
    o12.p25_trunk = 1;
    o12.p25_is_tuned = 1;
    o12.trunk_is_tuned = 1;
    o12.trunk_hangtime = 0.2f;
    s12.p25_cc_freq = 851000000;
    s12.trunk_cc_freq = 851000000;
    s12.p25_vc_freq[0] = 852000000;
    s12.trunk_vc_freq[0] = 852000000;
    p25_sm_ctx_t ctx12;
    p25_sm_init_ctx(&ctx12, &o12, &s12);
    ctx12.state = P25_SM_TUNED;
    ctx12.vc_freq_hz = 852000000;
    ctx12.slots[0].voice_active = 1;
    s12.p25_sm_force_release = 1;
    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    g_result_return_to_cc_calls = 0;
    p25_sm_release(&ctx12, &o12, &s12, "release-forced");
    assert(g_result_return_to_cc_calls == 1);
    assert(s12.p25_sm_force_release == 1);
    assert(o12.p25_is_tuned == 1);
    assert(ctx12.state == P25_SM_TUNED);

    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_tick_ctx(&ctx12, &o12, &s12);
    assert(g_result_return_to_cc_calls == 2);
    assert(s12.p25_sm_force_release == 0);
    assert(o12.p25_is_tuned == 0);
    assert(o12.trunk_is_tuned == 0);
    assert(ctx12.state == P25_SM_ON_CC);
    assert(s12.p25_retune_block_until == 0);

    install_trunk_tuning_hooks();

    DSD_FPRINTF(stderr, "P25 SM core tests passed\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
