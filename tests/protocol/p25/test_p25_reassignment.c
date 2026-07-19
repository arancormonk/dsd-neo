// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* P25 traffic-carrier reassignment and stale-update validation regressions. */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
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

static int g_tune_calls;
static int g_return_calls;
static dsd_trunk_tune_result g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    g_tune_calls++;
    if (!dsd_trunk_tune_result_is_ok(g_tune_result)) {
        return g_tune_result;
    }
    opts->trunk_is_tuned = 1;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return g_tune_result;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_return_calls++;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });
}

static int
expect_true(const char* label, int condition) {
    if (!condition) {
        DSD_FPRINTF(stderr, "%s: expected true\n", label);
        return 1;
    }
    return 0;
}

static void
init_case(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t* ctx) {
    const int id = 2;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_hangtime = 2.0f;
    opts->p25_grant_voice_to_s = 0.2;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_iden_tdma[id].base_freq = 851000000 / 5;
    state->p25_iden_tdma[id].chan_type = 3;
    state->p25_iden_tdma[id].chan_spac = 100;
    state->p25_iden_tdma[id].trust = 2;
    state->p25_iden_tdma[id].populated = 1;
    state->p25_chan_tdma_explicit[id] = 2;
    p25_sm_init_ctx(ctx, opts, state);
    g_tune_calls = 0;
    g_return_calls = 0;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
}

static void
init_p1_case(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t* ctx) {
    const int id = 1;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_hangtime = 2.0f;
    opts->p25_grant_voice_to_s = 0.2;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->p25_iden_fdma[id].base_freq = 851000000 / 5;
    state->p25_iden_fdma[id].chan_type = 1;
    state->p25_iden_fdma[id].chan_spac = 100;
    state->p25_iden_fdma[id].trust = 2;
    state->p25_iden_fdma[id].populated = 1;
    state->p25_chan_tdma_explicit[id] = 1;
    p25_sm_init_ctx(ctx, opts, state);
    g_tune_calls = 0;
    g_return_calls = 0;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
}

static void
note_cc_reacquired(const p25_sm_ctx_t* ctx, dsd_state* state) {
    double decoded_m = ctx->t_cc_tune_m + 0.010;
    if (decoded_m <= 0.0) {
        decoded_m = dsd_time_now_monotonic_s();
    }
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = decoded_m;
    state->p25_last_cc_msg_time = state->last_cc_sync_time;
    state->p25_last_cc_msg_time_m = decoded_m;
}

static void
age_initial_acquisition(p25_sm_ctx_t* ctx) {
    const double stale_m = dsd_time_now_monotonic_s() - 1.0;
    ctx->t_tune_m = stale_m;
    for (int slot = 0; slot < 2; slot++) {
        if (ctx->slots[slot].grant_active) {
            ctx->slots[slot].last_grant_m = stale_m;
            ctx->slots[slot].crypto_attempt_m = stale_m;
        }
    }
}

