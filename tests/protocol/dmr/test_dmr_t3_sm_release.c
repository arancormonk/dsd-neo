// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// DMR SM release gating: defer when slot is active; release via tick.

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static dsd_trunk_tune_result g_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_return_to_cc_calls = 0;

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (g_tune_to_freq_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_tune_to_freq_result;
    }
    if (opts) {
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_return_to_cc_calls++;
    if (g_return_to_cc_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_to_cc_result;
    }
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.trunk_enable = 1;
    opts.trunk_hangtime = 0.5f;
    state.trunk_cc_freq = 851000000;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });

    // Initialize and get context
    dmr_sm_init(&opts, &state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx != NULL);
    assert(ctx->state == DMR_SM_ON_CC);

    // Deferred grants must not advance the DMR SM or shared tuned state.
    g_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    dmr_sm_emit_group_grant(&opts, &state, 852000000, 0, 100, 1234);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_vc_freq[0] == 0);
    assert(ctx->state == DMR_SM_ON_CC);

    // Grant to tune to VC
    g_tune_to_freq_result = DSD_TRUNK_TUNE_RESULT_OK;
    long vc = 852000000;
    dmr_sm_emit_group_grant(&opts, &state, vc, 0, 100, 1234);
    assert(opts.trunk_is_tuned == 1);
    assert(ctx->state == DMR_SM_TUNED);

    // Simulate voice active on slot 0
    dmr_sm_emit_voice_sync(&opts, &state, 0);
    assert(ctx->slots[0].voice_active == 1);

    // Tick while voice active - should NOT release
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 1);
    assert(ctx->state == DMR_SM_TUNED);

    // Mark voice inactive but with recent activity (hangtime applies)
    ctx->slots[0].voice_active = 0;
    // t_voice_m is still recent, so tick should defer

    // Tick with recent voice - should stay tuned (hangtime)
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 1);

    // Set voice timestamp far in the past to exceed hangtime
    // Use a small positive value that's clearly in the past (more than hangtime_s ago)
    double now_m = ctx->t_voice_m; // Get current monotonic time reference
    ctx->t_voice_m = now_m - 10.0; // 10 seconds ago, well past hangtime

    // A deferred return-to-CC must leave the VC state untouched for retry.
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == vc);
    assert(ctx->state == DMR_SM_TUNED);

    // Tick should now release once return-to-CC succeeds.
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 0);
    assert(ctx->state == DMR_SM_ON_CC);

    // Forced releases must retry after a deferred CC retune even if voice stays active.
    dmr_sm_emit_group_grant(&opts, &state, vc, 0, 100, 1234);
    assert(opts.trunk_is_tuned == 1);
    assert(ctx->state == DMR_SM_TUNED);
    dmr_sm_emit_voice_sync(&opts, &state, 0);
    assert(ctx->slots[0].voice_active == 1);

    state.trunk_sm_force_release = 1;
    g_return_to_cc_calls = 0;
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    dmr_sm_emit_release(&opts, &state, 0);
    assert(g_return_to_cc_calls == 1);
    assert(state.trunk_sm_force_release == 1);
    assert(opts.trunk_is_tuned == 1);
    assert(ctx->state == DMR_SM_TUNED);

    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(g_return_to_cc_calls == 2);
    assert(state.trunk_sm_force_release == 0);
    assert(opts.trunk_is_tuned == 0);
    assert(ctx->state == DMR_SM_ON_CC);

    printf("DMR_T3_SM_RELEASE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
