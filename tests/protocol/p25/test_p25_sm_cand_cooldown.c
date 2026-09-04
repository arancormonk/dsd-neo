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
#include <math.h>
#include <stddef.h>
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

static dsd_trunk_tune_result
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    (void)ted_sps;
    g_last_tuned_cc = freq;
    g_tune_to_cc_calls++;
    return g_tune_to_cc_result;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = trunk_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
init_basic(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(o, 0, sizeof(*o));
    DSD_MEMSET(s, 0, sizeof(*s));
    o->trunk_enable = 1;
    o->trunk_hangtime = 0.2f; // short for test
    o->p25_prefer_candidates = 1;
    s->p25_cc_freq = 851000000;
    p25_sm_init_ctx(p25_sm_get_ctx(), o, s);
}

int
main(void) {
    const double timestamp_epsilon_s = 1.0e-9;
    static dsd_opts o;
    static dsd_state st;
    install_trunk_tuning_hooks();
    init_basic(&o, &st);
    // Two candidates A, B
    long A = 852000000;
    long B = 853000000;
    (void)dsd_trunk_cc_candidates_add(&st, A, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    (void)dsd_trunk_cc_candidates_add(&st, B, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    // Force CC hunt
    st.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;

    // First tick: should tune to A
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o, &st);
    assert(g_last_tuned_cc == A);

    // Simulate evaluation window expiry with no CC activity to trigger cooldown for A
    st.p25_cc_eval_freq = A;
    st.p25_cc_eval_start_m = dsd_time_now_monotonic_s() - 5.0;
    st.last_cc_sync_time_m = 0.0; // no CC activity

    // Next tick: cooldown applied; next hunt should pick B
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o, &st);
    assert(g_last_tuned_cc == B);

    static dsd_opts o2;
    static dsd_state st2;
    init_basic(&o2, &st2);
    (void)dsd_trunk_cc_candidates_add(&st2, A, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    (void)dsd_trunk_cc_candidates_add(&st2, B, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    double pending_m = dsd_time_now_monotonic_s() - 2.5;
    ctx->state = P25_SM_ON_CC;
    ctx->config.cc_grace_s = 5.0;
    ctx->t_cc_sync_m = pending_m;
    ctx->t_cc_tune_m = pending_m;
    ctx->cc_sync_pending = 1;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_HUNT_PROBE;
    st2.last_cc_sync_time_m = pending_m;
    st2.p25_cc_eval_freq = A;
    st2.p25_cc_eval_start_m = pending_m;

    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o2, &st2);
    assert(g_last_tuned_cc == B);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_HUNT_PROBE);

    // A candidate carried into a normal return must remain eligible for the
    // full return grace, then clear without cooldown after decoded CC activity.
    static dsd_opts o_return;
    static dsd_state st_return;
    init_basic(&o_return, &st_return);
    (void)dsd_trunk_cc_candidates_add(&st_return, A, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    ctx = p25_sm_get_ctx();
    ctx->config.cc_grace_s = 5.0;
    st_return.p25_cc_eval_freq = A;
    const double return_tune_m = dsd_time_now_monotonic_s() - 4.0;
    st_return.p25_last_cc_msg_time_m = return_tune_m - 0.25;
    assert(p25_sm_restart_pending_cc_acquisition(ctx, &o_return, &st_return, return_tune_m, "test-return") == 1);
    assert(fabs(st_return.p25_cc_eval_start_m - ctx->t_cc_tune_m) <= timestamp_epsilon_s);

    const dsd_trunk_cc_candidates* return_candidates = dsd_trunk_cc_candidates_peek(&st_return);
    assert(return_candidates != NULL);
    assert(return_candidates->count == 1);
    assert(return_candidates->cool_until[0] == 0.0);
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(ctx, &o_return, &st_return);
    assert(g_last_tuned_cc == 0);
    assert(ctx->state == P25_SM_ON_CC);
    assert(ctx->cc_sync_pending == 1);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN);
    assert(st_return.p25_cc_eval_freq == A);
    assert(return_candidates->cool_until[0] == 0.0);

    const double decoded_return_m = dsd_time_now_monotonic_s();
    st_return.last_cc_sync_time_m = decoded_return_m;
    st_return.p25_last_cc_msg_time_m = decoded_return_m;
    p25_sm_tick_ctx(ctx, &o_return, &st_return);
    assert(ctx->cc_sync_pending == 0);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_NONE);
    assert(st_return.p25_cc_eval_freq == 0);
    assert(return_candidates->cool_until[0] == 0.0);

    // A return that exhausts its full grace still cools the failed candidate
    // and advances to the next hunt probe.
    static dsd_opts o_return_timeout;
    static dsd_state st_return_timeout;
    init_basic(&o_return_timeout, &st_return_timeout);
    (void)dsd_trunk_cc_candidates_add(&st_return_timeout, A, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    (void)dsd_trunk_cc_candidates_add(&st_return_timeout, B, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    ctx = p25_sm_get_ctx();
    ctx->config.cc_grace_s = 5.0;
    st_return_timeout.p25_cc_eval_freq = A;
    const double expired_return_m = dsd_time_now_monotonic_s() - 5.5;
    st_return_timeout.p25_last_cc_msg_time_m = expired_return_m - 0.25;
    assert(p25_sm_restart_pending_cc_acquisition(ctx, &o_return_timeout, &st_return_timeout, expired_return_m,
                                                 "test-return-timeout")
           == 1);

    const dsd_trunk_cc_candidates* expired_candidates = dsd_trunk_cc_candidates_peek(&st_return_timeout);
    assert(expired_candidates != NULL);
    assert(expired_candidates->count == 2);
    g_last_tuned_cc = 0;
    p25_sm_tick_ctx(ctx, &o_return_timeout, &st_return_timeout);
    assert(g_last_tuned_cc == B);
    assert(expired_candidates->cool_until[0] > dsd_time_now_monotonic_s());
    assert(st_return_timeout.p25_cc_eval_freq == B);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_HUNT_PROBE);

    // A controller timeout is an in-flight tune, not the start of CC acquisition.
    static dsd_opts o3;
    static dsd_state st3;
    init_basic(&o3, &st3);
    (void)dsd_trunk_cc_candidates_add(&st3, A, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    (void)dsd_trunk_cc_candidates_add(&st3, B, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    st3.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;
    dsd_trunk_tuning_hooks pending_hooks = {0};
    pending_hooks.tune_to_cc_request = trunk_tune_to_cc;
    dsd_trunk_tuning_hooks_set(pending_hooks);
    g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    g_tune_to_cc_calls = 0;
    g_last_tuned_cc = 0;

    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
    ctx = p25_sm_get_ctx();
    assert(g_tune_to_cc_calls == 1);
    assert(g_last_tuned_cc == A);
    assert(ctx->cc_tune_pending == 1);
    assert(ctx->cc_tune_request_id != 0U);
    assert(ctx->t_cc_tune_m == 0.0);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_HUNT_PROBE);
    assert(st3.p25_cc_eval_freq == A);
    assert(st3.p25_cc_eval_start_m == 0.0);

    // Repeated watchdog ticks cannot cool A or queue B before completion.
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
    assert(g_tune_to_cc_calls == 1);
    assert(ctx->cc_tune_pending == 1);
    assert(ctx->t_cc_tune_m == 0.0);

    uint64_t pending_request = ctx->cc_tune_request_id;
    dsd_trunk_tuning_request_complete(pending_request, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
    assert(g_tune_to_cc_calls == 1);
    assert(ctx->cc_tune_pending == 0);
    assert(ctx->cc_sync_pending == 1);
    assert(ctx->t_cc_tune_m > 0.0);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_HUNT_PROBE);
    assert(fabs(st3.p25_cc_eval_start_m - ctx->t_cc_tune_m) <= timestamp_epsilon_s);

    // Only the post-completion acquisition window can fail A and advance to B.
    pending_m = dsd_time_now_monotonic_s() - 2.5;
    ctx->t_cc_sync_m = pending_m;
    ctx->t_cc_tune_m = pending_m;
    st3.last_cc_sync_time_m = pending_m;
    st3.p25_cc_eval_start_m = pending_m;
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
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
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
    assert(g_tune_to_cc_calls == 3);
    assert(ctx->state == P25_SM_HUNTING);
    assert(ctx->cc_sync_pending == 0);
    assert(ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_NONE);
    assert(ctx->t_cc_sync_m > stale_decoded_m);
    assert(dsd_trunk_tuning_pending_request() == failed_request);
    assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));

    // Stale decoded activity cannot masquerade as acquisition after failure.
    p25_sm_tick_ctx(p25_sm_get_ctx(), &o3, &st3);
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