static int
test_assignment_after_failed_tune(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = (2 << 12) | 2;
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, 4101, 5101, 0);
    int rc = 0;

    init_case(&opts, &state, &ctx);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    p25_sm_event(&ctx, &opts, &state, &grant);
    rc |= expect_true("failed tune attempted", g_tune_calls == 1 && ctx.state == P25_SM_ON_CC);

    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_event(&ctx, &opts, &state, &grant);
    rc |= expect_true("assignment accepted after failed tune",
                      g_tune_calls == 2 && ctx.state == P25_SM_TUNED && opts.trunk_is_tuned == 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_assignment_after_timeout(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = (2 << 12) | 2;
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, 4201, 5201, 0);
    int rc = 0;

    init_case(&opts, &state, &ctx);
    p25_sm_event(&ctx, &opts, &state, &grant);
    age_initial_acquisition(&ctx);
    p25_sm_tick_ctx(&ctx, &opts, &state);
    rc |= expect_true("initial acquisition timed out",
                      g_return_calls == 1 && ctx.state == P25_SM_ON_CC && opts.trunk_is_tuned == 0);

    note_cc_reacquired(&ctx, &state);
    p25_sm_event(&ctx, &opts, &state, &grant);
    rc |= expect_true("assignment accepted after timeout",
                      g_tune_calls == 2 && ctx.state == P25_SM_TUNED && opts.trunk_is_tuned == 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_assignment_after_explicit_release(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = (2 << 12) | 2;
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, 4301, 5301, 0);
    int rc = 0;

    init_case(&opts, &state, &ctx);
    p25_sm_event(&ctx, &opts, &state, &grant);
    p25_sm_release(&ctx, &opts, &state, "test-explicit-release");
    note_cc_reacquired(&ctx, &state);
    p25_sm_event(&ctx, &opts, &state, &grant);
    rc |= expect_true("assignment accepted after explicit release",
                      g_return_calls == 1 && g_tune_calls == 2 && ctx.state == P25_SM_TUNED);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_followup_reuses_retained_carrier(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = (2 << 12) | 2;
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, 4401, 5401, 0);
    p25_sm_event_t first = p25_sm_ev_ptt_call(0, 4401, 0, 5401, 1, 0);
    p25_sm_event_t followup = p25_sm_ev_ptt_call(0, 4401, 0, 5402, 1, 0);
    int rc = 0;

    init_case(&opts, &state, &ctx);
    p25_sm_event(&ctx, &opts, &state, &grant);
    p25_sm_event(&ctx, &opts, &state, &first);
    p25_sm_event_t end = p25_sm_ev_end_call_at(0, 4401, 5401, dsd_time_now_monotonic_s());
    p25_sm_event(&ctx, &opts, &state, &end);
    rc |= expect_true("transmission end retained carrier",
                      ctx.state == P25_SM_TUNED && ctx.t_hangtime_m > 0.0 && g_return_calls == 0);
    p25_sm_event(&ctx, &opts, &state, &followup);
    rc |= expect_true("followup reopened without tune", g_tune_calls == 1 && g_return_calls == 0
                                                            && ctx.slots[0].voice_active == 1
                                                            && ctx.slots[0].src == 5402 && ctx.t_hangtime_m == 0.0);
    dsd_state_ext_free_all(&state);
    return rc;
}

// Deterministic event fixture derived from the 20:37:07 Motorola Talk
// Complete and 20:37:08 TDU sequence in p25_mot.log. Both signals retain the
// P1 carrier, and the next channel-user activity begins without another tune.
static int
test_motorola_talk_complete_tdu_fixture(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = 0x100A;
    const long freq = 851125000;
    int rc = 0;

    init_p1_case(&opts, &state, &ctx);
    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, freq, 4601, 5601, 0);
    p25_sm_event(&ctx, &opts, &state, &ev);
    rc |= expect_true("fixture assignment armed", g_tune_calls == 1 && state.p25_sm_mode == DSD_P25_SM_MODE_ARMED);

    ev = p25_sm_ev_active_call(0, 4601, 0, 5601, 1, 0);
    p25_sm_event(&ctx, &opts, &state, &ev);
    rc |= expect_true("fixture first epoch followed", state.p25_sm_mode == DSD_P25_SM_MODE_FOLLOW);

    ev = p25_sm_ev_end_call_at(0, 4601, 5601, dsd_time_now_monotonic_s());
    p25_sm_event(&ctx, &opts, &state, &ev);
    rc |= expect_true("fixture Talk Complete retained carrier",
                      ctx.state == P25_SM_TUNED && ctx.vc_freq_hz == freq && ctx.slots[0].grant_active
                          && state.p25_sm_mode == DSD_P25_SM_MODE_HANG && g_return_calls == 0);

    ev = p25_sm_ev_tdu();
    p25_sm_event(&ctx, &opts, &state, &ev);
    rc |= expect_true("fixture TDU retained carrier",
                      ctx.state == P25_SM_TUNED && ctx.vc_freq_hz == freq && ctx.slots[0].target_id == 4601
                          && state.p25_sm_mode == DSD_P25_SM_MODE_HANG && g_return_calls == 0);

    ev = p25_sm_ev_active_call(0, 4601, 0, 5602, 1, 0);
    p25_sm_event(&ctx, &opts, &state, &ev);
    rc |= expect_true("fixture follow-up reused carrier", g_tune_calls == 1 && g_return_calls == 0
                                                              && ctx.slots[0].voice_active && ctx.slots[0].src == 5602
                                                              && state.p25_sm_mode == DSD_P25_SM_MODE_FOLLOW);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_p1_source_less_update_validation(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = 0x100A;
    const long freq = 851125000;
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, freq, 4701, 5701, 0);
    p25_sm_event_t ptt = p25_sm_ev_ptt_call(0, 4701, 0, 5701, 1, 0);
    p25_sm_event_t update = p25_sm_ev_group_grant_update(channel, freq, 4701, 0, P25_SM_SVC_UNKNOWN);
    int rc = 0;

    init_p1_case(&opts, &state, &ctx);
    p25_sm_event(&ctx, &opts, &state, &grant);
    p25_sm_event(&ctx, &opts, &state, &ptt);
    p25_sm_event_t end = p25_sm_ev_end_call_at(0, 4701, 5701, dsd_time_now_monotonic_s());
    p25_sm_event(&ctx, &opts, &state, &end);
    ctx.t_hangtime_m = dsd_time_now_monotonic_s() - 3.0;
    p25_sm_tick_ctx(&ctx, &opts, &state);
    note_cc_reacquired(&ctx, &state);

    p25_sm_event(&ctx, &opts, &state, &update);
    rc |= expect_true("P1 fresh source-less update quarantined",
                      g_tune_calls == 1 && ctx.state == P25_SM_ON_CC && ctx.recent_call_ends[0].valid);

    ctx.recent_call_ends[0].end_m = dsd_time_now_monotonic_s() - 2.1;
    ctx.recent_call_ends[0].last_match_m = dsd_time_now_monotonic_s();
    p25_sm_event(&ctx, &opts, &state, &update);
    rc |= expect_true("P1 validation probe uses logical slot zero", g_tune_calls == 2 && ctx.state == P25_SM_TUNED
                                                                        && ctx.vc_stale_regrant_probe
                                                                        && ctx.vc_stale_regrant_probe_slot == 0);

    age_initial_acquisition(&ctx);
    p25_sm_tick_ctx(&ctx, &opts, &state);
    note_cc_reacquired(&ctx, &state);
    p25_sm_event(&ctx, &opts, &state, &update);
    rc |= expect_true("P1 failed validation probe did not repeat", g_tune_calls == 2 && ctx.state == P25_SM_ON_CC);

    grant.src = 5702;
    p25_sm_event(&ctx, &opts, &state, &grant);
    rc |= expect_true("P1 changed-source assignment bypassed stale validation",
                      g_tune_calls == 3 && ctx.state == P25_SM_TUNED && ctx.slots[0].src == 5702);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_failed_validation_probe_does_not_loop(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t ctx;
    const int channel = (2 << 12) | 2;
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, 4501, 5501, 0);
    p25_sm_event_t ptt = p25_sm_ev_ptt_call(0, 4501, 0, 5501, 1, 0);
    p25_sm_event_t update = p25_sm_ev_group_grant_update(channel, 0, 4501, 0, P25_SM_SVC_UNKNOWN);
    int rc = 0;

    init_case(&opts, &state, &ctx);
    p25_sm_event(&ctx, &opts, &state, &grant);
    p25_sm_event(&ctx, &opts, &state, &ptt);
    p25_sm_event_t end = p25_sm_ev_end_call_at(0, 4501, 5501, dsd_time_now_monotonic_s());
    p25_sm_event(&ctx, &opts, &state, &end);
    ctx.t_hangtime_m = dsd_time_now_monotonic_s() - 3.0;
    p25_sm_tick_ctx(&ctx, &opts, &state);
    note_cc_reacquired(&ctx, &state);

    p25_sm_event(&ctx, &opts, &state, &update);
    rc |= expect_true("fresh ambiguous update quarantined", g_tune_calls == 1 && ctx.state == P25_SM_ON_CC);

    ctx.recent_call_ends[0].end_m = dsd_time_now_monotonic_s() - 2.1;
    ctx.recent_call_ends[0].last_match_m = dsd_time_now_monotonic_s();
    p25_sm_event(&ctx, &opts, &state, &update);
    rc |= expect_true("single validation probe tuned",
                      g_tune_calls == 2 && ctx.state == P25_SM_TUNED && ctx.vc_stale_regrant_probe == 1);

    age_initial_acquisition(&ctx);
    p25_sm_tick_ctx(&ctx, &opts, &state);
    note_cc_reacquired(&ctx, &state);
    p25_sm_event(&ctx, &opts, &state, &update);
    rc |= expect_true("failed validation probe did not repeat", g_tune_calls == 2 && ctx.state == P25_SM_ON_CC);

    p25_sm_event(&ctx, &opts, &state, &grant);
    rc |= expect_true("authoritative assignment accepted after failed probe",
                      g_tune_calls == 3 && ctx.state == P25_SM_TUNED && opts.trunk_is_tuned == 1);
    dsd_state_ext_free_all(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    install_hooks();
    rc |= test_assignment_after_failed_tune();
    rc |= test_assignment_after_timeout();
    rc |= test_assignment_after_explicit_release();
    rc |= test_followup_reuses_retained_carrier();
    rc |= test_motorola_talk_complete_tdu_fixture();
    rc |= test_p1_source_less_update_validation();
    rc |= test_failed_validation_probe_does_not_loop();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
