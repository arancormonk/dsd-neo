// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * End-to-end DMR Tier III trunking shim tests:
 * - Neighbor/alternate CC candidates
 * - Explicit frequency grants
 * - LPCN-derived grants with trust gating (on-CC vs off-CC)
 * - Release handling via tick with slot activity + hangtime
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
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

// Provide local stubs to satisfy DMR SM linker deps
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
init_env(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->use_rigctl = 0;
    opts->audio_in_type = AUDIO_IN_PULSE;
    state->trunk_cc_freq = 851000000; // mock CC
}

static void
test_neighbor_candidates(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);

    long cand[3] = {851012500, 852500000, 0};
    dmr_sm_on_neighbor_update(&opts, &state, cand, 3);
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&state);
    assert(cc != NULL);
    assert(cc->count >= 2);
}

static void
test_explicit_grant_and_release(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    opts.trunk_hangtime = 0.5f;

    // Initialize SM
    dmr_sm_init(&opts, &state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx != NULL);

    long vc = 852000000;
    dmr_sm_emit_group_grant(&opts, &state, vc, /*lpcn*/ 0, /*tg*/ 1001, /*src*/ 42);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == vc);
    assert(ctx->state == DMR_SM_TUNED);
    assert(ctx->vc_slot == -1);
    assert(state.dmr_mono_slot == -1);

    // Voice active on slot 0
    dmr_sm_emit_voice_sync(&opts, &state, 0);
    assert(ctx->slots[0].voice_active == 1);
    assert(ctx->vc_slot == 0);
    assert(state.dmr_mono_slot == 0);

    // Tick while voice active - should stay tuned
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 1);

    // Mark voice inactive (but hangtime not expired yet)
    ctx->slots[0].voice_active = 0;
    // t_voice_m was just set, so within hangtime

    // Tick - should stay tuned (hangtime)
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 1);

    // Set voice timestamp far in past to exceed hangtime
    double now_m = ctx->t_voice_m; // Get current monotonic time reference
    ctx->t_voice_m = now_m - 10.0; // 10 seconds ago, well past hangtime

    // Tick - should release
    dmr_sm_tick_ctx(ctx, &opts, &state);
    assert(opts.trunk_is_tuned == 0);
    assert(ctx->state == DMR_SM_ON_CC);
}

static void
test_lpcn_trust_gating(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);

    // Initialize SM
    dmr_sm_init(&opts, &state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();

    // On CC (trunk_is_tuned==0): allow tuning with untrusted LPCN mapping
    int lpcn = 0x0123;
    long f1 = 853000000;
    state.trunk_chan_map[lpcn] = f1;
    state.dmr_lcn_trust[lpcn] = 1; // unconfirmed
    opts.trunk_is_tuned = 0;       // on CC
    ctx->state = DMR_SM_ON_CC;
    dmr_sm_emit_group_grant(&opts, &state, /*freq_hz*/ 0, lpcn, /*tg*/ 101, /*src*/ 99);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f1);

    // Off CC (currently tuned to VC): block tune with untrusted mapping
    int lpcn2 = 0x0124;
    long f2 = 854000000;
    state.trunk_chan_map[lpcn2] = f2;
    state.dmr_lcn_trust[lpcn2] = 1; // unconfirmed
    long prev = state.trunk_vc_freq[0];
    opts.trunk_is_tuned = 1; // off CC
    dmr_sm_emit_group_grant(&opts, &state, /*freq_hz*/ 0, lpcn2, /*tg*/ 101, /*src*/ 99);
    assert(state.trunk_vc_freq[0] == prev); // unchanged (blocked)
}

static void
test_grant_identity_seeds_matching_voice_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;

    dmr_sm_init(&opts, &state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_emit_group_grant_slot(&opts, &state, 855012500L, 0x245, 1, 3101, 4202);
    assert(ctx->state == DMR_SM_TUNED);
    assert(ctx->vc_slot == 1);
    assert(state.dmr_mono_slot == 1);

    dsd_call_snapshot call;
    dmr_sm_emit_voice_sync(&opts, &state, 0);
    assert(dsd_call_state_get(&state, 0U, &call) == 0);
    assert(ctx->vc_identity_published == 0);
    assert(state.dmr_mono_slot == 1);

    dmr_sm_emit_voice_sync(&opts, &state, 1);
    assert(dsd_call_state_get(&state, 1U, &call) == 1);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(call.ota_target_id == 3101U);
    assert(call.policy_target_id == 3101U);
    assert(call.ota_source_id == 4202U);
    assert(call.channel == 0x245U);
    assert(call.frequency_hz == 855012500L);
    assert(ctx->vc_identity_published == 1);

    dsd_state_ext_free_all(&state);
}

static void
test_slotless_grant_selects_ts2_on_voice_sync(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;

    dmr_sm_init(&opts, &state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_emit_group_grant(&opts, &state, 855037500L, 0x247, 7101, 8202);
    assert(ctx->state == DMR_SM_TUNED);
    assert(ctx->vc_slot == -1);
    assert(state.dmr_mono_slot == -1);

    dmr_sm_emit_voice_sync(&opts, &state, 1);
    assert(ctx->vc_slot == 1);
    assert(state.dmr_mono_slot == 1);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 1U, &call) == 1);
    assert(call.ota_target_id == 7101U);
    assert(call.ota_source_id == 8202U);

    dsd_state_ext_free_all(&state);
}

static void
test_voice_header_and_first_sync_share_epoch(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;

    dmr_sm_init(&opts, &state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_emit_group_grant_slot(&opts, &state, 855025000L, 0x246, 0, 5101, 6202);
    assert(ctx->state == DMR_SM_TUNED);

    const dsd_call_observation voice_header = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 5101U,
        .policy_target_id = 5101U,
        .ota_source_id = 6202U,
        .channel = 0x246U,
        .frequency_hz = 855025000L,
        .service_options = 0x83U,
        .emergency = 1U,
        .priority = 3U,
        .has_service_metadata = 1U,
    };
    assert(dsd_call_state_observe(&state, &voice_header, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot before;
    assert(dsd_call_state_get(&state, 0U, &before) == 1);

    dmr_sm_emit_voice_sync(&opts, &state, 0);

    dsd_call_snapshot after;
    assert(dsd_call_state_get(&state, 0U, &after) == 1);
    assert(after.epoch == before.epoch);
    assert(after.service_options == 0x83U);
    assert(after.emergency == 1U);
    assert(after.priority == 3U);
    assert(ctx->vc_identity_published == 1);

    dsd_state_ext_free_all(&state);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });
    test_neighbor_candidates();
    test_explicit_grant_and_release();
    test_lpcn_trust_gating();
    test_grant_identity_seeds_matching_voice_slot();
    test_slotless_grant_selects_ts2_on_voice_sync();
    test_voice_header_and_first_sync_share_epoch();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    printf("DMR_T3_SHIM: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
