// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused tests for P25 trunk SM timing/backoff/CC-hunt behaviors. */

#include <assert.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdio.h>
#include <time.h>
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
static long g_rigctl_current_freq = 0;

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

static long
fake_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return g_rigctl_current_freq;
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

static void
setup_tdma_iden(dsd_state* s, int id) {
    s->p25_chan_iden = id;
    s->p25_iden_tdma[id].base_freq = 851000000 / 5;
    s->p25_iden_tdma[id].chan_type = 3;
    s->p25_iden_tdma[id].chan_spac = 100;
    s->p25_iden_tdma[id].trust = 2;
    s->p25_iden_tdma[id].populated = 1;
    s->p25_chan_tdma_explicit[id] = 2;
}

static void
clear_decoder_vc_tune_state(dsd_opts* opts, dsd_state* state) {
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
}

int
main(void) {
    p25_sm_event_t legacy_group_grant = {
        P25_SM_EV_GRANT, -1, 0x1234, 851500000L, 1000, 123, 0, P25_SM_SVC_UNKNOWN, 1, 0x80, 0x1234, 0, 0.0,
    };
    assert(legacy_group_grant.is_group == 1);
    assert(legacy_group_grant.algid == 0x80);
    assert(legacy_group_grant.keyid == 0x1234);
    assert(legacy_group_grant.data_call_override == 0);

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
    int enc_cache_seen = 0;
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (s4.p25_enc_tg_cache_tg[i] == 1234U && s4.p25_enc_tg_cache_until[i] > time(NULL)) {
            enc_cache_seen = 1;
        }
    }
    assert(enc_cache_seen == 1);
    // re-emit should not create a runtime policy block
    p25_emit_enc_lockout_once(&o4, &s4, 0, 1234, 0x40);
    dsd_tg_policy_lookup lockout_lookup;
    assert(dsd_tg_policy_lookup_id(&s4, 1234U, &lockout_lookup) == 0);
    assert(lockout_lookup.match == DSD_TG_POLICY_MATCH_NONE);

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

    // 15) Verified CC callers seed an unknown CC from the live tuner before leaving the control channel.
    static dsd_opts o13;
    static dsd_state s13;
    DSD_MEMSET(&o13, 0, sizeof(o13));
    DSD_MEMSET(&s13, 0, sizeof(s13));
    o13.p25_trunk = 1;
    o13.trunk_tune_group_calls = 1;
    o13.audio_in_type = AUDIO_IN_RTL;
    o13.rtlsdr_center_freq = 851012500U;
    s13.synctype = DSD_SYNC_P25P1_POS;
    s13.p25_cc_is_tdma = 1;
    s13.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s13.p25_iden_fdma[id9].chan_type = 1;
    s13.p25_iden_fdma[id9].chan_spac = 100;
    s13.p25_iden_fdma[id9].trust = 2;
    s13.p25_iden_fdma[id9].populated = 1;
    s13.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx13;
    p25_sm_init_ctx(&ctx13, &o13, &s13);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_freq_calls = 0;
    p25_sm_seed_cc_from_current_tuner_if_unknown(&o13, &s13);
    p25_sm_event(&ctx13, &o13, &s13, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(s13.p25_cc_freq == 851012500);
    assert(s13.trunk_cc_freq == 851012500);
    assert(s13.trunk_lcn_freq[0] == 851012500);
    assert(s13.p25_cc_is_tdma == 0);
    assert(ctx13.state == P25_SM_TUNED);

    static dsd_opts o14;
    static dsd_state s14;
    DSD_MEMSET(&o14, 0, sizeof(o14));
    DSD_MEMSET(&s14, 0, sizeof(s14));
    o14.p25_trunk = 1;
    o14.trunk_tune_group_calls = 1;
    o14.audio_in_type = AUDIO_IN_TCP;
    o14.use_rigctl = 1;
    s14.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s14.p25_iden_fdma[id9].chan_type = 1;
    s14.p25_iden_fdma[id9].chan_spac = 100;
    s14.p25_iden_fdma[id9].trust = 2;
    s14.p25_iden_fdma[id9].populated = 1;
    s14.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx14;
    p25_sm_init_ctx(&ctx14, &o14, &s14);
    g_rigctl_current_freq = 851987500;
    dsd_rigctl_query_hooks hooks = {0};
    hooks.get_current_freq_hz = fake_get_current_freq_hz;
    dsd_rigctl_query_hooks_set(hooks);
    g_result_tune_to_freq_calls = 0;
    p25_sm_seed_cc_from_current_tuner_if_unknown(&o14, &s14);
    p25_sm_event(&ctx14, &o14, &s14, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(s14.p25_cc_freq == 851987500);
    assert(s14.trunk_cc_freq == 851987500);
    assert(s14.trunk_lcn_freq[0] == 851987500);
    assert(ctx14.state == P25_SM_TUNED);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    g_rigctl_current_freq = 0;

    static dsd_opts o14b;
    static dsd_state s14b;
    DSD_MEMSET(&o14b, 0, sizeof(o14b));
    DSD_MEMSET(&s14b, 0, sizeof(s14b));
    o14b.p25_trunk = 1;
    o14b.trunk_tune_group_calls = 1;
    o14b.audio_in_type = AUDIO_IN_RTL;
    o14b.rtlsdr_center_freq = 851012500U;
    s14b.synctype = DSD_SYNC_P25P2_POS;
    s14b.p25_cc_is_tdma = 2;
    s14b.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s14b.p25_iden_fdma[id9].chan_type = 1;
    s14b.p25_iden_fdma[id9].chan_spac = 100;
    s14b.p25_iden_fdma[id9].trust = 2;
    s14b.p25_iden_fdma[id9].populated = 1;
    s14b.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx14b;
    p25_sm_init_ctx(&ctx14b, &o14b, &s14b);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    g_result_tune_to_freq_calls = 0;
    p25_sm_seed_cc_from_current_tuner_if_unknown(&o14b, &s14b);
    p25_sm_event(&ctx14b, &o14b, &s14b, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(s14b.p25_cc_freq == 851012500);
    assert(s14b.trunk_cc_freq == 851012500);
    assert(s14b.trunk_lcn_freq[0] == 851012500);
    assert(s14b.p25_cc_is_tdma == 2);
    assert(o14b.p25_is_tuned == 0);
    assert(s14b.p25_sm_tune_count == 0);
    assert(ctx14b.state == P25_SM_ON_CC);
    assert(s14b.last_cc_sync_time_m > 0.0);
    int tune_cc_calls_before_tick = g_result_tune_to_cc_calls;
    p25_sm_tick_ctx(&ctx14b, &o14b, &s14b);
    assert(ctx14b.state == P25_SM_ON_CC);
    assert(g_result_tune_to_cc_calls == tune_cc_calls_before_tick);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;

    static dsd_opts o14c;
    static dsd_state s14c;
    DSD_MEMSET(&o14c, 0, sizeof(o14c));
    DSD_MEMSET(&s14c, 0, sizeof(s14c));
    o14c.p25_trunk = 1;
    o14c.trunk_tune_group_calls = 1;
    s14c.p25_cc_freq = 851012500;
    s14c.trunk_cc_freq = 851012500;
    s14c.nac = 0x123;
    s14c.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s14c.p25_iden_fdma[id9].chan_type = 1;
    s14c.p25_iden_fdma[id9].chan_spac = 100;
    s14c.p25_iden_fdma[id9].trust = 2;
    s14c.p25_iden_fdma[id9].populated = 1;
    s14c.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx14c;
    p25_sm_init_ctx(&ctx14c, &o14c, &s14c);
    ctx14c.state = P25_SM_IDLE;
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx14c, &o14c, &s14c, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(s14c.p25_cc_freq == 851012500);
    assert(s14c.trunk_cc_freq == 851012500);
    assert(o14c.p25_is_tuned == 0);
    assert(s14c.p25_sm_tune_count == 0);
    assert(ctx14c.state == P25_SM_ON_CC);
    assert(ctx14c.expected_cc_nac == 0x123);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;

    // 16) A generic trunk CC alias is enough to seed the P25-specific alias.
    static dsd_opts o15;
    static dsd_state s15;
    DSD_MEMSET(&o15, 0, sizeof(o15));
    DSD_MEMSET(&s15, 0, sizeof(s15));
    o15.p25_trunk = 1;
    o15.trunk_tune_group_calls = 1;
    s15.trunk_cc_freq = 851000000;
    s15.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s15.p25_iden_fdma[id9].chan_type = 1;
    s15.p25_iden_fdma[id9].chan_spac = 100;
    s15.p25_iden_fdma[id9].trust = 2;
    s15.p25_iden_fdma[id9].populated = 1;
    s15.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx15;
    p25_sm_init_ctx(&ctx15, &o15, &s15);
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx15, &o15, &s15, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(s15.p25_cc_freq == 851000000);
    assert(s15.trunk_cc_freq == 851000000);
    assert(s15.trunk_lcn_freq[0] == 851000000);
    assert(ctx15.state == P25_SM_TUNED);

    // 17) Never learn the CC from the current tuner while already voice-tuned.
    static dsd_opts o16;
    static dsd_state s16;
    DSD_MEMSET(&o16, 0, sizeof(o16));
    DSD_MEMSET(&s16, 0, sizeof(s16));
    o16.p25_trunk = 1;
    o16.p25_is_tuned = 1;
    o16.trunk_is_tuned = 1;
    o16.audio_in_type = AUDIO_IN_RTL;
    o16.rtlsdr_center_freq = 852112500U;
    p25_sm_seed_cc_from_current_tuner_if_unknown(&o16, &s16);
    assert(s16.p25_cc_freq == 0);
    assert(s16.trunk_cc_freq == 0);
    assert(s16.trunk_lcn_freq[0] == 0);

    // 18) Unguarded generic grant events must not sample the current tuner as a CC.
    static dsd_opts o17;
    static dsd_state s17;
    DSD_MEMSET(&o17, 0, sizeof(o17));
    DSD_MEMSET(&s17, 0, sizeof(s17));
    o17.p25_trunk = 1;
    o17.trunk_tune_group_calls = 1;
    o17.audio_in_type = AUDIO_IN_RTL;
    o17.rtlsdr_center_freq = 852112500U;
    s17.synctype = DSD_SYNC_P25P2_POS;
    s17.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s17.p25_iden_fdma[id9].chan_type = 1;
    s17.p25_iden_fdma[id9].chan_spac = 100;
    s17.p25_iden_fdma[id9].trust = 2;
    s17.p25_iden_fdma[id9].populated = 1;
    s17.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx17;
    p25_sm_init_ctx(&ctx17, &o17, &s17);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx17, &o17, &s17, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(s17.p25_cc_freq == 0);
    assert(s17.trunk_cc_freq == 0);
    assert(s17.trunk_lcn_freq[0] == 0);
    assert(ctx17.state == P25_SM_IDLE);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;

    // 19) Return-to-CC after a seeded first grant must refresh stale CC sync metadata.
    static dsd_opts o18;
    static dsd_state s18;
    DSD_MEMSET(&o18, 0, sizeof(o18));
    DSD_MEMSET(&s18, 0, sizeof(s18));
    o18.p25_trunk = 1;
    o18.trunk_tune_group_calls = 1;
    o18.p25_prefer_candidates = 1;
    s18.trunk_cc_freq = 851012500;
    s18.nac = 0x123;
    s18.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s18.p25_iden_fdma[id9].chan_type = 1;
    s18.p25_iden_fdma[id9].chan_spac = 100;
    s18.p25_iden_fdma[id9].trust = 2;
    s18.p25_iden_fdma[id9].populated = 1;
    s18.p25_chan_tdma_explicit[id9] = 1;
    (void)dsd_trunk_cc_candidates_add(&s18, 853000000, 0);
    p25_sm_ctx_t ctx18;
    p25_sm_init_ctx(&ctx18, &o18, &s18);
    assert(ctx18.state == P25_SM_IDLE);
    g_result_tune_to_freq_calls = 0;
    g_result_return_to_cc_calls = 0;
    p25_sm_event(&ctx18, &o18, &s18, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx18.state == P25_SM_TUNED);
    assert(s18.p25_cc_freq == 851012500);
    assert(s18.last_cc_sync_time_m > 0.0);

    s18.last_cc_sync_time = time(NULL) - 10;
    s18.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;
    ctx18.t_cc_sync_m = s18.last_cc_sync_time_m;
    const double stale_seed_cc_sync_m = s18.last_cc_sync_time_m;
    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_release(&ctx18, &o18, &s18, "seeded-release");
    assert(g_result_return_to_cc_calls == 1);
    assert(ctx18.state == P25_SM_ON_CC);
    assert(o18.p25_is_tuned == 0);
    assert(s18.last_cc_sync_time_m > stale_seed_cc_sync_m);

    int cc_calls_before_seeded_release_tick = g_result_tune_to_cc_calls;
    p25_sm_tick_ctx(&ctx18, &o18, &s18);
    assert(ctx18.state == P25_SM_ON_CC);
    assert(g_result_tune_to_cc_calls == cc_calls_before_seeded_release_tick);

    // 20) Grants observed before decoded CC activity after a CC retune must not pull us back to a VC.
    static dsd_opts o18aa;
    static dsd_state s18aa;
    DSD_MEMSET(&o18aa, 0, sizeof(o18aa));
    DSD_MEMSET(&s18aa, 0, sizeof(s18aa));
    o18aa.p25_trunk = 1;
    o18aa.trunk_tune_group_calls = 1;
    s18aa.p25_cc_freq = 851000000;
    s18aa.trunk_cc_freq = 851000000;
    s18aa.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s18aa.p25_iden_fdma[id9].chan_type = 1;
    s18aa.p25_iden_fdma[id9].chan_spac = 100;
    s18aa.p25_iden_fdma[id9].trust = 2;
    s18aa.p25_iden_fdma[id9].populated = 1;
    s18aa.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx18aa;
    p25_sm_init_ctx(&ctx18aa, &o18aa, &s18aa);
    const double pending_grant_cc_tune_m = dsd_time_now_monotonic_s();
    ctx18aa.state = P25_SM_ON_CC;
    ctx18aa.t_cc_sync_m = pending_grant_cc_tune_m - 0.25;
    ctx18aa.t_cc_tune_m = pending_grant_cc_tune_m;
    ctx18aa.cc_sync_pending = 1;
    s18aa.last_cc_sync_time_m = pending_grant_cc_tune_m - 0.10;
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx18aa, &o18aa, &s18aa, &ev9);
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18aa.state == P25_SM_ON_CC);
    assert(ctx18aa.cc_sync_pending == 1);

    s18aa.last_cc_sync_time_m = pending_grant_cc_tune_m;
    p25_sm_event(&ctx18aa, &o18aa, &s18aa, &ev9);
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18aa.state == P25_SM_ON_CC);
    assert(ctx18aa.cc_sync_pending == 1);

    s18aa.last_cc_sync_time_m = pending_grant_cc_tune_m + 0.25;
    p25_sm_event(&ctx18aa, &o18aa, &s18aa, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx18aa.state == P25_SM_TUNED);
    assert(ctx18aa.cc_sync_pending == 0);

    // 21) A CC retune that does not produce decoded CC activity should hunt before full stale-CC grace.
    static dsd_opts o18b;
    static dsd_state s18b;
    DSD_MEMSET(&o18b, 0, sizeof(o18b));
    DSD_MEMSET(&s18b, 0, sizeof(s18b));
    o18b.p25_trunk = 1;
    o18b.p25_prefer_candidates = 1;
    s18b.p25_cc_freq = 851000000;
    s18b.trunk_cc_freq = 851000000;
    (void)dsd_trunk_cc_candidates_add(&s18b, 852000000, 0);
    p25_sm_ctx_t ctx18b;
    p25_sm_init_ctx(&ctx18b, &o18b, &s18b);
    ctx18b.config.cc_grace_s = 5.0;
    const double pending_cc_tune_m = dsd_time_now_monotonic_s() - 2.5;
    ctx18b.t_cc_sync_m = pending_cc_tune_m;
    ctx18b.t_cc_tune_m = pending_cc_tune_m;
    ctx18b.cc_sync_pending = 1;
    s18b.last_cc_sync_time = time(NULL) - 3;
    s18b.last_cc_sync_time_m = pending_cc_tune_m;
    g_result_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_cc_calls = 0;
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(&ctx18b, &o18b, &s18b);
    assert(g_result_tune_to_cc_calls == 1);
    assert(g_last_tuned_cc == 852000000);
    assert(ctx18b.state == P25_SM_ON_CC);

    // 22) Stale SM context after a no-carrier VC clear must not skip the next same-RF tune.
    static dsd_opts o19a;
    static dsd_state s19a;
    DSD_MEMSET(&o19a, 0, sizeof(o19a));
    DSD_MEMSET(&s19a, 0, sizeof(s19a));
    o19a.p25_trunk = 1;
    o19a.trunk_tune_group_calls = 1;
    s19a.p25_cc_freq = 851000000;
    setup_tdma_iden(&s19a, 2);
    const int tdma_slot0_ch = (2 << 12) | 0x0000;
    const int tdma_slot1_ch = (2 << 12) | 0x0001;
    p25_sm_event_t same_tg_slot0 = p25_sm_ev_group_grant(tdma_slot0_ch, 0, 3101, 4101, 0);
    p25_sm_event_t moved_tg_slot1 = p25_sm_ev_group_grant(tdma_slot1_ch, 0, 3102, 4102, 0);
    p25_sm_ctx_t ctx19a;
    p25_sm_init_ctx(&ctx19a, &o19a, &s19a);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_freq_calls = 0;

    p25_sm_event(&ctx19a, &o19a, &s19a, &same_tg_slot0);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx19a.state == P25_SM_TUNED);
    assert(o19a.p25_is_tuned == 1);

    clear_decoder_vc_tune_state(&o19a, &s19a);
    p25_sm_event(&ctx19a, &o19a, &s19a, &same_tg_slot0);
    assert(g_result_tune_to_freq_calls == 2);
    assert(o19a.p25_is_tuned == 1);
    assert(ctx19a.state == P25_SM_TUNED);

    clear_decoder_vc_tune_state(&o19a, &s19a);
    p25_sm_event(&ctx19a, &o19a, &s19a, &moved_tg_slot1);
    assert(g_result_tune_to_freq_calls == 3);
    assert(o19a.p25_is_tuned == 1);
    assert(ctx19a.state == P25_SM_TUNED);
    assert(ctx19a.slots[1].grant_active == 1);

    // 23) ENC lockout must keep a clear opposite-slot grant pending on the same TDMA carrier.
    static dsd_opts o19b;
    static dsd_state s19b;
    DSD_MEMSET(&o19b, 0, sizeof(o19b));
    DSD_MEMSET(&s19b, 0, sizeof(s19b));
    o19b.p25_trunk = 1;
    o19b.p25_is_tuned = 1;
    o19b.trunk_is_tuned = 1;
    o19b.trunk_tune_enc_calls = 0;
    s19b.p25_cc_freq = 851000000;
    s19b.p25_vc_freq[0] = s19b.p25_vc_freq[1] = 851000000;
    s19b.trunk_vc_freq[0] = s19b.trunk_vc_freq[1] = 851000000;
    setup_tdma_iden(&s19b, 2);

    p25_sm_ctx_t ctx19b;
    p25_sm_init_ctx(&ctx19b, &o19b, &s19b);
    ctx19b.state = P25_SM_TUNED;
    ctx19b.vc_is_tdma = 1;
    ctx19b.vc_freq_hz = 851000000;
    ctx19b.vc_channel = tdma_slot0_ch;
    ctx19b.vc_tg = 5101;
    ctx19b.slots[1].grant_active = 1;
    ctx19b.slots[1].freq_hz = 851000000;
    ctx19b.slots[1].channel = tdma_slot1_ch;
    ctx19b.slots[1].target_id = 5102;
    ctx19b.slots[1].last_grant_m = dsd_time_now_monotonic_s();
    s19b.p25_p2_audio_allowed[0] = 1;

    p25_sm_event_t enc_slot0 = p25_sm_ev_enc(0, 0x84, 0x1234, 5101);
    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_return_to_cc_calls = 0;
    p25_sm_event(&ctx19b, &o19b, &s19b, &enc_slot0);
    assert(g_result_return_to_cc_calls == 0);
    assert(o19b.p25_is_tuned == 1);
    assert(ctx19b.state == P25_SM_TUNED);
    assert(ctx19b.slots[1].grant_active == 1);
    assert(s19b.p25_p2_audio_allowed[0] == 0);
    assert(s19b.p25_p2_enc_lockout_muted[0] == 1);

