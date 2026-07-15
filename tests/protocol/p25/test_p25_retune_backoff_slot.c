// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify retune backoff applies per-slot on TDMA voice channels:
 * - After returning from a VC with no voice observed, a short retune backoff
 *   is applied for the same RF frequency and slot.
 * - A subsequent grant to the opposite slot at the same RF is allowed
 *   immediately (no backoff).
 * - Multiple failed VC/slot pairs remain blocked concurrently instead of the
 *   newest failure replacing the previous one.
 * - Explicit data grants bypass stale failed-voice backoff on the same slot.
 * - Remaining per-slot data grants keep a canceled same-carrier voice grant
 *   from arming failed-voice fallback backoff.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
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

// Canonical trunk-tuning hooks used by the test.
static long g_last_tuned_vc = 0;
static int g_tune_to_freq_calls = 0;
static int g_return_to_cc_calls = 0;
static dsd_trunk_tune_result g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

static dsd_trunk_tune_result
tune_to_freq_result(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    g_tune_to_freq_calls++;
    g_last_tuned_vc = freq;
    if (opts) {
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc_result(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_return_to_cc_calls++;
    if (!dsd_trunk_tune_result_is_ok(g_return_to_cc_result)) {
        return g_return_to_cc_result;
    }
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return g_return_to_cc_result;
}

static void
install_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_to_freq_result;
    hooks.return_to_cc_request = return_to_cc_result;
    dsd_trunk_tuning_hooks_set(hooks);
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
retune_backoff_empty(const dsd_state* state) {
    if (!state || state->p25_retune_block_until != 0 || state->p25_retune_block_freq != 0) {
        return 0;
    }
    for (int i = 0; i < DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH; i++) {
        if (state->p25_retune_block_history_until[i] != 0 || state->p25_retune_block_history_freq[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int
retune_backoff_history_has_slot(const dsd_state* state, long freq, int slot) {
    if (!state || freq <= 0) {
        return 0;
    }
    for (int i = 0; i < DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH; i++) {
        if (state->p25_retune_block_history_freq[i] == freq && state->p25_retune_block_history_slot[i] == slot
            && state->p25_retune_block_history_until[i] > time(NULL)) {
            return 1;
        }
    }
    return 0;
}

static void
age_pending_grants(p25_sm_ctx_t* ctx, double age_s) {
    if (!ctx) {
        return;
    }
    double stale_m = dsd_time_now_monotonic_s() - age_s;
    ctx->t_tune_m = stale_m;
    for (int s = 0; s < 2; s++) {
        if (ctx->slots[s].grant_active && !ctx->slots[s].data_call) {
            ctx->slots[s].last_grant_m = stale_m;
        }
    }
}

static void
mark_cc_reacquired(dsd_state* state) {
    if (!state) {
        return;
    }
    double now_m = dsd_time_now_monotonic_s();
    if (now_m <= state->last_cc_sync_time_m) {
        now_m = state->last_cc_sync_time_m + 0.001;
    }
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = now_m;
    state->p25_last_cc_msg_time = state->last_cc_sync_time;
    state->p25_last_cc_msg_time_m = now_m;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    row.priority = priority;
    row.preempt = preempt ? 1u : 0u;
    return dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static void
init_stale_regrant_case(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t* ctx, int id) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_hangtime = 0.2f;
    opts->p25_retune_backoff_s = 10.0;
    state->p25_cc_freq = 851000000;
    state->p25_chan_iden = id;
    state->p25_iden_tdma[id].base_freq = 851000000 / 5;
    state->p25_iden_tdma[id].chan_type = 3;
    state->p25_iden_tdma[id].chan_spac = 100;
    state->p25_iden_tdma[id].trust = 2;
    state->p25_iden_tdma[id].populated = 1;
    state->p25_chan_tdma_explicit[id] = 2;
    p25_sm_init_ctx(ctx, opts, state);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
}

static void
end_tdma_call(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* grant, int slot) {
    p25_sm_event_t ptt = p25_sm_ev_ptt(slot);
    p25_sm_event_t end = p25_sm_ev_end(slot);
    p25_sm_event(ctx, opts, state, grant);
    p25_sm_event(ctx, opts, state, &ptt);
    p25_sm_event(ctx, opts, state, &end);
}

static int
test_stale_regrant_guard(void) {
    int rc = 0;
    const int id = 2;
    const int ch_slot0 = (id << 12) | 0x0002;
    const int ch_slot1 = (id << 12) | 0x0003;
    const int ch_slot1_other_freq = (id << 12) | 0x0005;

    // Constructors retain whether an event is an authoritative assignment or
    // a source-less update that may merely repeat a stale assignment.
    {
        p25_sm_event_t assignment = p25_sm_ev_group_grant(ch_slot1, 0, 5901, 6901, 0);
        p25_sm_event_t update = p25_sm_ev_group_grant_update(ch_slot1, 0, 5901, 0, P25_SM_SVC_UNKNOWN);
        rc |= expect_true("assignment provenance retained",
                          assignment.grant_provenance == P25_SM_GRANT_PROVENANCE_ASSIGNMENT);
        rc |= expect_true("update provenance retained", update.grant_provenance == P25_SM_GRANT_PROVENANCE_UPDATE);
    }

    // An immediate ambiguous update for the exact call ended by MAC_END_PTT is
    // stale. Slot remains part of the assignment identity, so a source-less
    // companion-slot update is independently eligible.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6001, 7001, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t stale = p25_sm_ev_group_grant_update(ch_slot1, 0, 6001, 0, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t companion = p25_sm_ev_group_grant_update(ch_slot0, 0, 6001, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        rc |= expect_true("stale guard call ended", g_tune_to_freq_calls == 1 && g_return_to_cc_calls == 1
                                                        && opts.trunk_is_tuned == 0 && ctx.state == P25_SM_ON_CC);
        rc |= expect_true("stale guard armed",
                          ctx.recent_call_end_valid == 1 && ctx.recent_call_end_freq_hz == g_last_tuned_vc
                              && ctx.recent_call_end_slot == 1 && ctx.recent_call_end_target == 6001
                              && fabs(ctx.t_recent_call_end_last_match_m) <= 1.0e-9);
        rc |= expect_true("normal end did not arm failed-vc backoff", retune_backoff_empty(&state));
        mark_cc_reacquired(&state);

        p25_sm_event(&ctx, &opts, &state, &stale);
        rc |= expect_true("exact stale regrant skipped",
                          g_tune_to_freq_calls == 1 && opts.trunk_is_tuned == 0 && ctx.state == P25_SM_ON_CC);

        p25_sm_event(&ctx, &opts, &state, &companion);
        rc |= expect_true("source-less companion slot allowed",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && ctx.state == P25_SM_TUNED
                              && ctx.slots[0].grant_active && ctx.recent_call_end_valid == 1);
        dsd_state_ext_free_all(&state);
    }

    // An authoritative assignment proves a new call epoch even if the system
    // reuses the same TG, carrier, slot, and source RID.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6021, 7021, 0);
        p25_sm_event_t same_rid_rekey = p25_sm_ev_group_grant(ch_slot1, 0, 6021, 7021, 0);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &same_rid_rekey);
        rc |= expect_true("authoritative same-rid rekey allowed", g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1
                                                                      && ctx.state == P25_SM_TUNED && ctx.vc_src == 7021
                                                                      && ctx.recent_call_end_valid == 0);
        dsd_state_ext_free_all(&state);
    }

    // After a slow CC reacquisition, one ambiguous exact update may validate
    // the assignment once the bounded post-END hold has elapsed. Time spent off
    // the CC does not count as a quiet gap; this path is explicitly a probe.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6051, 7051, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t delayed_update = p25_sm_ev_group_grant_update(ch_slot1, 0, 6051, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 4.0;
        ctx.t_recent_call_end_last_match_m = 0.0;
        p25_sm_event(&ctx, &opts, &state, &delayed_update);
        rc |= expect_true("delayed first exact update probed",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && ctx.state == P25_SM_TUNED
                              && ctx.vc_stale_regrant_probe == 1 && ctx.recent_call_end_valid == 0);
        dsd_state_ext_free_all(&state);
    }

    // A different talkgroup on the ended carrier/slot is a different call.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6101, 0, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t replacement = p25_sm_ev_group_grant(ch_slot1, 0, 6102, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &replacement);
        rc |= expect_true("different talkgroup allowed",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && ctx.vc_tg == 6102);
        dsd_state_ext_free_all(&state);
    }

    // The same talkgroup and slot on a different RF channel is also eligible.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6201, 0, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t replacement = p25_sm_ev_group_grant(ch_slot1_other_freq, 0, 6201, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        long ended_freq = ctx.recent_call_end_freq_hz;
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &replacement);
        rc |= expect_true("different frequency allowed",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && g_last_tuned_vc != ended_freq);
        dsd_state_ext_free_all(&state);
    }

    // A known source change on an update proves a new call even when TG/RF/slot
    // are reused.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6301, 7301, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t replacement = p25_sm_ev_group_grant_update(ch_slot1, 0, 6301, 7302, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &replacement);
        rc |= expect_true("new source allowed", g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1
                                                    && ctx.vc_src == 7302 && ctx.recent_call_end_valid == 0);
        dsd_state_ext_free_all(&state);
    }

    // Data grants do not participate in voice-call stale suppression.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        opts.trunk_tune_data_calls = 1;
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6401, 0, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t data = p25_sm_ev_group_data_grant(ch_slot1, 0, 6401, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &data);
        rc |= expect_true("data grant allowed",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && ctx.vc_data_call == 1);
        dsd_state_ext_free_all(&state);
    }

    // If the immediate CC return is rejected, no guard survives to interfere
    // with continued signaling on the still-tuned voice carrier.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6451, 0, P25_SM_SVC_UNKNOWN);

        g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_FAILED;
        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        rc |= expect_true("failed call-end return preserves vc",
                          g_tune_to_freq_calls == 1 && g_return_to_cc_calls == 1 && opts.trunk_is_tuned == 1
                              && ctx.state == P25_SM_TUNED && ctx.recent_call_end_valid == 0);
        p25_sm_event(&ctx, &opts, &state, &ended);
        rc |= expect_true("failed call-end repeat remains eligible",
                          g_tune_to_freq_calls == 1 && ctx.slots[1].grant_active == 1);
        g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
        dsd_state_ext_free_all(&state);
    }

    // A quiet gap proves the old assignment disappeared and permits the next
    // exact update without consuming the bounded validation probe.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6501, 7501, 0);
        p25_sm_event_t update = p25_sm_ev_group_grant_update(ch_slot1, 0, 6501, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &update);
        rc |= expect_true("initial ambiguous regrant skipped",
                          g_tune_to_freq_calls == 1 && opts.trunk_is_tuned == 0 && ctx.recent_call_end_valid == 1);

        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 1.5;
        ctx.t_recent_call_end_last_match_m = dsd_time_now_monotonic_s() - 0.25;
        const double previous_match_m = ctx.t_recent_call_end_last_match_m;
        p25_sm_event(&ctx, &opts, &state, &update);
        rc |= expect_true("pre-probe continuous update remains skipped",
                          g_tune_to_freq_calls == 1 && opts.trunk_is_tuned == 0 && ctx.recent_call_end_valid == 1
                              && ctx.t_recent_call_end_last_match_m > previous_match_m);

        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 4.0;
        ctx.t_recent_call_end_last_match_m = dsd_time_now_monotonic_s() - 3.0;
        p25_sm_event(&ctx, &opts, &state, &update);
        rc |= expect_true("quiet stale guard allows call", g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1
                                                               && ctx.recent_call_end_valid == 0
                                                               && ctx.vc_stale_regrant_probe == 0);
        dsd_state_ext_free_all(&state);
    }

    // Continuous updates receive one validation tune after the post-END hold.
    // If the probe sees only another END and no voice activity, failed-VC
    // backoff prevents the next stale cycle from tuning again.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6521, 7521, 0);
        p25_sm_event_t update = p25_sm_ev_group_grant_update(ch_slot1, 0, 6521, 0, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t sync = {.type = P25_SM_EV_VC_SYNC, .slot = 1};
        p25_sm_event_t end = p25_sm_ev_end(1);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &update);
        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 2.25;
        ctx.t_recent_call_end_last_match_m = dsd_time_now_monotonic_s() - 0.25;
        p25_sm_event(&ctx, &opts, &state, &update);
        rc |= expect_true("continuous exact update gets one probe",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && ctx.vc_stale_regrant_probe == 1
                              && ctx.vc_stale_regrant_probe_slot == 1);

        // Carrier/frame sync alone does not prove that this assignment became
        // active; require PTT/ACTIVE on the probed slot.
        p25_sm_event(&ctx, &opts, &state, &sync);
        p25_sm_event(&ctx, &opts, &state, &end);
        rc |= expect_true("failed probe arms slot backoff",
                          g_return_to_cc_calls == 2 && opts.trunk_is_tuned == 0 && ctx.state == P25_SM_ON_CC
                              && retune_backoff_history_has_slot(&state, g_last_tuned_vc, 1));

        mark_cc_reacquired(&state);
        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 2.25;
        ctx.t_recent_call_end_last_match_m = dsd_time_now_monotonic_s() - 0.25;
        p25_sm_event(&ctx, &opts, &state, &update);
        rc |= expect_true("backoff prevents repeated stale probe",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 0 && ctx.state == P25_SM_ON_CC);
        dsd_state_ext_free_all(&state);
    }

    // Voice activity confirms that a validation tune found a real follow-up.
    // Its normal call end must not be treated as a failed probe.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6531, 7531, 0);
        p25_sm_event_t update = p25_sm_ev_group_grant_update(ch_slot1, 0, 6531, 0, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t companion_ptt = p25_sm_ev_ptt(0);
        p25_sm_event_t companion_end = p25_sm_ev_end(0);
        p25_sm_event_t ptt = p25_sm_ev_ptt(1);
        p25_sm_event_t end = p25_sm_ev_end(1);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        p25_sm_event(&ctx, &opts, &state, &update);
        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 2.25;
        ctx.t_recent_call_end_last_match_m = dsd_time_now_monotonic_s() - 0.25;
        p25_sm_event(&ctx, &opts, &state, &update);
        p25_sm_event(&ctx, &opts, &state, &companion_ptt);
        rc |= expect_true("companion activity does not confirm stale regrant probe",
                          ctx.vc_stale_regrant_probe == 1 && ctx.vc_stale_regrant_probe_slot == 1);
        p25_sm_event(&ctx, &opts, &state, &companion_end);
        p25_sm_event(&ctx, &opts, &state, &ptt);
        rc |= expect_true("voice confirms stale regrant probe", g_tune_to_freq_calls == 2
                                                                    && ctx.vc_stale_regrant_probe == 0
                                                                    && ctx.vc_stale_regrant_probe_slot == -1);

        p25_sm_event(&ctx, &opts, &state, &end);
        rc |= expect_true("successful probe ends without failed-vc backoff",
                          g_return_to_cc_calls == 2 && opts.trunk_is_tuned == 0 && retune_backoff_empty(&state));
        dsd_state_ext_free_all(&state);
    }

    // Even continuous ambiguous updates cannot starve later calls forever.
    {
        static dsd_opts opts;
        static dsd_state state;
        p25_sm_ctx_t ctx;
        init_stale_regrant_case(&opts, &state, &ctx, id);
        p25_sm_event_t ended = p25_sm_ev_group_grant(ch_slot1, 0, 6551, 7551, 0);
        p25_sm_event_t update = p25_sm_ev_group_grant_update(ch_slot1, 0, 6551, 0, P25_SM_SVC_UNKNOWN);

        end_tdma_call(&ctx, &opts, &state, &ended, 1);
        mark_cc_reacquired(&state);
        ctx.t_recent_call_end_m = dsd_time_now_monotonic_s() - 11.0;
        ctx.t_recent_call_end_last_match_m = dsd_time_now_monotonic_s() - 0.25;
        p25_sm_event(&ctx, &opts, &state, &update);
        rc |= expect_true("stale guard max age allows call",
                          g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1 && ctx.recent_call_end_valid == 0);
        dsd_state_ext_free_all(&state);
    }

    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    install_tuning_hooks();

    // Enable trunking and seed a CC
    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_hangtime = 0.2f;
    opts.p25_grant_voice_to_s = 0.5; // apply backoff when dt_since_tune >= 0.5s
    opts.p25_retune_backoff_s = 2.0; // backoff window
    st.p25_cc_freq = 851000000;      // known CC

    // TDMA IDEN: id=2, type=3 => denom=2; trusted
    int id = 2;
    st.p25_chan_iden = id;
    // Populate new dual-array
    st.p25_iden_tdma[id].base_freq = 851000000 / 5;
    st.p25_iden_tdma[id].chan_type = 3;
    st.p25_iden_tdma[id].chan_spac = 100;
    st.p25_iden_tdma[id].trust = 2;
    st.p25_iden_tdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 2; // TDMA known

    // Two channels mapping to the same RF: low bit selects slot
    int ch_slot0 = (id << 12) | 0x0002; // slot 0
    int ch_slot1 = (id << 12) | 0x0003; // slot 1 (same RF)
    int alt_id = 3;
    int ch_slot0_alt_id = (alt_id << 12) | 0x0002; // slot 0, same RF via a different IDEN

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &opts, &st);

    // 1) Grant on slot 1 -> tune
    p25_sm_event_t ev_slot1 = p25_sm_ev_group_grant(ch_slot1, 0, 1001, 2002, 0);
    p25_sm_event(&ctx, &opts, &st, &ev_slot1);
    rc |=
        expect_true("initial tune", st.p25_sm_tune_count == 1 && opts.trunk_is_tuned == 1 && g_tune_to_freq_calls == 1);

    // 2) Let the grant timeout without any voice/VC sync; this arms backoff for slot 1.
    age_pending_grants(&ctx, 1.0);
    ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&ctx, &opts, &st);
    rc |= expect_true("returned", opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1 && ctx.state == P25_SM_ON_CC);
    rc |= expect_true("backoff armed", st.p25_retune_block_freq == g_last_tuned_vc && st.p25_retune_block_slot == 1
                                           && st.p25_retune_block_until > time(NULL));
    mark_cc_reacquired(&st);

    // 3) Same-slot repeat grant on the same RF should be blocked during backoff.
    p25_sm_event(&ctx, &opts, &st, &ev_slot1);
    rc |= expect_true("same-slot blocked", g_tune_to_freq_calls == 1 && opts.trunk_is_tuned == 0);

    // 4) Opposite-slot grant on the same RF should be allowed immediately.
    p25_sm_event_t ev_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 1001, 2002, 0);
    p25_sm_event(&ctx, &opts, &st, &ev_slot0);
    rc |= expect_true("other-slot allowed", g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 1);

    // 5) When the second slot also fails before voice, both recent failures
    // should remain blocked. This catches single-entry backoff regressions.
    age_pending_grants(&ctx, 1.0);
    ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&ctx, &opts, &st);
    rc |= expect_true("second returned",
                      opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 2 && ctx.state == P25_SM_ON_CC);
    rc |=
        expect_true("second backoff armed", st.p25_retune_block_freq == g_last_tuned_vc && st.p25_retune_block_slot == 0
                                                && st.p25_retune_block_until > time(NULL));
    mark_cc_reacquired(&st);

    p25_sm_event(&ctx, &opts, &st, &ev_slot1);
    rc |= expect_true("first failed slot still blocked", g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 0);
    p25_sm_event(&ctx, &opts, &st, &ev_slot0);
    rc |= expect_true("second failed slot blocked", g_tune_to_freq_calls == 2 && opts.trunk_is_tuned == 0);

    // 5b) Channel 0 is a valid IDEN 0 TDMA slot. When both slots on the same
    // RF fail together, channel 0/slot 0 must remain in the per-slot backoff.
    {
        static dsd_opts zero_opts;
        static dsd_state zero_st;
        DSD_MEMSET(&zero_opts, 0, sizeof zero_opts);
        DSD_MEMSET(&zero_st, 0, sizeof zero_st);
        zero_opts.trunk_enable = 1;
        zero_opts.trunk_tune_group_calls = 1;
        zero_opts.trunk_hangtime = 0.2f;
        zero_opts.p25_grant_voice_to_s = 0.5;
        zero_opts.p25_retune_backoff_s = 2.0;
        zero_st.p25_cc_freq = 851000000;
        zero_st.p25_chan_iden = 0;
        zero_st.p25_iden_tdma[0].base_freq = 851000000 / 5;
        zero_st.p25_iden_tdma[0].chan_type = 3;
        zero_st.p25_iden_tdma[0].chan_spac = 100;
        zero_st.p25_iden_tdma[0].trust = 2;
        zero_st.p25_iden_tdma[0].populated = 1;
        zero_st.p25_chan_tdma_explicit[0] = 2;

        p25_sm_ctx_t zero_ctx;
        p25_sm_init_ctx(&zero_ctx, &zero_opts, &zero_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;

        p25_sm_event_t zero_ev_slot1 = p25_sm_ev_group_grant(0x0001, 0, 3001, 4001, 0);
        p25_sm_event_t zero_ev_slot0 = p25_sm_ev_group_grant(0x0000, 0, 3000, 4000, 0);
        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot1);
        rc |= expect_true("zero channel initial tune", g_tune_to_freq_calls == 1 && zero_opts.trunk_is_tuned == 1);
        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot0);
        rc |= expect_true("zero channel same-carrier no retune", g_tune_to_freq_calls == 1);
        rc |= expect_true("zero channel both slots active",
                          zero_ctx.slots[0].grant_active && zero_ctx.slots[1].grant_active);

        age_pending_grants(&zero_ctx, 1.0);
        zero_ctx.t_voice_m = 0.0;
        p25_sm_tick_ctx(&zero_ctx, &zero_opts, &zero_st);
        rc |= expect_true("zero channel returned",
                          zero_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1 && zero_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("zero channel slot0 backoff remembered",
                          retune_backoff_history_has_slot(&zero_st, g_last_tuned_vc, 0));
        rc |= expect_true("zero channel slot1 backoff remembered",
                          retune_backoff_history_has_slot(&zero_st, g_last_tuned_vc, 1));
        mark_cc_reacquired(&zero_st);

        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot0);
        rc |= expect_true("zero channel slot0 blocked", g_tune_to_freq_calls == 1 && zero_opts.trunk_is_tuned == 0);
        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot1);
        rc |= expect_true("zero channel slot1 blocked", g_tune_to_freq_calls == 1 && zero_opts.trunk_is_tuned == 0);

        dsd_state_ext_free_all(&zero_st);
    }

    // 6) A decoder/frame-sync forced release before grant timeout still records
    // the failed VC because no voice was observed.
    static dsd_opts forced_opts;
    static dsd_state forced_st;
    DSD_MEMSET(&forced_opts, 0, sizeof forced_opts);
    DSD_MEMSET(&forced_st, 0, sizeof forced_st);
    forced_opts.trunk_enable = 1;
    forced_opts.trunk_tune_group_calls = 1;
    forced_opts.trunk_hangtime = 0.2f;
    forced_opts.p25_grant_voice_to_s = 10.0;
    forced_opts.p25_retune_backoff_s = 2.0;
    forced_st.p25_cc_freq = 851000000;
    forced_st.p25_chan_iden = id;
    forced_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    forced_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t forced_ctx;
    p25_sm_init_ctx(&forced_ctx, &forced_opts, &forced_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event(&forced_ctx, &forced_opts, &forced_st, &ev_slot1);
    rc |= expect_true("forced initial tune",
                      forced_st.p25_sm_tune_count == 1 && forced_opts.trunk_is_tuned == 1 && g_tune_to_freq_calls == 1);

    age_pending_grants(&forced_ctx, 1.0);
    forced_ctx.t_voice_m = 0.0;
    forced_st.p25_sm_force_release = 1;
    p25_sm_release(&forced_ctx, &forced_opts, &forced_st, "explicit-release");
    rc |= expect_true("forced returned",
                      forced_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1 && forced_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("forced backoff armed", forced_st.p25_retune_block_freq == g_last_tuned_vc
                                                  && forced_st.p25_retune_block_slot == 1
                                                  && forced_st.p25_retune_block_until > time(NULL));

    // 7) Data grants time out and forced-release without arming failed-voice backoff.
    static dsd_opts data_opts;
    static dsd_state data_st;
    DSD_MEMSET(&data_opts, 0, sizeof data_opts);
    DSD_MEMSET(&data_st, 0, sizeof data_st);
    data_opts.trunk_enable = 1;
    data_opts.trunk_tune_group_calls = 1;
    data_opts.trunk_tune_data_calls = 1;
    data_opts.trunk_hangtime = 0.2f;
    data_opts.p25_grant_voice_to_s = 0.5;
    data_opts.p25_retune_backoff_s = 2.0;
    data_st.p25_cc_freq = 851000000;
    data_st.p25_chan_iden = id;
    data_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    data_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t data_ctx;
    p25_sm_init_ctx(&data_ctx, &data_opts, &data_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event_t data_ev_slot1 = p25_sm_ev_group_data_grant(ch_slot1, 0, 1001, 2002, P25_SM_SVC_UNKNOWN);
    p25_sm_event(&data_ctx, &data_opts, &data_st, &data_ev_slot1);
    rc |= expect_true("data initial tune", data_st.p25_sm_tune_count == 1 && data_opts.trunk_is_tuned == 1
                                               && g_tune_to_freq_calls == 1 && data_ctx.vc_data_call == 1);

    double stale_data_tune_m = dsd_time_now_monotonic_s() - 1.0;
    data_ctx.t_tune_m = stale_data_tune_m;
    data_ctx.t_voice_m = 0.0;
    p25_sm_event(&data_ctx, &data_opts, &data_st, &data_ev_slot1);
    rc |= expect_true("data duplicate refreshed tune timer", g_tune_to_freq_calls == 1 && g_return_to_cc_calls == 0
                                                                 && data_opts.trunk_is_tuned == 1
                                                                 && data_ctx.t_tune_m > stale_data_tune_m);
    p25_sm_tick_ctx(&data_ctx, &data_opts, &data_st);
    rc |= expect_true("data duplicate avoids stale timeout",
                      data_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 0 && data_ctx.state == P25_SM_TUNED);

    data_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    data_ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&data_ctx, &data_opts, &data_st);
    rc |= expect_true("data returned",
                      data_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1 && data_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("data timeout does not arm backoff", retune_backoff_empty(&data_st));
    mark_cc_reacquired(&data_st);

    p25_sm_event(&data_ctx, &data_opts, &data_st, &ev_slot1);
    rc |= expect_true("voice grant after data timeout allowed",
                      g_tune_to_freq_calls == 2 && data_opts.trunk_is_tuned == 1 && data_ctx.vc_data_call == 0);

    static dsd_opts data_forced_opts;
    static dsd_state data_forced_st;
    DSD_MEMSET(&data_forced_opts, 0, sizeof data_forced_opts);
    DSD_MEMSET(&data_forced_st, 0, sizeof data_forced_st);
    data_forced_opts.trunk_enable = 1;
    data_forced_opts.trunk_tune_group_calls = 1;
    data_forced_opts.trunk_tune_data_calls = 1;
    data_forced_opts.trunk_hangtime = 0.2f;
    data_forced_opts.p25_grant_voice_to_s = 10.0;
    data_forced_opts.p25_retune_backoff_s = 2.0;
    data_forced_st.p25_cc_freq = 851000000;
    data_forced_st.p25_chan_iden = id;
    data_forced_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    data_forced_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t data_forced_ctx;
    p25_sm_init_ctx(&data_forced_ctx, &data_forced_opts, &data_forced_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event(&data_forced_ctx, &data_forced_opts, &data_forced_st, &data_ev_slot1);
    rc |= expect_true("forced data initial tune",
                      data_forced_st.p25_sm_tune_count == 1 && data_forced_opts.trunk_is_tuned == 1
                          && g_tune_to_freq_calls == 1 && data_forced_ctx.vc_data_call == 1);

    data_forced_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    data_forced_ctx.t_voice_m = 0.0;
    data_forced_st.p25_sm_force_release = 1;
    p25_sm_release(&data_forced_ctx, &data_forced_opts, &data_forced_st, "explicit-data-release");
    rc |= expect_true("forced data returned", data_forced_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1
                                                  && data_forced_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("forced data does not arm backoff", retune_backoff_empty(&data_forced_st));

    // 8) A stale failed-voice backoff must not suppress an explicit data grant
    // on the same TDMA channel/slot.
    static dsd_opts mixed_opts;
    static dsd_state mixed_st;
    DSD_MEMSET(&mixed_opts, 0, sizeof mixed_opts);
    DSD_MEMSET(&mixed_st, 0, sizeof mixed_st);
    mixed_opts.trunk_enable = 1;
    mixed_opts.trunk_tune_group_calls = 1;
    mixed_opts.trunk_tune_data_calls = 1;
    mixed_opts.trunk_hangtime = 0.2f;
    mixed_opts.p25_grant_voice_to_s = 0.5;
    mixed_opts.p25_retune_backoff_s = 2.0;
    mixed_st.p25_cc_freq = 851000000;
    mixed_st.p25_chan_iden = id;
    mixed_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    mixed_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t mixed_ctx;
    p25_sm_init_ctx(&mixed_ctx, &mixed_opts, &mixed_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event(&mixed_ctx, &mixed_opts, &mixed_st, &ev_slot1);
    rc |= expect_true("mixed voice initial tune", mixed_st.p25_sm_tune_count == 1 && mixed_opts.trunk_is_tuned == 1
                                                      && g_tune_to_freq_calls == 1 && mixed_ctx.vc_data_call == 0);

    age_pending_grants(&mixed_ctx, 1.0);
    mixed_ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&mixed_ctx, &mixed_opts, &mixed_st);
    rc |= expect_true("mixed voice returned",
                      mixed_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1 && mixed_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("mixed voice backoff armed", mixed_st.p25_retune_block_freq == g_last_tuned_vc
                                                       && mixed_st.p25_retune_block_slot == 1
                                                       && mixed_st.p25_retune_block_until > time(NULL));
    mark_cc_reacquired(&mixed_st);

    p25_sm_event(&mixed_ctx, &mixed_opts, &mixed_st, &data_ev_slot1);
    rc |= expect_true("data grant bypasses voice backoff",
                      g_tune_to_freq_calls == 2 && mixed_opts.trunk_is_tuned == 1 && mixed_ctx.vc_data_call == 1);

    // 9) A same-carrier data grant gets a fresh timeout window without hiding a
    // pending failed voice grant on the other TDMA slot when the carrier returns
    // to the CC.
    {
        static dsd_opts mixed_data_opts;
        static dsd_state mixed_data_st;
        DSD_MEMSET(&mixed_data_opts, 0, sizeof mixed_data_opts);
        DSD_MEMSET(&mixed_data_st, 0, sizeof mixed_data_st);
        mixed_data_opts.trunk_enable = 1;
        mixed_data_opts.trunk_tune_group_calls = 1;
        mixed_data_opts.trunk_tune_data_calls = 1;
        mixed_data_opts.trunk_hangtime = 0.2f;
        mixed_data_opts.p25_grant_voice_to_s = 0.5;
        mixed_data_opts.p25_retune_backoff_s = 2.0;
        mixed_data_st.p25_cc_freq = 851000000;
        mixed_data_st.p25_chan_iden = id;
        mixed_data_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        mixed_data_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t mixed_data_ctx;
        p25_sm_init_ctx(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t mixed_voice_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 3301, 4301, 0);
        p25_sm_event_t mixed_data_slot1 = p25_sm_ev_group_data_grant(ch_slot1, 0, 3302, 4302, P25_SM_SVC_UNKNOWN);

        p25_sm_event(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st, &mixed_voice_slot0);
        rc |= expect_true("mixed data voice tune", g_tune_to_freq_calls == 1 && mixed_data_opts.trunk_is_tuned == 1
                                                       && mixed_data_ctx.slots[0].grant_active
                                                       && mixed_data_ctx.vc_data_call == 0);
        double stale_grant_m = dsd_time_now_monotonic_s() - 1.0;
        time_t stale_grant_wall = time(NULL) - 10;
        mixed_data_ctx.t_tune_m = stale_grant_m;
        mixed_data_ctx.t_voice_m = stale_grant_m;
        mixed_data_ctx.slots[0].last_grant_m = stale_grant_m;
        mixed_data_st.last_vc_sync_time = stale_grant_wall;
        mixed_data_st.p25_last_vc_tune_time = stale_grant_wall;
        mixed_data_st.last_vc_sync_time_m = stale_grant_m;
        mixed_data_st.p25_last_vc_tune_time_m = stale_grant_m;
        p25_sm_event(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st, &mixed_data_slot1);
        rc |= expect_true("mixed data same-carrier refreshes timeout",
                          g_tune_to_freq_calls == 1 && mixed_data_ctx.vc_data_call == 1
                              && mixed_data_ctx.slots[1].data_call && mixed_data_ctx.t_tune_m > stale_grant_m);
        rc |= expect_true("mixed data clears stale voice hangtime", mixed_data_ctx.t_voice_m == 0.0);
        rc |= expect_true("mixed data refreshes vc watchdogs",
                          mixed_data_st.last_vc_sync_time > stale_grant_wall
                              && mixed_data_st.p25_last_vc_tune_time > stale_grant_wall
                              && mixed_data_st.last_vc_sync_time_m > stale_grant_m
                              && mixed_data_st.p25_last_vc_tune_time_m > stale_grant_m);
        p25_sm_tick_ctx(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st);
        rc |= expect_true("mixed data timeout window retained", mixed_data_opts.trunk_is_tuned == 1
                                                                    && g_return_to_cc_calls == 0
                                                                    && mixed_data_ctx.state == P25_SM_TUNED);
        mixed_data_ctx.t_tune_m = stale_grant_m;
        p25_sm_tick_ctx(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st);
        rc |= expect_true("mixed data returned", mixed_data_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1
                                                     && mixed_data_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("mixed data voice slot backoff",
                          retune_backoff_history_has_slot(&mixed_data_st, g_last_tuned_vc, 0));

        dsd_state_ext_free_all(&mixed_data_st);
    }

    // 10) When one slot ends while the other slot has a pending data grant,
    // the data grant stays on the refreshed grant-timeout path rather than
    // inheriting the ended slot's hangtime.
    {
        static dsd_opts pending_data_opts;
        static dsd_state pending_data_st;
        DSD_MEMSET(&pending_data_opts, 0, sizeof pending_data_opts);
        DSD_MEMSET(&pending_data_st, 0, sizeof pending_data_st);
        pending_data_opts.trunk_enable = 1;
        pending_data_opts.trunk_tune_group_calls = 1;
        pending_data_opts.trunk_tune_data_calls = 1;
        pending_data_opts.trunk_hangtime = 0.2f;
        pending_data_opts.p25_grant_voice_to_s = 0.8;
        pending_data_opts.p25_retune_backoff_s = 2.0;
        pending_data_st.p25_cc_freq = 851000000;
        pending_data_st.p25_chan_iden = id;
        pending_data_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        pending_data_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t pending_data_ctx;
        p25_sm_init_ctx(&pending_data_ctx, &pending_data_opts, &pending_data_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t pending_data_voice_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 3501, 4501, 0);
        p25_sm_event_t pending_data_slot1 = p25_sm_ev_group_data_grant(ch_slot1, 0, 3502, 4502, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t pending_data_ptt_slot0 = p25_sm_ev_ptt(0);
        p25_sm_event_t pending_data_end_slot0 = p25_sm_ev_end(0);

        p25_sm_event(&pending_data_ctx, &pending_data_opts, &pending_data_st, &pending_data_voice_slot0);
        rc |=
            expect_true("pending data initial tune", g_tune_to_freq_calls == 1 && pending_data_opts.trunk_is_tuned == 1
                                                         && pending_data_ctx.slots[0].grant_active);
        p25_sm_event(&pending_data_ctx, &pending_data_opts, &pending_data_st, &pending_data_ptt_slot0);
        pending_data_st.p25_p2_audio_allowed[0] = 1;
        rc |= expect_true("pending data voice active", pending_data_ctx.slots[0].voice_active == 1);

        p25_sm_event(&pending_data_ctx, &pending_data_opts, &pending_data_st, &pending_data_slot1);
        rc |= expect_true("pending data same-carrier accepted",
                          g_tune_to_freq_calls == 1 && pending_data_ctx.vc_data_call == 1
                              && pending_data_ctx.slots[1].grant_active && pending_data_ctx.slots[1].data_call
                              && pending_data_ctx.slots[0].voice_active);

        p25_sm_tick_ctx(&pending_data_ctx, &pending_data_opts, &pending_data_st);
        rc |= expect_true("pending data voice tick retained", pending_data_opts.trunk_is_tuned == 1
                                                                  && g_return_to_cc_calls == 0
                                                                  && pending_data_ctx.t_voice_m > 0.0);
        p25_sm_event(&pending_data_ctx, &pending_data_opts, &pending_data_st, &pending_data_end_slot0);
        rc |= expect_true("pending data blocks explicit voice release",
                          pending_data_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 0
                              && pending_data_ctx.state == P25_SM_TUNED && pending_data_ctx.slots[0].grant_active == 0
                              && pending_data_ctx.slots[1].grant_active && pending_data_ctx.slots[1].data_call);

        double now_m = dsd_time_now_monotonic_s();
        pending_data_ctx.t_voice_m = now_m - 1.0;
        pending_data_ctx.t_tune_m = now_m - 0.2;
        p25_sm_tick_ctx(&pending_data_ctx, &pending_data_opts, &pending_data_st);
        rc |= expect_true("pending data not released on ended-slot hangtime",
                          pending_data_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 0
                              && pending_data_ctx.state == P25_SM_TUNED && pending_data_ctx.t_voice_m == 0.0
                              && pending_data_ctx.slots[1].grant_active);

        now_m = dsd_time_now_monotonic_s();
        pending_data_ctx.t_tune_m = now_m - 1.0;
        p25_sm_tick_ctx(&pending_data_ctx, &pending_data_opts, &pending_data_st);
        rc |= expect_true("pending data grant timeout returned", pending_data_opts.trunk_is_tuned == 0
                                                                     && g_return_to_cc_calls == 1
                                                                     && pending_data_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("pending data timeout does not arm backoff", retune_backoff_empty(&pending_data_st));

        dsd_state_ext_free_all(&pending_data_st);
    }

    // 10b) If a data grant remains active on one TDMA slot after a later
    // same-carrier voice grant on the other slot is canceled before PTT, the
    // data-only timeout must not arm failed-voice fallback backoff for that
    // canceled voice slot.
    {
        static dsd_opts data_after_cancel_opts;
        static dsd_state data_after_cancel_st;
        DSD_MEMSET(&data_after_cancel_opts, 0, sizeof data_after_cancel_opts);
        DSD_MEMSET(&data_after_cancel_st, 0, sizeof data_after_cancel_st);
        data_after_cancel_opts.trunk_enable = 1;
        data_after_cancel_opts.trunk_tune_group_calls = 1;
        data_after_cancel_opts.trunk_tune_data_calls = 1;
        data_after_cancel_opts.trunk_hangtime = 0.2f;
        data_after_cancel_opts.p25_grant_voice_to_s = 0.8;
        data_after_cancel_opts.p25_retune_backoff_s = 2.0;
        data_after_cancel_st.p25_cc_freq = 851000000;
        data_after_cancel_st.p25_chan_iden = id;
        data_after_cancel_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        data_after_cancel_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t data_after_cancel_ctx;
        p25_sm_init_ctx(&data_after_cancel_ctx, &data_after_cancel_opts, &data_after_cancel_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t data_after_cancel_data = p25_sm_ev_group_data_grant(ch_slot1, 0, 3651, 4651, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t data_after_cancel_voice = p25_sm_ev_group_grant(ch_slot0, 0, 3652, 4652, 0);
        p25_sm_event_t data_after_cancel_end = p25_sm_ev_end(0);

        p25_sm_event(&data_after_cancel_ctx, &data_after_cancel_opts, &data_after_cancel_st, &data_after_cancel_data);
        rc |= expect_true("data-after-cancel data tuned", g_tune_to_freq_calls == 1
                                                              && data_after_cancel_opts.trunk_is_tuned == 1
                                                              && data_after_cancel_ctx.slots[1].grant_active
                                                              && data_after_cancel_ctx.slots[1].data_call);

        p25_sm_event(&data_after_cancel_ctx, &data_after_cancel_opts, &data_after_cancel_st, &data_after_cancel_voice);
        rc |=
            expect_true("data-after-cancel voice same-carrier",
                        g_tune_to_freq_calls == 1 && data_after_cancel_ctx.vc_data_call == 0
                            && data_after_cancel_ctx.slots[0].grant_active && !data_after_cancel_ctx.slots[0].data_call
                            && data_after_cancel_ctx.slots[1].grant_active && data_after_cancel_ctx.slots[1].data_call);

        p25_sm_event(&data_after_cancel_ctx, &data_after_cancel_opts, &data_after_cancel_st, &data_after_cancel_end);
        rc |= expect_true("data-after-cancel data remains",
                          data_after_cancel_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 0
                              && data_after_cancel_ctx.slots[0].grant_active == 0
                              && data_after_cancel_ctx.slots[1].grant_active && data_after_cancel_ctx.slots[1].data_call
                              && data_after_cancel_ctx.recent_call_end_valid == 0);

        data_after_cancel_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
        data_after_cancel_ctx.t_voice_m = 0.0;
        p25_sm_tick_ctx(&data_after_cancel_ctx, &data_after_cancel_opts, &data_after_cancel_st);
        rc |= expect_true("data-after-cancel returned", data_after_cancel_opts.trunk_is_tuned == 0
                                                            && g_return_to_cc_calls == 1
                                                            && data_after_cancel_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("data-after-cancel no fallback backoff", retune_backoff_empty(&data_after_cancel_st));
        mark_cc_reacquired(&data_after_cancel_st);

        p25_sm_event(&data_after_cancel_ctx, &data_after_cancel_opts, &data_after_cancel_st, &data_after_cancel_voice);
        rc |= expect_true("data-after-cancel follow-up voice allowed", g_tune_to_freq_calls == 2
                                                                           && data_after_cancel_opts.trunk_is_tuned == 1
                                                                           && data_after_cancel_ctx.vc_data_call == 0);

        dsd_state_ext_free_all(&data_after_cancel_st);
    }

    // 11) A same-target data grant on the opposite TDMA slot must not clear an
    // active voice slot for that target.
    {
        static dsd_opts same_target_opts;
        static dsd_state same_target_st;
        DSD_MEMSET(&same_target_opts, 0, sizeof same_target_opts);
        DSD_MEMSET(&same_target_st, 0, sizeof same_target_st);
        same_target_opts.trunk_enable = 1;
        same_target_opts.trunk_tune_group_calls = 1;
        same_target_opts.trunk_tune_data_calls = 1;
        same_target_opts.trunk_hangtime = 0.2f;
        same_target_opts.p25_grant_voice_to_s = 0.8;
        same_target_opts.p25_retune_backoff_s = 2.0;
        same_target_st.p25_cc_freq = 851000000;
        same_target_st.p25_chan_iden = id;
        same_target_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        same_target_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t same_target_ctx;
        p25_sm_init_ctx(&same_target_ctx, &same_target_opts, &same_target_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t same_target_voice = p25_sm_ev_group_grant(ch_slot0, 0, 3601, 4601, 0);
        p25_sm_event_t same_target_data = p25_sm_ev_group_data_grant(ch_slot1, 0, 3601, 4602, P25_SM_SVC_UNKNOWN);
        p25_sm_event_t same_target_ptt = p25_sm_ev_ptt(0);

        p25_sm_event(&same_target_ctx, &same_target_opts, &same_target_st, &same_target_voice);
        rc |= expect_true("same-target voice tuned", g_tune_to_freq_calls == 1 && same_target_opts.trunk_is_tuned == 1
                                                         && same_target_ctx.slots[0].grant_active
                                                         && same_target_ctx.slots[0].target_id == 3601
                                                         && !same_target_ctx.slots[0].data_call);
        p25_sm_event(&same_target_ctx, &same_target_opts, &same_target_st, &same_target_ptt);
        same_target_st.p25_p2_audio_allowed[0] = 1;
        rc |= expect_true("same-target voice active", same_target_ctx.slots[0].voice_active == 1);

        p25_sm_event(&same_target_ctx, &same_target_opts, &same_target_st, &same_target_data);
        rc |= expect_true("same-target data accepted", g_tune_to_freq_calls == 1 && same_target_ctx.vc_data_call == 1
                                                           && same_target_ctx.slots[1].grant_active
                                                           && same_target_ctx.slots[1].data_call
                                                           && same_target_ctx.slots[1].target_id == 3601);
        rc |= expect_true("same-target voice slot retained", same_target_ctx.slots[0].grant_active
                                                                 && same_target_ctx.slots[0].voice_active
                                                                 && same_target_ctx.slots[0].target_id == 3601
                                                                 && same_target_st.p25_p2_audio_allowed[0] == 1);

        dsd_state_ext_free_all(&same_target_st);
    }

    // 12) When one slot ends while the other slot has a pending voice grant,
    // the pending grant stays on the grant-timeout path rather than inheriting
    // the ended slot's hangtime.
    {
        static dsd_opts pending_opts;
        static dsd_state pending_st;
        DSD_MEMSET(&pending_opts, 0, sizeof pending_opts);
        DSD_MEMSET(&pending_st, 0, sizeof pending_st);
        pending_opts.trunk_enable = 1;
        pending_opts.trunk_tune_group_calls = 1;
        pending_opts.trunk_hangtime = 0.2f;
        pending_opts.p25_grant_voice_to_s = 0.8;
        pending_opts.p25_retune_backoff_s = 2.0;
        pending_st.p25_cc_freq = 851000000;
        pending_st.p25_chan_iden = id;
        pending_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        pending_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t pending_ctx;
        p25_sm_init_ctx(&pending_ctx, &pending_opts, &pending_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t pending_active_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 3401, 4401, 0);
        p25_sm_event_t pending_voice_slot1 = p25_sm_ev_group_grant(ch_slot1, 0, 3402, 4402, 0);
        p25_sm_event_t ptt_slot0 = p25_sm_ev_ptt(0);
        p25_sm_event_t end_slot0 = p25_sm_ev_end(0);

        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &pending_active_slot0);
        rc |= expect_true("pending initial tune", g_tune_to_freq_calls == 1 && pending_opts.trunk_is_tuned == 1);
        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &ptt_slot0);
        pending_st.p25_p2_audio_allowed[0] = 1;
        double stale_watchdog_m = dsd_time_now_monotonic_s() - 3.0;
        time_t stale_watchdog_wall = time(NULL) - 10;
        pending_st.last_vc_sync_time = stale_watchdog_wall;
        pending_st.p25_last_vc_tune_time = stale_watchdog_wall;
        pending_st.last_vc_sync_time_m = stale_watchdog_m;
        pending_st.p25_last_vc_tune_time_m = stale_watchdog_m;
        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &pending_voice_slot1);
        rc |= expect_true("pending same-carrier grant", g_tune_to_freq_calls == 1 && pending_ctx.slots[1].grant_active
                                                            && pending_ctx.slots[1].voice_active == 0);
        rc |= expect_true("pending same-carrier refreshes vc watchdogs",
                          pending_st.last_vc_sync_time > stale_watchdog_wall
                              && pending_st.p25_last_vc_tune_time > stale_watchdog_wall
                              && pending_st.last_vc_sync_time_m > stale_watchdog_m
                              && pending_st.p25_last_vc_tune_time_m > stale_watchdog_m);
        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &end_slot0);

        double now_m = dsd_time_now_monotonic_s();
        pending_ctx.t_voice_m = now_m - 1.0;
        pending_ctx.t_tune_m = now_m - 0.2;
        pending_ctx.slots[1].last_grant_m = pending_ctx.t_tune_m;
        p25_sm_tick_ctx(&pending_ctx, &pending_opts, &pending_st);
        rc |= expect_true("pending grant not released on ended-slot hangtime",
                          pending_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 0
                              && pending_ctx.state == P25_SM_TUNED && pending_ctx.slots[1].grant_active);

        now_m = dsd_time_now_monotonic_s();
        pending_ctx.t_tune_m = now_m - 1.0;
        pending_ctx.slots[1].last_grant_m = pending_ctx.t_tune_m;
        p25_sm_tick_ctx(&pending_ctx, &pending_opts, &pending_st);
        rc |=
            expect_true("pending grant timeout returned", pending_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1
                                                              && pending_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("pending grant timeout backoff",
                          retune_backoff_history_has_slot(&pending_st, g_last_tuned_vc, 1));
        rc |= expect_true("active slot not marked failed",
                          !retune_backoff_history_has_slot(&pending_st, g_last_tuned_vc, 0));

        dsd_state_ext_free_all(&pending_st);
    }

    // 13) A grant accepted from a MAC_IDLE payload must survive the following
    // idle event so the other slot's explicit end cannot release the VC early.
    {
        static dsd_opts idle_payload_opts;
        static dsd_state idle_payload_st;
        DSD_MEMSET(&idle_payload_opts, 0, sizeof idle_payload_opts);
        DSD_MEMSET(&idle_payload_st, 0, sizeof idle_payload_st);
        idle_payload_opts.trunk_enable = 1;
        idle_payload_opts.trunk_tune_group_calls = 1;
        idle_payload_opts.trunk_hangtime = 0.2f;
        idle_payload_opts.p25_grant_voice_to_s = 0.8;
        idle_payload_opts.p25_retune_backoff_s = 2.0;
        idle_payload_st.p25_cc_freq = 851000000;
        idle_payload_st.p25_chan_iden = id;
        idle_payload_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        idle_payload_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t idle_payload_ctx;
        p25_sm_init_ctx(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t active_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 3451, 4451, 0);
        p25_sm_event_t ptt_slot0 = p25_sm_ev_ptt(0);
        p25_sm_event_t payload_grant_slot1 = p25_sm_ev_group_grant(ch_slot1, 0, 3452, 4452, 0);
        p25_sm_event_t end_slot0 = p25_sm_ev_end(0);

        p25_sm_event(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st, &active_slot0);
        rc |= expect_true("idle-payload initial tune",
                          g_tune_to_freq_calls == 1 && idle_payload_opts.trunk_is_tuned == 1);
        p25_sm_event(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st, &ptt_slot0);
        idle_payload_st.p25_p2_audio_allowed[0] = 1;

        double idle_observed_m = dsd_time_now_monotonic_s() - 1.0;
        p25_sm_event(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st, &payload_grant_slot1);
        rc |= expect_true("idle-payload same-carrier grant", g_tune_to_freq_calls == 1
                                                                 && idle_payload_ctx.slots[1].grant_active
                                                                 && idle_payload_ctx.slots[1].voice_active == 0);

        p25_sm_event_t idle_slot1 = p25_sm_ev_idle_at(1, idle_observed_m);
        p25_sm_event(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st, &idle_slot1);
        rc |= expect_true("idle-payload grant survives idle",
                          idle_payload_ctx.slots[1].grant_active && idle_payload_ctx.slots[1].voice_active == 0);

        p25_sm_event(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st, &end_slot0);
        rc |= expect_true("idle-payload pending grant blocks explicit release",
                          idle_payload_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 0
                              && idle_payload_ctx.state == P25_SM_TUNED && idle_payload_ctx.slots[1].grant_active);

        double now_m = dsd_time_now_monotonic_s();
        idle_payload_ctx.t_voice_m = 0.0;
        idle_payload_ctx.t_tune_m = now_m - 1.0;
        idle_payload_ctx.slots[1].last_grant_m = idle_payload_ctx.t_tune_m;
        p25_sm_tick_ctx(&idle_payload_ctx, &idle_payload_opts, &idle_payload_st);
        rc |= expect_true("idle-payload grant timeout returned", idle_payload_opts.trunk_is_tuned == 0
                                                                     && g_return_to_cc_calls == 1
                                                                     && idle_payload_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("idle-payload slot backoff",
                          retune_backoff_history_has_slot(&idle_payload_st, g_last_tuned_vc, 1));

        dsd_state_ext_free_all(&idle_payload_st);
    }

    // 13b) A repeat of the same same-carrier grant during IDLE hangtime is a
    // refresh of the retained slot context, not a preemption candidate.
    {
        static dsd_opts repeat_opts;
        static dsd_state repeat_st;
        DSD_MEMSET(&repeat_opts, 0, sizeof repeat_opts);
        DSD_MEMSET(&repeat_st, 0, sizeof repeat_st);
        repeat_opts.trunk_enable = 1;
        repeat_opts.trunk_tune_group_calls = 1;
        repeat_opts.trunk_hangtime = 2.0f;
        repeat_opts.p25_grant_voice_to_s = 0.8;
        repeat_opts.p25_retune_backoff_s = 2.0;
        repeat_st.p25_cc_freq = 851000000;
        repeat_st.p25_chan_iden = id;
        repeat_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        repeat_st.p25_chan_tdma_explicit[id] = 2;
        repeat_st.p25_iden_tdma[alt_id] = st.p25_iden_tdma[id];
        repeat_st.p25_chan_tdma_explicit[alt_id] = 2;

        p25_sm_ctx_t repeat_ctx;
        p25_sm_init_ctx(&repeat_ctx, &repeat_opts, &repeat_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;

        p25_sm_event_t repeat_grant = p25_sm_ev_group_grant(ch_slot0, 0, 3461, 4461, 0);
        p25_sm_event_t repeat_alt_id_grant = p25_sm_ev_group_grant(ch_slot0_alt_id, 0, 3461, 4461, 0);
        p25_sm_event_t repeat_ptt = p25_sm_ev_ptt(0);
        p25_sm_event_t repeat_idle = p25_sm_ev_idle(0);

        p25_sm_event(&repeat_ctx, &repeat_opts, &repeat_st, &repeat_grant);
        rc |= expect_true("repeat initial tune", g_tune_to_freq_calls == 1 && repeat_opts.trunk_is_tuned == 1
                                                     && repeat_ctx.slots[0].grant_active);
        p25_sm_event(&repeat_ctx, &repeat_opts, &repeat_st, &repeat_ptt);
        repeat_st.p25_p2_audio_allowed[0] = 1;
        rc |= expect_true("repeat voice active",
                          repeat_ctx.slots[0].voice_active && repeat_ctx.slots[0].last_active_m > 0.0);

        p25_sm_event(&repeat_ctx, &repeat_opts, &repeat_st, &repeat_idle);
        repeat_st.p25_p2_audio_allowed[0] = 0;
        rc |= expect_true("repeat idle retained hangtime",
                          !repeat_ctx.slots[0].grant_active && !repeat_ctx.slots[0].voice_active
                              && repeat_ctx.slots[0].last_active_m > 0.0 && repeat_ctx.t_voice_m > 0.0);
        double old_last_grant_m = repeat_ctx.slots[0].last_grant_m;

        p25_sm_event(&repeat_ctx, &repeat_opts, &repeat_st, &repeat_alt_id_grant);
        rc |= expect_true("repeat hangtime alt id grant accepted",
                          g_tune_to_freq_calls == 1 && g_return_to_cc_calls == 0 && repeat_opts.trunk_is_tuned == 1
                              && repeat_ctx.state == P25_SM_TUNED && repeat_ctx.slots[0].grant_active);
        rc |= expect_true("repeat hangtime alt id grant refreshed",
                          repeat_ctx.slots[0].last_grant_m > old_last_grant_m
                              && repeat_ctx.slots[0].channel == ch_slot0_alt_id
                              && repeat_ctx.slots[0].last_active_m == 0.0 && repeat_ctx.t_voice_m == 0.0);

        dsd_state_ext_free_all(&repeat_st);
    }

    // 13c) A same-slot group TG and private RID can have the same numeric
    // target ID; they must not collapse into one duplicate grant context.
    {
        static dsd_opts collision_opts;
        static dsd_state collision_st;
        DSD_MEMSET(&collision_opts, 0, sizeof collision_opts);
        DSD_MEMSET(&collision_st, 0, sizeof collision_st);
        collision_opts.trunk_enable = 1;
        collision_opts.trunk_tune_group_calls = 1;
        collision_opts.trunk_tune_private_calls = 1;
        collision_opts.trunk_hangtime = 0.2f;
        collision_opts.p25_grant_voice_to_s = 0.8;
        collision_opts.p25_retune_backoff_s = 2.0;
        collision_st.p25_cc_freq = 851000000;
        collision_st.p25_chan_iden = id;
        collision_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        collision_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t collision_ctx;
        p25_sm_init_ctx(&collision_ctx, &collision_opts, &collision_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t private_grant = p25_sm_ev_indiv_grant(ch_slot0, 0, 3661, 4661, 0);
        p25_sm_event_t group_grant = p25_sm_ev_group_grant(ch_slot0, 0, 3661, 5661, 0);

        p25_sm_event(&collision_ctx, &collision_opts, &collision_st, &private_grant);
        rc |= expect_true("namespace private tuned", g_tune_to_freq_calls == 1 && collision_opts.trunk_is_tuned == 1
                                                         && collision_ctx.slots[0].grant_active
                                                         && !collision_ctx.slots[0].is_group
                                                         && collision_ctx.slots[0].dst == 3661);

        p25_sm_event(&collision_ctx, &collision_opts, &collision_st, &group_grant);
        rc |= expect_true("namespace group replaces private",
                          g_tune_to_freq_calls == 1 && collision_ctx.state == P25_SM_TUNED
                              && collision_ctx.slots[0].grant_active && collision_ctx.slots[0].is_group
                              && collision_ctx.slots[0].target_id == 3661 && collision_ctx.slots[0].ota_tg == 3661
                              && collision_ctx.slots[0].dst == 0 && collision_ctx.vc_src == 5661);

        dsd_state_ext_free_all(&collision_st);
    }

    // 14) A forced release after one TDMA slot produced voice still records a
    // pending grant on the other slot that never produced audio.
    {
        static dsd_opts forced_pending_opts;
        static dsd_state forced_pending_st;
        DSD_MEMSET(&forced_pending_opts, 0, sizeof forced_pending_opts);
        DSD_MEMSET(&forced_pending_st, 0, sizeof forced_pending_st);
        forced_pending_opts.trunk_enable = 1;
        forced_pending_opts.trunk_tune_group_calls = 1;
        forced_pending_opts.trunk_hangtime = 0.2f;
        forced_pending_opts.p25_grant_voice_to_s = 10.0;
        forced_pending_opts.p25_retune_backoff_s = 2.0;
        forced_pending_st.p25_cc_freq = 851000000;
        forced_pending_st.p25_chan_iden = id;
        forced_pending_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        forced_pending_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t forced_pending_ctx;
        p25_sm_init_ctx(&forced_pending_ctx, &forced_pending_opts, &forced_pending_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t forced_pending_active = p25_sm_ev_group_grant(ch_slot0, 0, 3701, 4701, 0);
        p25_sm_event_t forced_pending_waiting = p25_sm_ev_group_grant(ch_slot1, 0, 3702, 4702, 0);
        p25_sm_event_t forced_pending_ptt = p25_sm_ev_ptt(0);

        p25_sm_event(&forced_pending_ctx, &forced_pending_opts, &forced_pending_st, &forced_pending_active);
        rc |= expect_true("forced-pending initial tune", g_tune_to_freq_calls == 1
                                                             && forced_pending_opts.trunk_is_tuned == 1
                                                             && forced_pending_ctx.slots[0].grant_active);
        p25_sm_event(&forced_pending_ctx, &forced_pending_opts, &forced_pending_st, &forced_pending_ptt);
        forced_pending_st.p25_p2_audio_allowed[0] = 1;
        rc |= expect_true("forced-pending voice observed",
                          forced_pending_ctx.t_voice_m > 0.0 && forced_pending_ctx.slots[0].voice_active == 1);

        p25_sm_event(&forced_pending_ctx, &forced_pending_opts, &forced_pending_st, &forced_pending_waiting);
        rc |= expect_true("forced-pending same-carrier accepted", g_tune_to_freq_calls == 1
                                                                      && forced_pending_ctx.slots[1].grant_active
                                                                      && forced_pending_ctx.slots[1].voice_active == 0);

        forced_pending_st.p25_sm_force_release = 1;
        p25_sm_release(&forced_pending_ctx, &forced_pending_opts, &forced_pending_st, "forced-pending-release");
        rc |=
            expect_true("forced-pending returned", forced_pending_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 1
                                                       && forced_pending_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("forced-pending slot1 backoff",
                          retune_backoff_history_has_slot(&forced_pending_st, g_last_tuned_vc, 1));
        rc |= expect_true("forced-pending active slot not failed",
                          !retune_backoff_history_has_slot(&forced_pending_st, g_last_tuned_vc, 0));

        dsd_state_ext_free_all(&forced_pending_st);
    }

    // 15) A transient return-to-CC failure during grant timeout must not record
    // a failed VC until the retry is accepted.
    static const dsd_trunk_tune_result transient_returns[] = {
        DSD_TRUNK_TUNE_RESULT_DEFERRED,
        DSD_TRUNK_TUNE_RESULT_FAILED,
    };

    for (size_t i = 0; i < sizeof(transient_returns) / sizeof(transient_returns[0]); i++) {
        static dsd_opts transient_opts;
        static dsd_state transient_st;
        DSD_MEMSET(&transient_opts, 0, sizeof transient_opts);
        DSD_MEMSET(&transient_st, 0, sizeof transient_st);
        transient_opts.trunk_enable = 1;
        transient_opts.trunk_tune_group_calls = 1;
        transient_opts.trunk_hangtime = 0.2f;
        transient_opts.p25_grant_voice_to_s = 0.5;
        transient_opts.p25_retune_backoff_s = 2.0;
        transient_st.p25_cc_freq = 851000000;
        transient_st.p25_chan_iden = id;
        transient_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        transient_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t transient_ctx;
        p25_sm_init_ctx(&transient_ctx, &transient_opts, &transient_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        g_return_to_cc_result = transient_returns[i];
        p25_sm_event(&transient_ctx, &transient_opts, &transient_st, &ev_slot1);
        rc |= expect_true("transient initial tune", transient_st.p25_sm_tune_count == 1
                                                        && transient_opts.trunk_is_tuned == 1
                                                        && g_tune_to_freq_calls == 1);

        age_pending_grants(&transient_ctx, 1.0);
        transient_ctx.t_voice_m = 0.0;
        p25_sm_tick_ctx(&transient_ctx, &transient_opts, &transient_st);
        rc |=
            expect_true("transient return preserved vc", transient_opts.trunk_is_tuned == 1 && g_return_to_cc_calls == 1
                                                             && transient_ctx.state == P25_SM_TUNED);
        rc |= expect_true("transient return did not arm backoff",
                          transient_st.p25_retune_block_until == 0 && transient_st.p25_retune_block_freq == 0);

        g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
        p25_sm_tick_ctx(&transient_ctx, &transient_opts, &transient_st);
        rc |= expect_true("transient retry returned", transient_opts.trunk_is_tuned == 0 && g_return_to_cc_calls == 2
                                                          && transient_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("transient retry backoff armed", transient_st.p25_retune_block_freq == g_last_tuned_vc
                                                               && transient_st.p25_retune_block_slot == 1
                                                               && transient_st.p25_retune_block_until > time(NULL));
        dsd_state_ext_free_all(&transient_st);
    }
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    // 16) Same-carrier preemption releases to the CC first. The replacement
    // grant must retune the VC instead of reusing a carrier that is no longer tuned.
    {
        static dsd_opts preempt_opts;
        static dsd_state preempt_st;
        DSD_MEMSET(&preempt_opts, 0, sizeof preempt_opts);
        DSD_MEMSET(&preempt_st, 0, sizeof preempt_st);
        preempt_opts.trunk_enable = 1;
        preempt_opts.trunk_tune_group_calls = 1;
        preempt_opts.trunk_hangtime = 0.2f;
        preempt_opts.p25_retune_backoff_s = 2.0;
        preempt_st.p25_cc_freq = 851000000;
        preempt_st.p25_chan_iden = id;
        preempt_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        preempt_st.p25_chan_tdma_explicit[id] = 2;

        rc |= expect_true("seed preempt active", seed_exact(&preempt_st, 4001, "A", "PREEMPT-ACTIVE", 10, 0) == 0);
        rc |=
            expect_true("seed preempt candidate", seed_exact(&preempt_st, 4002, "A", "PREEMPT-CANDIDATE", 90, 1) == 0);
        (void)dsd_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", "0", 1);
        (void)dsd_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", "0", 1);

        p25_sm_ctx_t preempt_ctx;
        p25_sm_init_ctx(&preempt_ctx, &preempt_opts, &preempt_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t preempt_active = p25_sm_ev_group_grant(ch_slot0, 0, 4001, 5001, 0);
        p25_sm_event_t preempt_candidate = p25_sm_ev_group_grant(ch_slot0, 0, 4002, 5002, 0);

        p25_sm_event(&preempt_ctx, &preempt_opts, &preempt_st, &preempt_active);
        rc |= expect_true("preempt active tuned", g_tune_to_freq_calls == 1 && preempt_opts.trunk_is_tuned == 1
                                                      && preempt_ctx.state == P25_SM_TUNED);
        preempt_ctx.slots[0].voice_active = 1;
        preempt_ctx.slots[0].last_active_m = dsd_time_now_monotonic_s();
        preempt_ctx.t_voice_m = preempt_ctx.slots[0].last_active_m;
        preempt_st.p25_p2_audio_allowed[0] = 1;

        p25_sm_event(&preempt_ctx, &preempt_opts, &preempt_st, &preempt_candidate);
        rc |= expect_true("preempt returned to cc first", g_return_to_cc_calls == 1);
        rc |= expect_true("preempt retuned vc", g_tune_to_freq_calls == 2 && preempt_opts.trunk_is_tuned == 1
                                                    && preempt_st.p25_sm_tune_count == 2);
        rc |= expect_true("preempt replacement active", preempt_ctx.state == P25_SM_TUNED && preempt_ctx.vc_tg == 4002
                                                            && preempt_ctx.slots[0].grant_active
                                                            && preempt_ctx.slots[0].target_id == 4002);
        rc |= expect_true("preempt canceled cc acquisition",
                          preempt_ctx.cc_sync_pending == 0 && preempt_ctx.t_cc_tune_m == 0.0);

        // A duplicate carried by VC signaling must still refresh the accepted
        // call after the immediate replacement tune.
        preempt_ctx.slots[0].last_grant_m = 0.0;
        p25_sm_event(&preempt_ctx, &preempt_opts, &preempt_st, &preempt_candidate);
        rc |= expect_true("preempt duplicate grant refreshed", preempt_ctx.slots[0].last_grant_m > 0.0
                                                                   && g_tune_to_freq_calls == 2
                                                                   && preempt_ctx.state == P25_SM_TUNED);

        static const dsd_trunk_tune_result preempt_release_failures[] = {
            DSD_TRUNK_TUNE_RESULT_DEFERRED,
            DSD_TRUNK_TUNE_RESULT_FAILED,
        };
        for (size_t i = 0; i < sizeof(preempt_release_failures) / sizeof(preempt_release_failures[0]); i++) {
            static dsd_opts failed_preempt_opts;
            static dsd_state failed_preempt_st;
            DSD_MEMSET(&failed_preempt_opts, 0, sizeof failed_preempt_opts);
            DSD_MEMSET(&failed_preempt_st, 0, sizeof failed_preempt_st);
            failed_preempt_opts.trunk_enable = 1;
            failed_preempt_opts.trunk_tune_group_calls = 1;
            failed_preempt_opts.trunk_hangtime = 0.2f;
            failed_preempt_opts.p25_retune_backoff_s = 2.0;
            failed_preempt_st.p25_cc_freq = 851000000;
            failed_preempt_st.p25_chan_iden = id;
            failed_preempt_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
            failed_preempt_st.p25_chan_tdma_explicit[id] = 2;

            rc |= expect_true("seed failed-preempt active",
                              seed_exact(&failed_preempt_st, 4101, "A", "PREEMPT-FAIL-ACTIVE", 10, 0) == 0);
            rc |= expect_true("seed failed-preempt candidate",
                              seed_exact(&failed_preempt_st, 4102, "A", "PREEMPT-FAIL-CAND", 90, 1) == 0);

            p25_sm_ctx_t failed_preempt_ctx;
            p25_sm_init_ctx(&failed_preempt_ctx, &failed_preempt_opts, &failed_preempt_st);
            g_last_tuned_vc = 0;
            g_tune_to_freq_calls = 0;
            g_return_to_cc_calls = 0;
            g_return_to_cc_result = preempt_release_failures[i];

            p25_sm_event_t failed_preempt_active = p25_sm_ev_group_grant(ch_slot0, 0, 4101, 5101, 0);
            p25_sm_event_t failed_preempt_candidate = p25_sm_ev_group_grant(ch_slot0, 0, 4102, 5102, 0);
            p25_sm_event(&failed_preempt_ctx, &failed_preempt_opts, &failed_preempt_st, &failed_preempt_active);
            rc |= expect_true("failed-preempt active tuned", g_tune_to_freq_calls == 1
                                                                 && failed_preempt_opts.trunk_is_tuned == 1
                                                                 && failed_preempt_ctx.state == P25_SM_TUNED);
            failed_preempt_ctx.slots[0].voice_active = 1;
            failed_preempt_ctx.slots[0].last_active_m = dsd_time_now_monotonic_s();
            failed_preempt_ctx.t_voice_m = failed_preempt_ctx.slots[0].last_active_m;
            failed_preempt_st.p25_p2_audio_allowed[0] = 1;

            p25_sm_event(&failed_preempt_ctx, &failed_preempt_opts, &failed_preempt_st, &failed_preempt_candidate);
            rc |= expect_true("failed-preempt attempted release", g_return_to_cc_calls == 1);
            rc |= expect_true("failed-preempt did not reuse carrier",
                              g_tune_to_freq_calls == 1 && failed_preempt_st.p25_sm_tune_count == 1
                                  && failed_preempt_ctx.vc_tg == 4101 && failed_preempt_ctx.slots[0].target_id == 4101);
            rc |= expect_true("failed-preempt preserved active call",
                              failed_preempt_opts.trunk_is_tuned == 1 && failed_preempt_ctx.state == P25_SM_TUNED
                                  && failed_preempt_ctx.slots[0].voice_active == 1);

            dsd_state_ext_free_all(&failed_preempt_st);
        }
        g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

        (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
        (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");
        dsd_state_ext_free_all(&preempt_st);
    }

    // 17) MAC_END_PTT suppresses only the exact trailing Motorola-style grant
    // identity, without reusing the broader failed-VC backoff policy.
    rc |= test_stale_regrant_guard();

    dsd_state_ext_free_all(&mixed_st);
    dsd_state_ext_free_all(&data_forced_st);
    dsd_state_ext_free_all(&data_st);
    dsd_state_ext_free_all(&forced_st);
    dsd_state_ext_free_all(&st);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
