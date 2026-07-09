// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify CC candidate cooldown: after tuning a failing candidate, it is
 * cooled down and skipped on the next hunt in favor of another candidate. */

#include <assert.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static long g_last_tuned_cc = 0;
static int g_tune_to_cc_calls = 0;
static dsd_trunk_tune_result g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

void
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)ted_sps;
    g_last_tuned_cc = freq;
}

static dsd_trunk_tune_result
trunk_tune_to_cc_result(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    trunk_tune_to_cc(opts, state, freq, ted_sps);
    g_tune_to_cc_calls++;
    return g_tune_to_cc_result;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc = trunk_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
init_basic(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(o, 0, sizeof(*o));
    DSD_MEMSET(s, 0, sizeof(*s));
    o->p25_trunk = 1;
    o->trunk_hangtime = 0.2f; // short for test
    o->p25_prefer_candidates = 1;
    s->p25_cc_freq = 851000000;
    p25_sm_init(o, s);
}

int
main(void) {
    static dsd_opts o;
    static dsd_state st;
    install_trunk_tuning_hooks();
    init_basic(&o, &st);
    // Two candidates A, B
    long A = 852000000;
    long B = 853000000;
    (void)dsd_trunk_cc_candidates_add(&st, A, 0);
    (void)dsd_trunk_cc_candidates_add(&st, B, 0);
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

    static dsd_opts o2;
    static dsd_state st2;
    init_basic(&o2, &st2);
    (void)dsd_trunk_cc_candidates_add(&st2, A, 0);
    (void)dsd_trunk_cc_candidates_add(&st2, B, 0);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    double pending_m = dsd_time_now_monotonic_s() - 2.5;
    ctx->state = P25_SM_ON_CC;
    ctx->config.cc_grace_s = 5.0;
    ctx->t_cc_sync_m = pending_m;
    ctx->t_cc_tune_m = pending_m;
    ctx->cc_sync_pending = 1;
    st2.last_cc_sync_time_m = pending_m;
    st2.p25_cc_eval_freq = A;
    st2.p25_cc_eval_start_m = pending_m;

    g_last_tuned_cc = 0;
    p25_sm_tick(&o2, &st2);
    assert(g_last_tuned_cc == B);

    // A controller timeout is an in-flight tune, not the start of CC acquisition.
    static dsd_opts o3;
    static dsd_state st3;
    init_basic(&o3, &st3);
    (void)dsd_trunk_cc_candidates_add(&st3, A, 0);
    (void)dsd_trunk_cc_candidates_add(&st3, B, 0);
    st3.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;
    dsd_trunk_tuning_hooks pending_hooks = {0};
    pending_hooks.tune_to_cc_result = trunk_tune_to_cc_result;
    dsd_trunk_tuning_hooks_set(pending_hooks);
    g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    g_tune_to_cc_calls = 0;
    g_last_tuned_cc = 0;

    p25_sm_tick(&o3, &st3);
    ctx = p25_sm_get_ctx();
    assert(g_tune_to_cc_calls == 1);
    assert(g_last_tuned_cc == A);
    assert(ctx->cc_tune_pending == 1);
    assert(ctx->cc_tune_request_id != 0U);
    assert(ctx->t_cc_tune_m == 0.0);
    assert(st3.p25_cc_eval_freq == A);
    assert(st3.p25_cc_eval_start_m == 0.0);

    // Repeated watchdog ticks cannot cool A or queue B before completion.
    p25_sm_tick(&o3, &st3);
    p25_sm_tick(&o3, &st3);
    assert(g_tune_to_cc_calls == 1);
    assert(ctx->cc_tune_pending == 1);
    assert(ctx->t_cc_tune_m == 0.0);

    uint64_t pending_request = ctx->cc_tune_request_id;
    dsd_trunk_tuning_request_complete(pending_request, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick(&o3, &st3);
    assert(g_tune_to_cc_calls == 1);
    assert(ctx->cc_tune_pending == 0);
    assert(ctx->cc_sync_pending == 1);
    assert(ctx->t_cc_tune_m > 0.0);
    assert(st3.p25_cc_eval_start_m == ctx->t_cc_tune_m);

    // Only the post-completion acquisition window can fail A and advance to B.
    pending_m = dsd_time_now_monotonic_s() - 2.5;
    ctx->t_cc_sync_m = pending_m;
    ctx->t_cc_tune_m = pending_m;
    st3.last_cc_sync_time_m = pending_m;
    st3.p25_cc_eval_start_m = pending_m;
    p25_sm_tick(&o3, &st3);
    assert(g_tune_to_cc_calls == 2);
    assert(g_last_tuned_cc == B);

    // A terminal asynchronous failure stays gated while the SM selects a
    // replacement target.
    uint64_t failed_request = ctx->cc_tune_request_id;
    assert(failed_request != 0U);
    const double stale_decoded_m = dsd_time_now_monotonic_s() - 10.0;
    st3.p25_last_cc_msg_time_m = stale_decoded_m;
    g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    dsd_trunk_tuning_request_publish(failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    p25_sm_tick(&o3, &st3);
    assert(g_tune_to_cc_calls == 3);
    assert(ctx->state == P25_SM_HUNTING);
    assert(ctx->cc_sync_pending == 0);
    assert(ctx->t_cc_sync_m > stale_decoded_m);
    assert(dsd_trunk_tuning_pending_request() == failed_request);
    assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));

    // Stale decoded activity cannot masquerade as acquisition after failure.
    p25_sm_tick(&o3, &st3);
    assert(ctx->state == P25_SM_HUNTING);

    // A later successful replacement commits the new target and reopens dispatch.
    uint64_t recovery_request = dsd_trunk_tuning_request_begin();
    assert(recovery_request > failed_request);
    dsd_trunk_tuning_request_complete(recovery_request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