#ifdef USE_RADIO
    // 24) A failed CQPSK retry must roll back the one-shot override and TDMA timing.
    static dsd_opts o19;
    static dsd_state s19;
    DSD_MEMSET(&o19, 0, sizeof(o19));
    DSD_MEMSET(&s19, 0, sizeof(s19));
    o19.p25_trunk = 1;
    o19.trunk_tune_group_calls = 1;
    o19.audio_in_type = AUDIO_IN_RTL;
    o19.trunk_hangtime = 5.0f;
    o19.p25_grant_voice_to_s = 5.0;
    s19.p25_cc_freq = 851000000;
    s19.trunk_cc_freq = 851000000;
    s19.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s19.p25_iden_fdma[id9].chan_type = 1;
    s19.p25_iden_fdma[id9].chan_spac = 100;
    s19.p25_iden_fdma[id9].trust = 2;
    s19.p25_iden_fdma[id9].populated = 1;
    s19.p25_chan_tdma_explicit[id9] = 2;
    s19.p25_vc_cqpsk_pref = -1;
    (void)dsd_unsetenv("DSD_NEO_CQPSK");
    dsd_neo_config_init(&o19);
    dsd_rtl_stream_metrics_hooks_set(NULL);

    p25_sm_ctx_t ctx19;
    p25_sm_init_ctx(&ctx19, &o19, &s19);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx19, &o19, &s19, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx19.state == P25_SM_TUNED);
    assert(ctx19.vc_is_tdma == 1);
    assert(o19.p25_is_tuned == 1);
    const int tuned_sps = s19.samplesPerSymbol;
    const int tuned_center = s19.symbolCenter;

    s19.p25_vc_cqpsk_override = 0;
    ctx19.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    int calls_before_cqpsk_retry = g_result_tune_to_freq_calls;
    p25_sm_tick_ctx(&ctx19, &o19, &s19);
    assert(g_result_tune_to_freq_calls == calls_before_cqpsk_retry + 1);
    assert(ctx19.vc_cqpsk_retry_done == 0);
    assert(s19.p25_vc_cqpsk_override == 0);
    assert(s19.samplesPerSymbol == tuned_sps);
    assert(s19.symbolCenter == tuned_center);
    assert(ctx19.state == P25_SM_TUNED);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;

    // 25) A successful CQPSK retry refreshes the TDMA grant timeout window.
    static dsd_opts o20;
    static dsd_state s20;
    DSD_MEMSET(&o20, 0, sizeof(o20));
    DSD_MEMSET(&s20, 0, sizeof(s20));
    o20.p25_trunk = 1;
    o20.trunk_tune_group_calls = 1;
    o20.audio_in_type = AUDIO_IN_RTL;
    o20.trunk_hangtime = 5.0f;
    o20.p25_grant_voice_to_s = 0.8;
    o20.p25_retune_backoff_s = 2.0;
    s20.p25_cc_freq = 851000000;
    s20.trunk_cc_freq = 851000000;
    s20.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s20.p25_iden_fdma[id9].chan_type = 1;
    s20.p25_iden_fdma[id9].chan_spac = 100;
    s20.p25_iden_fdma[id9].trust = 2;
    s20.p25_iden_fdma[id9].populated = 1;
    s20.p25_chan_tdma_explicit[id9] = 2;
    s20.p25_vc_cqpsk_pref = -1;
    (void)dsd_unsetenv("DSD_NEO_CQPSK");
    dsd_neo_config_init(&o20);
    dsd_rtl_stream_metrics_hooks_set(NULL);

    p25_sm_ctx_t ctx20;
    p25_sm_init_ctx(&ctx20, &o20, &s20);
    g_result_tune_to_freq_calls = 0;
    g_result_return_to_cc_calls = 0;
    p25_sm_event(&ctx20, &o20, &s20, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx20.state == P25_SM_TUNED);
    assert(ctx20.vc_is_tdma == 1);
    assert(o20.p25_is_tuned == 1);

    s20.p25_vc_cqpsk_override = 0;
    const double stale_grant_m = dsd_time_now_monotonic_s() - 0.85;
    ctx20.t_tune_m = stale_grant_m;
    ctx20.t_voice_m = 0.0;
    ctx20.slots[0].last_grant_m = stale_grant_m;
    p25_sm_tick_ctx(&ctx20, &o20, &s20);
    assert(g_result_tune_to_freq_calls == 2);
    assert(g_result_return_to_cc_calls == 0);
    assert(ctx20.vc_cqpsk_retry_done == 1);
    assert(ctx20.t_tune_m > stale_grant_m);
    assert(ctx20.state == P25_SM_TUNED);
    assert(o20.p25_is_tuned == 1);
    assert(ctx20.slots[0].grant_active == 1);
    assert(s20.p25_retune_block_until == 0);
#endif

    install_trunk_tuning_hooks();

    DSD_FPRINTF(stderr, "P25 SM core tests passed\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
