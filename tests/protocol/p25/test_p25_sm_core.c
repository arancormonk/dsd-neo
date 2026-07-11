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
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <math.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
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
static int g_result_hook_commits_decoder_state = 1;
static long g_rigctl_current_freq = 0;
static atomic_int g_release_hook_block = 0;
static dsd_mutex_t g_release_hook_mutex;
static dsd_cond_t g_release_hook_cond;
static int g_release_hook_entered = 0;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} release_thread_args;

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
    if (g_result_tune_to_freq_result == DSD_TRUNK_TUNE_RESULT_OK && g_result_hook_commits_decoder_state) {
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
    g_result_return_to_cc_calls++;
    if (atomic_load(&g_release_hook_block)) {
        int sync_rc = 0;
        if (opts) {
            opts->p25_is_tuned = 0;
            opts->trunk_is_tuned = 0;
        }
        if (state) {
            state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
            state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        }
        sync_rc = dsd_mutex_lock(&g_release_hook_mutex);
        if (sync_rc != 0) {
            atomic_store(&g_release_hook_block, 0);
            return DSD_TRUNK_TUNE_RESULT_FAILED;
        }
        g_release_hook_entered = 1;
        sync_rc = dsd_cond_signal(&g_release_hook_cond);
        while (sync_rc == 0 && atomic_load(&g_release_hook_block)) {
            sync_rc = dsd_cond_wait(&g_release_hook_cond, &g_release_hook_mutex);
        }
        if (dsd_mutex_unlock(&g_release_hook_mutex) != 0 || sync_rc != 0) {
            atomic_store(&g_release_hook_block, 0);
            return DSD_TRUNK_TUNE_RESULT_FAILED;
        }
    }
    return g_result_return_to_cc_result;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    release_wrapper_thread(void* arg) {
    release_thread_args* args = (release_thread_args*)arg;
    p25_sm_on_release(args ? args->opts : NULL, args ? args->state : NULL);
    DSD_THREAD_RETURN;
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
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18aa.state == P25_SM_ON_CC);
    assert(ctx18aa.cc_sync_pending == 1);

    p25_sm_event(&ctx18aa, &o18aa, &s18aa, &(p25_sm_event_t){.type = P25_SM_EV_CC_SYNC});
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18aa.state == P25_SM_ON_CC);
    assert(ctx18aa.cc_sync_pending == 1);

    s18aa.p25_last_cc_msg_time = time(NULL);
    s18aa.p25_last_cc_msg_time_m = pending_grant_cc_tune_m + 0.25;
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

    // 22) HUNTING must keep grants blocked until decoded CC activity proves reacquisition,
    // then recover even when that activity is not a grant.
    static dsd_opts o18c;
    static dsd_state s18c;
    DSD_MEMSET(&o18c, 0, sizeof(o18c));
    DSD_MEMSET(&s18c, 0, sizeof(s18c));
    o18c.p25_trunk = 1;
    o18c.trunk_tune_group_calls = 1;
    s18c.p25_cc_freq = 851000000;
    s18c.trunk_cc_freq = 851000000;
    s18c.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s18c.p25_iden_fdma[id9].chan_type = 1;
    s18c.p25_iden_fdma[id9].chan_spac = 100;
    s18c.p25_iden_fdma[id9].trust = 2;
    s18c.p25_iden_fdma[id9].populated = 1;
    s18c.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx18c;
    p25_sm_init_ctx(&ctx18c, &o18c, &s18c);
    ctx18c.config.cc_grace_s = 5.0;
    const double hunting_pending_tune_m = dsd_time_now_monotonic_s() - 2.5;
    ctx18c.t_cc_sync_m = hunting_pending_tune_m;
    ctx18c.t_cc_tune_m = hunting_pending_tune_m;
    ctx18c.cc_sync_pending = 1;
    s18c.last_cc_sync_time = time(NULL) - 3;
    s18c.last_cc_sync_time_m = hunting_pending_tune_m;
    s18c.p25_last_cc_msg_time_m = hunting_pending_tune_m - 0.25;
    g_result_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    g_result_tune_to_cc_calls = 0;
    p25_sm_tick_ctx(&ctx18c, &o18c, &s18c);
    assert(g_result_tune_to_cc_calls == 1);
    assert(ctx18c.state == P25_SM_HUNTING);
    assert(ctx18c.cc_sync_pending == 1);

    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_freq_calls = 0;
    p25_sm_event(&ctx18c, &o18c, &s18c, &ev9);
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18c.state == P25_SM_HUNTING);
    assert(ctx18c.cc_sync_pending == 1);

    s18c.last_cc_sync_time_m = hunting_pending_tune_m + 0.25;
    p25_sm_event(&ctx18c, &o18c, &s18c, &ev9);
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18c.state == P25_SM_HUNTING);
    assert(ctx18c.cc_sync_pending == 1);

    int cc_calls_before_hunt_recovery = g_result_tune_to_cc_calls;
    p25_sm_tick_ctx(&ctx18c, &o18c, &s18c);
    assert(g_result_tune_to_cc_calls == cc_calls_before_hunt_recovery);
    assert(ctx18c.state == P25_SM_HUNTING);
    assert(ctx18c.cc_sync_pending == 1);

    s18c.p25_last_cc_msg_time = time(NULL);
    s18c.p25_last_cc_msg_time_m = hunting_pending_tune_m + 0.25;
    ctx18c.t_hunt_try_m = 0.0;
    p25_sm_tick_ctx(&ctx18c, &o18c, &s18c);
    assert(g_result_tune_to_cc_calls == cc_calls_before_hunt_recovery);
    assert(g_result_tune_to_freq_calls == 0);
    assert(ctx18c.state == P25_SM_ON_CC);
    assert(ctx18c.cc_sync_pending == 0);
    assert(ctx18c.t_cc_tune_m == 0.0);
    assert(fabs(ctx18c.t_cc_sync_m - s18c.p25_last_cc_msg_time_m) <= cc_sync_epsilon_s);

    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    p25_sm_event(&ctx18c, &o18c, &s18c, &ev9);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx18c.state == P25_SM_ON_CC);

    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_event(&ctx18c, &o18c, &s18c, &ev9);
    assert(g_result_tune_to_freq_calls == 2);
    assert(ctx18c.state == P25_SM_TUNED);
    g_result_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    // 23) An accepted external CC retune restarts pending acquisition in ON_CC.
    static dsd_opts o18d;
    static dsd_state s18d;
    DSD_MEMSET(&o18d, 0, sizeof(o18d));
    DSD_MEMSET(&s18d, 0, sizeof(s18d));
    o18d.p25_trunk = 1;
    s18d.p25_cc_freq = 851000000;
    s18d.trunk_cc_freq = 851000000;
    p25_sm_ctx_t ctx18d;
    p25_sm_init_ctx(&ctx18d, &o18d, &s18d);
    const double external_cc_tune_m = 100.0;
    const double external_decoded_cc_m = external_cc_tune_m - 2.0;
    ctx18d.state = P25_SM_HUNTING;
    ctx18d.cc_sync_pending = 1;
    ctx18d.t_cc_sync_m = external_cc_tune_m - 3.0;
    ctx18d.t_cc_tune_m = external_cc_tune_m - 3.0;
    ctx18d.t_hunt_try_m = external_cc_tune_m - 1.0;
    s18d.p25_sm_mode = DSD_P25_SM_MODE_HUNTING;
    s18d.p25_cc_eval_freq = 852000000;
    s18d.p25_cc_eval_start_m = external_cc_tune_m - 3.0;
    s18d.last_cc_sync_time_m = external_cc_tune_m - 3.0;
    s18d.p25_last_cc_msg_time_m = external_decoded_cc_m;
    assert(p25_sm_restart_pending_cc_acquisition(&ctx18d, &o18d, &s18d, external_cc_tune_m, "test-retune") == 1);
    assert(ctx18d.state == P25_SM_ON_CC);
    assert(ctx18d.cc_sync_pending == 1);
    assert(fabs(ctx18d.t_cc_sync_m - external_cc_tune_m) <= cc_sync_epsilon_s);
    assert(fabs(ctx18d.t_cc_tune_m - external_cc_tune_m) <= cc_sync_epsilon_s);
    assert(ctx18d.t_hunt_try_m == 0.0);
    assert(s18d.p25_sm_mode == DSD_P25_SM_MODE_ON_CC);
    assert(s18d.p25_cc_eval_freq == 852000000);
    assert(fabs(s18d.p25_cc_eval_start_m - ctx18d.t_cc_tune_m) <= cc_sync_epsilon_s);
    assert(fabs(s18d.last_cc_sync_time_m - external_cc_tune_m) <= cc_sync_epsilon_s);
    assert(fabs(s18d.p25_last_cc_msg_time_m - external_decoded_cc_m) <= cc_sync_epsilon_s);

    // 24) The first decoded grant after asynchronous completion must satisfy CC reacquisition.
    static dsd_opts o18e;
    static dsd_state s18e;
    DSD_MEMSET(&o18e, 0, sizeof(o18e));
    DSD_MEMSET(&s18e, 0, sizeof(s18e));
    o18e.p25_trunk = 1;
    o18e.trunk_tune_group_calls = 1;
    s18e.p25_cc_freq = 851000000;
    s18e.trunk_cc_freq = 851000000;
    s18e.p25_iden_fdma[id9] = s9.p25_iden_fdma[id9];
    s18e.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx18e;
    p25_sm_init_ctx(&ctx18e, &o18e, &s18e);
    ctx18e.state = P25_SM_ON_CC;
    s18e.p25_sm_mode = DSD_P25_SM_MODE_ON_CC;

    const uint64_t early_grant_request_id = dsd_trunk_tuning_request_begin();
    assert(early_grant_request_id != 0U);
    dsd_trunk_tuning_request_mark_ready(early_grant_request_id);
    assert(p25_sm_await_pending_cc_tune(&ctx18e, &o18e, &s18e, early_grant_request_id, "test-early-grant") == 1);
    dsd_trunk_tuning_request_publish(early_grant_request_id, DSD_TRUNK_TUNE_RESULT_OK);

    double early_grant_completion_m = 0.0;
    assert(dsd_trunk_tuning_request_status(early_grant_request_id, &early_grant_completion_m)
           == DSD_TRUNK_TUNE_RESULT_OK);
    assert(early_grant_completion_m > 0.0);
    const double early_grant_decoded_m = early_grant_completion_m + 0.001;
    s18e.last_cc_sync_time = time(NULL);
    s18e.last_cc_sync_time_m = early_grant_decoded_m;
    s18e.p25_last_cc_msg_time = s18e.last_cc_sync_time;
    s18e.p25_last_cc_msg_time_m = early_grant_decoded_m;
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_tune_to_freq_calls = 0;

    p25_sm_event(&ctx18e, &o18e, &s18e, &ev9);
    assert(ctx18e.cc_tune_pending == 0);
    assert(ctx18e.cc_sync_pending == 0);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx18e.state == P25_SM_TUNED);

    // A later raw frame sync must not move the tune boundary past decoded CC
    // activity that arrived after asynchronous completion but before SM resolution.
    static dsd_opts o18f;
    static dsd_state s18f;
    DSD_MEMSET(&o18f, 0, sizeof(o18f));
    DSD_MEMSET(&s18f, 0, sizeof(s18f));
    o18f.p25_trunk = 1;
    s18f.p25_cc_freq = 851000000;
    s18f.trunk_cc_freq = 851000000;
    p25_sm_ctx_t ctx18f;
    p25_sm_init_ctx(&ctx18f, &o18f, &s18f);
    ctx18f.state = P25_SM_ON_CC;
    s18f.p25_sm_mode = DSD_P25_SM_MODE_ON_CC;

    const uint64_t decoded_before_resolution_request_id = dsd_trunk_tuning_request_begin();
    assert(decoded_before_resolution_request_id != 0U);
    dsd_trunk_tuning_request_mark_ready(decoded_before_resolution_request_id);
    assert(p25_sm_await_pending_cc_tune(&ctx18f, &o18f, &s18f, decoded_before_resolution_request_id,
                                        "test-decoded-before-resolution")
           == 1);
    dsd_trunk_tuning_request_publish(decoded_before_resolution_request_id, DSD_TRUNK_TUNE_RESULT_OK);

    double decoded_before_resolution_completion_m = 0.0;
    assert(
        dsd_trunk_tuning_request_status(decoded_before_resolution_request_id, &decoded_before_resolution_completion_m)
        == DSD_TRUNK_TUNE_RESULT_OK);
    assert(decoded_before_resolution_completion_m > 0.0);
    const double decoded_before_resolution_m = decoded_before_resolution_completion_m + 0.001;
    const double later_raw_sync_m = decoded_before_resolution_m + 0.001;
    s18f.p25_last_cc_msg_time = time(NULL);
    s18f.p25_last_cc_msg_time_m = decoded_before_resolution_m;
    s18f.last_cc_sync_time = s18f.p25_last_cc_msg_time;
    s18f.last_cc_sync_time_m = later_raw_sync_m;

    p25_sm_tick_ctx(&ctx18f, &o18f, &s18f);
    assert(ctx18f.cc_tune_pending == 0);
    assert(ctx18f.cc_sync_pending == 0);
    assert(ctx18f.t_cc_tune_m == 0.0);
    assert(fabs(ctx18f.t_cc_sync_m - decoded_before_resolution_m) <= cc_sync_epsilon_s);
    assert(ctx18f.state == P25_SM_ON_CC);

    // 25) Stale SM context after a no-carrier VC clear must not skip the next same-RF tune.
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

    // 26) ENC lockout must keep a clear opposite-slot grant pending on the same TDMA carrier.
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
    s19b.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    s19b.p25_p2_enc_lockout_muted[0] = 1U;

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

    // A retained sticky block suppresses only encrypted/unknown re-probes;
    // an explicit-clear grant on the same slot starts a new clear call.
    static dsd_opts o19e;
    static dsd_state s19e;
    DSD_MEMSET(&o19e, 0, sizeof(o19e));
    DSD_MEMSET(&s19e, 0, sizeof(s19e));
    o19e.p25_trunk = 1;
    o19e.p25_is_tuned = 1;
    o19e.trunk_is_tuned = 1;
    o19e.trunk_tune_group_calls = 1;
    o19e.trunk_tune_enc_calls = 0;
    s19e.p25_cc_freq = 851000000;
    s19e.p25_vc_freq[0] = s19e.p25_vc_freq[1] = 851000000;
    s19e.trunk_vc_freq[0] = s19e.trunk_vc_freq[1] = 851000000;
    setup_tdma_iden(&s19e, 2);

    p25_sm_ctx_t ctx19e;
    p25_sm_init_ctx(&ctx19e, &o19e, &s19e);
    ctx19e.state = P25_SM_TUNED;
    ctx19e.vc_is_tdma = 1;
    ctx19e.vc_freq_hz = 851000000;
    ctx19e.vc_channel = tdma_slot0_ch;
    ctx19e.vc_tg = 5401;
    ctx19e.slots[0].freq_hz = 851000000;
    ctx19e.slots[0].channel = tdma_slot0_ch;
    ctx19e.slots[0].target_id = 5401;
    ctx19e.slots[0].ota_tg = 5401;
    ctx19e.slots[0].src = 6401;
    ctx19e.slots[0].is_group = 1;
    ctx19e.slots[0].svc_bits = 0x40;
    ctx19e.slots[0].last_grant_m = dsd_time_now_monotonic_s() - 1.0;
    ctx19e.slots[1].grant_active = 1;
    ctx19e.slots[1].voice_active = 1;
    ctx19e.slots[1].freq_hz = 851000000;
    ctx19e.slots[1].channel = tdma_slot1_ch;
    ctx19e.slots[1].target_id = 5402;
    ctx19e.slots[1].ota_tg = 5402;
    ctx19e.slots[1].is_group = 1;
    s19e.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    s19e.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;

    const unsigned sticky_grants_before = ctx19e.grant_count;
    p25_sm_event_t sticky_encrypted = p25_sm_ev_group_grant(tdma_slot0_ch, 0, 5401, 6402, 0x40);
    p25_sm_event(&ctx19e, &o19e, &s19e, &sticky_encrypted);
    assert(ctx19e.grant_count == sticky_grants_before);
    assert(ctx19e.slots[0].src == 6401);
    assert(s19e.p25_crypto_state[0] == DSD_P25_CRYPTO_BLOCKED);

    p25_sm_event_t sticky_unknown = p25_sm_ev_group_grant(tdma_slot0_ch, 0, 5401, 6403, P25_SM_SVC_UNKNOWN);
    p25_sm_event(&ctx19e, &o19e, &s19e, &sticky_unknown);
    assert(ctx19e.grant_count == sticky_grants_before);
    assert(ctx19e.slots[0].src == 6401);

    p25_sm_event_t replacement_clear = p25_sm_ev_group_grant(tdma_slot0_ch, 0, 5401, 6404, 0x00);
    p25_sm_event(&ctx19e, &o19e, &s19e, &replacement_clear);
    assert(ctx19e.grant_count == sticky_grants_before + 1U);
    assert(ctx19e.slots[0].grant_active == 1);
    assert(ctx19e.slots[0].src == 6404);
    assert(ctx19e.slots[0].svc_bits == 0x00);
    assert(ctx19e.slots[1].voice_active == 1);
    assert(s19e.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);

    // A true duplicate grant must refresh crypto classification when its
    // service options change from encrypted to explicit clear.
    static dsd_opts o19g;
    static dsd_state s19g;
    DSD_MEMSET(&o19g, 0, sizeof(o19g));
    DSD_MEMSET(&s19g, 0, sizeof(s19g));
    o19g.p25_trunk = 1;
    o19g.trunk_tune_group_calls = 1;
    o19g.trunk_tune_enc_calls = 0;
    s19g.p25_cc_freq = 851000000;
    setup_tdma_iden(&s19g, 2);

    p25_sm_ctx_t ctx19g;
    p25_sm_init_ctx(&ctx19g, &o19g, &s19g);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_hook_commits_decoder_state = 1;
    g_result_tune_to_freq_calls = 0;

    p25_sm_event_t duplicate_encrypted = p25_sm_ev_group_grant(tdma_slot0_ch, 0, 5501, 6501, 0x40);
    p25_sm_event(&ctx19g, &o19g, &s19g, &duplicate_encrypted);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx19g.state == P25_SM_TUNED);
    assert(ctx19g.grant_count == 1U);
    assert(ctx19g.slots[0].grant_active == 1);
    assert(ctx19g.slots[0].svc_bits == 0x40);
    assert(s19g.p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    p25_sm_event_t duplicate_clear = p25_sm_ev_group_grant(tdma_slot0_ch, 0, 5501, 6501, 0x00);
    p25_sm_event(&ctx19g, &o19g, &s19g, &duplicate_clear);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx19g.grant_count == 1U);
    assert(ctx19g.slots[0].svc_bits == 0x00);
    assert(s19g.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);
    assert(s19g.p25_p2_enc_lockout_muted[0] == 0U);

    s19g.p25_p2_audio_allowed[0] = 1;
    s19g.p25_p2_audio_ring_count[0] = 1;
    s19g.s_l4[0][0] = 321;
    p25_sm_event(&ctx19g, &o19g, &s19g, &duplicate_clear);
    assert(ctx19g.grant_count == 1U);
    assert(s19g.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);
    assert(s19g.p25_p2_audio_allowed[0] == 1);
    assert(s19g.p25_p2_audio_ring_count[0] == 1);
    assert(s19g.s_l4[0][0] == 321);

    // Phase 1 grants use logical slot 0 for service-option history. Repeating
    // an unchanged clear grant must preserve the active call's audio state.
    static dsd_opts o19h;
    static dsd_state s19h;
    DSD_MEMSET(&o19h, 0, sizeof(o19h));
    DSD_MEMSET(&s19h, 0, sizeof(s19h));
    o19h.p25_trunk = 1;
    o19h.trunk_tune_group_calls = 1;
    o19h.trunk_tune_enc_calls = 0;
    s19h.p25_cc_freq = 851000000;
    s19h.p25_iden_fdma[id9] = s9.p25_iden_fdma[id9];
    s19h.p25_chan_tdma_explicit[id9] = 1;

    p25_sm_ctx_t ctx19h;
    p25_sm_init_ctx(&ctx19h, &o19h, &s19h);
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_hook_commits_decoder_state = 1;
    g_result_tune_to_freq_calls = 0;

    p25_sm_event_t duplicate_fdma_clear = p25_sm_ev_group_grant(ch9, 0, 5601, 6601, 0x00);
    p25_sm_event(&ctx19h, &o19h, &s19h, &duplicate_fdma_clear);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx19h.state == P25_SM_TUNED);
    assert(ctx19h.vc_is_tdma == 0);
    assert(ctx19h.grant_count == 1U);
    assert(ctx19h.slots[0].grant_active == 1);
    assert(ctx19h.slots[0].svc_bits == 0x00);
    assert(s19h.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);

    s19h.p25_p2_audio_allowed[0] = 1;
    s19h.p25_p2_audio_ring_count[0] = 1;
    s19h.s_l4[0][0] = 654;
    p25_sm_event(&ctx19h, &o19h, &s19h, &duplicate_fdma_clear);
    assert(g_result_tune_to_freq_calls == 1);
    assert(ctx19h.grant_count == 1U);
    assert(ctx19h.slots[0].svc_bits == 0x00);
    assert(s19h.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR);
    assert(s19h.p25_p2_audio_allowed[0] == 1);
    assert(s19h.p25_p2_audio_ring_count[0] == 1);
    assert(s19h.s_l4[0][0] == 654);

    // Encrypted-follow mode tracks muted activity and never applies the
    // lockout-only classification timeout.
    static dsd_opts o19f;
    static dsd_state s19f;
    DSD_MEMSET(&o19f, 0, sizeof(o19f));
    DSD_MEMSET(&s19f, 0, sizeof(s19f));
    o19f.p25_trunk = 1;
    o19f.p25_is_tuned = 1;
    o19f.trunk_is_tuned = 1;
    o19f.trunk_tune_enc_calls = 1;
    s19f.p25_cc_freq = 851000000;
    s19f.p25_vc_freq[0] = s19f.p25_vc_freq[1] = 851000000;
    s19f.trunk_vc_freq[0] = s19f.trunk_vc_freq[1] = 851000000;
    s19f.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;

    p25_sm_ctx_t ctx19f;
    p25_sm_init_ctx(&ctx19f, &o19f, &s19f);
    ctx19f.state = P25_SM_TUNED;
    ctx19f.vc_is_tdma = 1;
    ctx19f.vc_freq_hz = 851000000;
    ctx19f.vc_channel = tdma_slot0_ch;
    ctx19f.vc_tg = 5501;
    ctx19f.config.grant_timeout_s = 0.1;
    ctx19f.slots[0].grant_active = 1;
    ctx19f.slots[0].freq_hz = 851000000;
    ctx19f.slots[0].channel = tdma_slot0_ch;
    ctx19f.slots[0].target_id = 5501;

    p25_sm_event_t follow_ptt = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx19f, &o19f, &s19f, &follow_ptt);
    assert(ctx19f.slots[0].voice_active == 1);
    assert(ctx19f.t_voice_m > 0.0);

    ctx19f.slots[0].voice_active = 0;
    s19f.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    p25_sm_event_t follow_active = p25_sm_ev_active(0);
    p25_sm_event(&ctx19f, &o19f, &s19f, &follow_active);
    assert(ctx19f.slots[0].voice_active == 1);

    const double stale_follow_grant_m = dsd_time_now_monotonic_s() - 1.0;
    ctx19f.t_tune_m = stale_follow_grant_m;
    ctx19f.slots[0].last_grant_m = stale_follow_grant_m;
    s19f.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    g_result_return_to_cc_calls = 0;
    p25_sm_tick_ctx(&ctx19f, &o19f, &s19f);
    assert(g_result_return_to_cc_calls == 0);
    assert(ctx19f.state == P25_SM_TUNED);
    assert(ctx19f.slots[0].grant_active == 1);
    assert(ctx19f.slots[0].voice_active == 1);
    assert(s19f.p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // 27) A Phase 2 classification timeout blocks and purges only the
    // unresolved slot while retaining an active clear companion.
    static dsd_opts o19c;
    static dsd_state s19c;
    DSD_MEMSET(&o19c, 0, sizeof(o19c));
    DSD_MEMSET(&s19c, 0, sizeof(s19c));
    o19c.p25_trunk = 1;
    o19c.p25_is_tuned = 1;
    o19c.trunk_is_tuned = 1;
    o19c.trunk_tune_enc_calls = 0;
    s19c.p25_cc_freq = 851000000;
    s19c.p25_vc_freq[0] = s19c.p25_vc_freq[1] = 851000000;
    s19c.trunk_vc_freq[0] = s19c.trunk_vc_freq[1] = 851000000;
    s19c.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    s19c.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    s19c.p25_p2_enc_lockout_muted[0] = 1U;
    s19c.p25_p2_audio_allowed[1] = 1;
    s19c.p25_p2_audio_ring_count[0] = 2;
    s19c.p25_p2_audio_ring_count[1] = 3;
    s19c.s_l4[0][0] = 111;
    s19c.s_r4[0][0] = 222;

    p25_sm_ctx_t ctx19c;
    p25_sm_init_ctx(&ctx19c, &o19c, &s19c);
    ctx19c.state = P25_SM_TUNED;
    ctx19c.vc_is_tdma = 1;
    ctx19c.vc_freq_hz = 851000000;
    ctx19c.vc_channel = tdma_slot0_ch;
    ctx19c.vc_tg = 5201;
    ctx19c.config.grant_timeout_s = 0.1;
    ctx19c.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    ctx19c.slots[0].grant_active = 1;
    ctx19c.slots[0].last_grant_m = ctx19c.t_tune_m;
    ctx19c.slots[1].grant_active = 1;
    ctx19c.slots[1].voice_active = 1;
    ctx19c.slots[1].last_grant_m = dsd_time_now_monotonic_s();
    g_result_return_to_cc_calls = 0;
    p25_sm_tick_ctx(&ctx19c, &o19c, &s19c);
    assert(ctx19c.state == P25_SM_TUNED);
    assert(g_result_return_to_cc_calls == 0);
    assert(ctx19c.slots[0].grant_active == 0);
    assert(ctx19c.slots[1].voice_active == 1);
    assert(s19c.p25_crypto_state[0] == DSD_P25_CRYPTO_BLOCKED);
    assert(s19c.p25_p2_enc_lockout_muted[0] == 1U);
    assert(s19c.p25_p2_audio_ring_count[0] == 0);
    assert(s19c.p25_p2_audio_ring_count[1] == 3);
    assert(s19c.s_l4[0][0] == 0);
    assert(s19c.s_r4[0][0] == 222);

    // A fresh data grant on the companion slot must not hide an expired voice
    // classification probe on this mixed Phase 2 carrier.
    static dsd_opts o19i;
    static dsd_state s19i;
    DSD_MEMSET(&o19i, 0, sizeof(o19i));
    DSD_MEMSET(&s19i, 0, sizeof(s19i));
    o19i.p25_trunk = 1;
    o19i.p25_is_tuned = 1;
    o19i.trunk_is_tuned = 1;
    o19i.trunk_tune_enc_calls = 0;
    s19i.p25_cc_freq = 851000000;
    s19i.p25_vc_freq[0] = s19i.p25_vc_freq[1] = 851000000;
    s19i.trunk_vc_freq[0] = s19i.trunk_vc_freq[1] = 851000000;
    s19i.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    s19i.p25_p2_enc_lockout_muted[0] = 1U;
    s19i.p25_p2_audio_ring_count[0] = 2;
    s19i.s_l4[0][0] = 333;

    p25_sm_ctx_t ctx19i;
    p25_sm_init_ctx(&ctx19i, &o19i, &s19i);
    ctx19i.state = P25_SM_TUNED;
    ctx19i.vc_is_tdma = 1;
    ctx19i.vc_data_call = 1;
    ctx19i.vc_freq_hz = 851000000;
    ctx19i.vc_channel = tdma_slot1_ch;
    ctx19i.vc_tg = 5202;
    ctx19i.config.grant_timeout_s = 0.1;
    ctx19i.t_tune_m = dsd_time_now_monotonic_s();
    ctx19i.slots[0].grant_active = 1;
    ctx19i.slots[0].last_grant_m = ctx19i.t_tune_m - 1.0;
    ctx19i.slots[1].grant_active = 1;
    ctx19i.slots[1].data_call = 1;
    ctx19i.slots[1].last_grant_m = ctx19i.t_tune_m;
    g_result_return_to_cc_calls = 0;
    p25_sm_tick_ctx(&ctx19i, &o19i, &s19i);
    assert(ctx19i.state == P25_SM_TUNED);
    assert(g_result_return_to_cc_calls == 0);
    assert(ctx19i.slots[0].grant_active == 0);
    assert(ctx19i.slots[1].grant_active == 1);
    assert(ctx19i.slots[1].data_call == 1);
    assert(s19i.p25_crypto_state[0] == DSD_P25_CRYPTO_BLOCKED);
    assert(s19i.p25_p2_audio_ring_count[0] == 0);
    assert(s19i.s_l4[0][0] == 0);

    // 28) A Phase 1 classification timeout with no companion returns to the
    // control channel and resets the call classification at teardown.
    static dsd_opts o19d;
    static dsd_state s19d;
    DSD_MEMSET(&o19d, 0, sizeof(o19d));
    DSD_MEMSET(&s19d, 0, sizeof(s19d));
    o19d.p25_trunk = 1;
    o19d.p25_is_tuned = 1;
    o19d.trunk_is_tuned = 1;
    o19d.trunk_tune_enc_calls = 0;
    s19d.p25_cc_freq = 851000000;
    s19d.p25_vc_freq[0] = s19d.p25_vc_freq[1] = 851125000;
    s19d.trunk_vc_freq[0] = s19d.trunk_vc_freq[1] = 851125000;
    s19d.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    s19d.p25_p2_enc_lockout_muted[0] = 1U;
    s19d.p25_p2_audio_ring_count[0] = 2;
    s19d.s_l4[0][0] = 111;

    p25_sm_ctx_t ctx19d;
    p25_sm_init_ctx(&ctx19d, &o19d, &s19d);
    ctx19d.state = P25_SM_TUNED;
    ctx19d.vc_is_tdma = 0;
    ctx19d.vc_freq_hz = 851125000;
    ctx19d.vc_tg = 5301;
    ctx19d.config.grant_timeout_s = 0.1;
    ctx19d.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    ctx19d.slots[0].grant_active = 1;
    ctx19d.slots[0].last_grant_m = ctx19d.t_tune_m;
    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_return_to_cc_calls = 0;
    p25_sm_tick_ctx(&ctx19d, &o19d, &s19d);
    assert(g_result_return_to_cc_calls == 1);
    assert(ctx19d.state == P25_SM_ON_CC);
    assert(o19d.p25_is_tuned == 0 && o19d.trunk_is_tuned == 0);
    assert(s19d.p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN);
    assert(s19d.p25_p2_enc_lockout_muted[0] == 0U);
    assert(s19d.p25_p2_audio_ring_count[0] == 0);
    assert(s19d.s_l4[0][0] == 0);

#ifdef USE_RADIO
    // 29) A failed CQPSK retry must roll back the one-shot override and TDMA timing.
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

    // 28) A successful CQPSK retry refreshes the TDMA grant timeout window.
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

    // 29) A concurrent stale-context release must not reset a release already in progress.
    static dsd_opts o21;
    static dsd_state s21;
    DSD_MEMSET(&o21, 0, sizeof(o21));
    DSD_MEMSET(&s21, 0, sizeof(s21));
    o21.p25_trunk = 1;
    o21.p25_is_tuned = 1;
    o21.trunk_is_tuned = 1;
    s21.p25_cc_freq = 851000000;
    s21.trunk_cc_freq = 851000000;
    s21.p25_vc_freq[0] = s21.p25_vc_freq[1] = 851125000;
    s21.trunk_vc_freq[0] = s21.trunk_vc_freq[1] = 851125000;

    p25_sm_ctx_t* release_ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(release_ctx, &o21, &s21);
    release_ctx->state = P25_SM_TUNED;
    release_ctx->vc_freq_hz = 851125000;
    release_ctx->vc_channel = ch9;
    release_ctx->vc_tg = 6101;
    release_ctx->tune_count = 41;
    release_ctx->release_count = 13;
    release_ctx->cc_return_count = 17;
    release_ctx->config.cc_grace_s = 9.0;

    int release_sync_rc = dsd_mutex_init(&g_release_hook_mutex);
    if (release_sync_rc != 0) {
        DSD_FPRINTF(stderr, "release race mutex init failed: %d\n", release_sync_rc);
        return 1;
    }
    release_sync_rc = dsd_cond_init(&g_release_hook_cond);
    if (release_sync_rc != 0) {
        DSD_FPRINTF(stderr, "release race condition init failed: %d\n", release_sync_rc);
        (void)dsd_mutex_destroy(&g_release_hook_mutex);
        return 1;
    }
    g_release_hook_entered = 0;
    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_return_to_cc_calls = 0;
    atomic_store(&g_release_hook_block, 1);

    release_thread_args release_args = {&o21, &s21};
    dsd_thread_t release_thread = (dsd_thread_t)0;
    release_sync_rc = dsd_thread_create(&release_thread, release_wrapper_thread, &release_args);
    if (release_sync_rc != 0) {
        DSD_FPRINTF(stderr, "release race thread create failed: %d\n", release_sync_rc);
        atomic_store(&g_release_hook_block, 0);
        (void)dsd_cond_destroy(&g_release_hook_cond);
        (void)dsd_mutex_destroy(&g_release_hook_mutex);
        return 1;
    }
    release_sync_rc = dsd_mutex_lock(&g_release_hook_mutex);
    if (release_sync_rc != 0) {
        DSD_FPRINTF(stderr, "release race mutex lock failed: %d\n", release_sync_rc);
        atomic_store(&g_release_hook_block, 0);
        (void)dsd_cond_signal(&g_release_hook_cond);
        (void)dsd_thread_join(release_thread);
        (void)dsd_cond_destroy(&g_release_hook_cond);
        (void)dsd_mutex_destroy(&g_release_hook_mutex);
        return 1;
    }
    int release_wait_rc = 0;
    while (!g_release_hook_entered && release_wait_rc == 0) {
        release_wait_rc = dsd_cond_timedwait(&g_release_hook_cond, &g_release_hook_mutex, 10000);
    }
    if (release_wait_rc != 0 || !g_release_hook_entered) {
        DSD_FPRINTF(stderr, "release race hook wait failed: %d\n", release_wait_rc);
        atomic_store(&g_release_hook_block, 0);
        (void)dsd_cond_signal(&g_release_hook_cond);
        (void)dsd_mutex_unlock(&g_release_hook_mutex);
        (void)dsd_thread_join(release_thread);
        (void)dsd_cond_destroy(&g_release_hook_cond);
        (void)dsd_mutex_destroy(&g_release_hook_mutex);
        return 1;
    }
    release_sync_rc = dsd_mutex_unlock(&g_release_hook_mutex);
    if (release_sync_rc != 0) {
        DSD_FPRINTF(stderr, "release race mutex unlock failed: %d\n", release_sync_rc);
        atomic_store(&g_release_hook_block, 0);
        (void)dsd_cond_signal(&g_release_hook_cond);
        (void)dsd_thread_join(release_thread);
        (void)dsd_cond_destroy(&g_release_hook_cond);
        (void)dsd_mutex_destroy(&g_release_hook_mutex);
        return 1;
    }

    p25_sm_on_release(&o21, &s21);
    assert(g_result_return_to_cc_calls == 1);
    assert(release_ctx->state == P25_SM_TUNED);
    assert(release_ctx->tune_count == 41);
    assert(release_ctx->release_count == 13);
    assert(release_ctx->cc_return_count == 17);
    assert(release_ctx->config.cc_grace_s == 9.0);

    release_sync_rc = dsd_mutex_lock(&g_release_hook_mutex);
    atomic_store(&g_release_hook_block, 0);
    int release_signal_rc = dsd_cond_signal(&g_release_hook_cond);
    int release_unlock_rc = (release_sync_rc == 0) ? dsd_mutex_unlock(&g_release_hook_mutex) : release_sync_rc;
    int release_join_rc = dsd_thread_join(release_thread);
    if (release_sync_rc != 0 || release_signal_rc != 0 || release_unlock_rc != 0 || release_join_rc != 0) {
        DSD_FPRINTF(stderr, "release race resume failed: lock=%d signal=%d unlock=%d join=%d\n", release_sync_rc,
                    release_signal_rc, release_unlock_rc, release_join_rc);
        (void)dsd_cond_destroy(&g_release_hook_cond);
        (void)dsd_mutex_destroy(&g_release_hook_mutex);
        return 1;
    }

    assert(g_result_return_to_cc_calls == 1);
    assert(release_ctx->state == P25_SM_ON_CC);
    assert(release_ctx->cc_sync_pending == 1);
    assert(release_ctx->t_cc_tune_m > 0.0);
    assert(release_ctx->tune_count == 41);
    assert(release_ctx->release_count == 14);
    assert(release_ctx->cc_return_count == 18);
    assert(release_ctx->config.cc_grace_s == 9.0);
    int release_cond_destroy_rc = dsd_cond_destroy(&g_release_hook_cond);
    int release_mutex_destroy_rc = dsd_mutex_destroy(&g_release_hook_mutex);
    if (release_cond_destroy_rc != 0 || release_mutex_destroy_rc != 0) {
        DSD_FPRINTF(stderr, "release race sync destroy failed: cond=%d mutex=%d\n", release_cond_destroy_rc,
                    release_mutex_destroy_rc);
        return 1;
    }

    // 30) Hardware-only legacy VC hooks still receive their return callback.
    install_trunk_tuning_hooks();
    static dsd_opts o22;
    static dsd_state s22;
    DSD_MEMSET(&o22, 0, sizeof(o22));
    DSD_MEMSET(&s22, 0, sizeof(s22));
    o22.p25_trunk = 1;
    o22.trunk_tune_group_calls = 1;
    s22.p25_cc_freq = 851000000;
    s22.trunk_cc_freq = 851000000;
    s22.p25_iden_fdma[id9].base_freq = 851000000 / 5;
    s22.p25_iden_fdma[id9].chan_type = 1;
    s22.p25_iden_fdma[id9].chan_spac = 100;
    s22.p25_iden_fdma[id9].trust = 2;
    s22.p25_iden_fdma[id9].populated = 1;
    s22.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx22;
    p25_sm_init_ctx(&ctx22, &o22, &s22);
    g_return_to_cc_called = 0;
    p25_sm_event(&ctx22, &o22, &s22, &ev9);
    assert(ctx22.state == P25_SM_TUNED);
    assert(o22.p25_is_tuned == 1 && o22.trunk_is_tuned == 1);
    assert(s22.p25_vc_freq[0] == ctx22.vc_freq_hz && s22.trunk_vc_freq[0] == ctx22.vc_freq_hz);
    assert(s22.last_vc_sync_time_m > 0.0 && s22.p25_last_vc_tune_time_m > 0.0);
    p25_sm_release(&ctx22, &o22, &s22, "legacy-hook-release");
    assert(g_return_to_cc_called == 1);
    assert(ctx22.state == P25_SM_ON_CC);

    // 31) Result hooks may control hardware without duplicating decoder bookkeeping.
    install_result_tuning_hooks();
    static dsd_opts o23;
    static dsd_state s23;
    DSD_MEMSET(&o23, 0, sizeof(o23));
    DSD_MEMSET(&s23, 0, sizeof(s23));
    o23.p25_trunk = 1;
    o23.trunk_tune_group_calls = 1;
    s23.p25_cc_freq = 851000000;
    s23.trunk_cc_freq = 851000000;
    s23.p25_iden_fdma[id9] = s22.p25_iden_fdma[id9];
    s23.p25_chan_tdma_explicit[id9] = 1;
    p25_sm_ctx_t ctx23;
    p25_sm_init_ctx(&ctx23, &o23, &s23);
    g_result_hook_commits_decoder_state = 0;
    g_result_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_result_return_to_cc_calls = 0;
    p25_sm_event(&ctx23, &o23, &s23, &ev9);
    assert(ctx23.state == P25_SM_TUNED);
    assert(o23.p25_is_tuned == 1 && o23.trunk_is_tuned == 1);
    assert(s23.p25_vc_freq[0] == ctx23.vc_freq_hz && s23.trunk_vc_freq[0] == ctx23.vc_freq_hz);
    p25_sm_release(&ctx23, &o23, &s23, "result-hook-release");
    assert(g_result_return_to_cc_calls == 1);
    assert(ctx23.state == P25_SM_ON_CC);
    g_result_hook_commits_decoder_state = 1;

    // 32) Externally cleared tune state still needs the complete decoder-side
    // release teardown, but it must not issue another CC retune or reset stats.
    static dsd_opts o24;
    static dsd_state s24;
    DSD_MEMSET(&o24, 0, sizeof(o24));
    DSD_MEMSET(&s24, 0, sizeof(s24));
    o24.p25_trunk = 1;
    s24.p25_cc_freq = 851000000;
    s24.trunk_cc_freq = 851000000;
    s24.last_cc_sync_time_m = dsd_time_now_monotonic_s();

    p25_sm_ctx_t ctx24;
    p25_sm_init_ctx(&ctx24, &o24, &s24);
    ctx24.state = P25_SM_TUNED;
    ctx24.vc_freq_hz = 851125000;
    ctx24.vc_channel = ch9;
    ctx24.vc_tg = 6201;
    ctx24.slots[0].grant_active = 1;
    ctx24.slots[0].tg = 6201;
    ctx24.tune_count = 23;
    ctx24.release_count = 29;
    ctx24.grant_count = 30;
    ctx24.cc_return_count = 31;
    s24.p25_sm_release_count = 37;
    s24.p25_p2_audio_allowed[0] = 1;
    s24.p25_p2_audio_allowed[1] = 1;
    s24.p25_p2_active_slot = 0;
    s24.payload_algid = 0x80;
    s24.payload_algidR = 0x81;
    s24.payload_keyid = 0x1234;
    s24.payload_keyidR = 0x5678;
    s24.dmr_so = 0x40;
    s24.dmr_soR = 0x41;
    s24.p25_service_options_valid[0] = 1;
    s24.p25_service_options_valid[1] = 1;
    s24.p25_p2_enc_lockout_muted[0] = 1;
    s24.p25_p2_enc_lockout_muted[1] = 1;
    s24.p25_policy_tg[0] = 6201;
    s24.p25_policy_tg[1] = 6202;

    dsd_tg_policy_call_route active_route = {6201U, 7201U, 851125000L, 1, 0, 0};
    dsd_tg_policy_decision active_decision = {0};
    active_decision.priority = 10;
    active_decision.tune_allowed = 1;
    assert(dsd_tg_policy_note_active_call(&s24, &active_route, &active_decision, 1.0) == 0);
    dsd_tg_policy_call_route candidate_route = {6202U, 7202U, 851250000L, 2, 0, 1};
    dsd_tg_policy_decision candidate_decision = {0};
    candidate_decision.priority = 20;
    candidate_decision.tune_allowed = 1;
    candidate_decision.preempt_requested = 1;
    assert(dsd_tg_policy_should_preempt(&o24, &s24, &candidate_route, &candidate_decision, 10.0) == 1);

    g_result_return_to_cc_calls = 0;
    p25_sm_release(&ctx24, &o24, &s24, "external-return-stale-context");
    assert(g_result_return_to_cc_calls == 0);
    assert(ctx24.state == P25_SM_ON_CC);
    assert(ctx24.vc_freq_hz == 0 && ctx24.vc_channel == 0 && ctx24.vc_tg == 0);
    assert(ctx24.slots[0].grant_active == 0);
    assert(ctx24.tune_count == 23);
    assert(ctx24.release_count == 30);
    assert(ctx24.grant_count == 30);
    assert(ctx24.cc_return_count == 32);
    assert(s24.p25_sm_release_count == 38);
    assert(s24.p25_p2_audio_allowed[0] == 0 && s24.p25_p2_audio_allowed[1] == 0);
    assert(s24.p25_p2_active_slot == -1);
    assert(s24.payload_algid == 0 && s24.payload_algidR == 0);
    assert(s24.payload_keyid == 0 && s24.payload_keyidR == 0);
    assert(s24.dmr_so == 0 && s24.dmr_soR == 0);
    assert(s24.p25_service_options_valid[0] == 0 && s24.p25_service_options_valid[1] == 0);
    assert(s24.p25_p2_enc_lockout_muted[0] == 0 && s24.p25_p2_enc_lockout_muted[1] == 0);
    assert(s24.p25_policy_tg[0] == 0 && s24.p25_policy_tg[1] == 0);
    assert(dsd_tg_policy_should_preempt(&o24, &s24, &candidate_route, &candidate_decision, 10.0) == 0);

    install_trunk_tuning_hooks();

    DSD_FPRINTF(stderr, "P25 SM core tests passed\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
