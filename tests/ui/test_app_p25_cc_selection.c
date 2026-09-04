// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#define CC_A 851000000L
#define CC_B 852000000L
#define VC   853000000L

static dsd_trunk_tune_result tune_result;
static int early_completion;
static uint64_t last_request;
static long last_freq;
static int last_sps;
static int cc_calls;
static int vc_calls;
static int return_calls;

static dsd_trunk_tune_result
cc_tune(dsd_opts* opts, dsd_state* state, long hz, int sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    cc_calls++;
    last_freq = hz;
    last_sps = sps;
    last_request = request_id;
    if (early_completion) {
        dsd_trunk_tuning_request_publish(request_id, early_completion > 0 ? DSD_TRUNK_TUNE_RESULT_OK
                                                                          : DSD_TRUNK_TUNE_RESULT_FAILED);
        assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    }
    return tune_result;
}

static dsd_trunk_tune_result
vc_tune(dsd_opts* opts, dsd_state* state, long hz, int sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)sps;
    (void)request_id;
    vc_calls++;
    last_freq = hz;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)request_id;
    return_calls++;
    last_freq = state->p25_cc_freq;
    assert(state->p25_cc_freq == state->trunk_cc_freq);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
setup(dsd_opts* opts, dsd_state* state) {
    initOpts(opts);
    initState(state);
    state->cli_argc_effective = 0;
    state->cli_argv = NULL;
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_TDMA, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    opts->verbose = 0;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = CC_A;
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    state->p25_cc_freq = state->trunk_cc_freq = CC_A;
    state->synctype = state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->p25_cc_is_tdma = 0;
    state->p2_wacn = 0xBEE00;
    state->p2_sysid = 0x37D;
    state->nac = 0x123;
    state->p2_cc = 0x123;
    state->p2_rfssid = 1;
    state->p2_siteid = 1;
    state->trunk_lcn_freq[0] = CC_A;
    state->lcn_freq_count = 1;
    state->p25_iden_tdma[1].base_freq = VC / 5;
    state->p25_iden_tdma[1].chan_spac = 100;
    state->p25_iden_tdma[1].chan_type = 3;
    state->p25_iden_tdma[1].trust = 2;
    state->p25_iden_tdma[1].populated = 1;
    state->p25_iden_tdma[1].wacn = state->p2_wacn;
    state->p25_iden_tdma[1].sysid = state->p2_sysid;
    state->p25_chan_tdma_explicit[1] = 2;
    state->p25_sys_is_tdma = 1;
    state->trunk_chan_map[0x1000] = VC;
    state->trunk_chan_map[0x1001] = VC;
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
    p25_sm_get_ctx()->config.cc_grace_s = 30.0;
    tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    early_completion = 0;
    cc_calls = vc_calls = return_calls = 0;
    dsd_trunk_tuning_requests_reset();
    const dsd_trunk_tuning_hooks hooks = {vc_tune, cc_tune, return_cc};
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
select_cc(dsd_opts* opts, dsd_state* state, uint32_t hz) {
#ifdef USE_RADIO
    assert(dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, hz) == DSD_APP_COMMAND_SUBMIT_QUEUED);
    assert(dsd_app_drain_cmds(opts, state) == 1);
#else
    (void)p25_sm_select_control_channel(p25_sm_get_ctx(), opts, state, (long)hz);
#endif
}

static void
start_voice(dsd_opts* opts, dsd_state* state) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    for (int slot = 0; slot < 2; slot++) {
        p25_sm_event_t grant = p25_sm_ev_group_grant(0x1000 + slot, VC, 1200 + slot, 300 + slot, 0);
        p25_sm_event(ctx, opts, state, &grant);
        p25_sm_event_t ptt = p25_sm_ev_ptt_call(slot, 1200 + slot, 0, 300 + slot, 1, 0);
        p25_sm_event(ctx, opts, state, &ptt);
        dsd_call_snapshot call;
        assert(dsd_call_state_get(state, (uint8_t)slot, &call) == 1);
        assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    }
    assert(ctx->vc_is_tdma == 1);
    state->synctype = state->lastsynctype = DSD_SYNC_P25P2_POS;
    state->p25_sm_force_release = 1;
    state->payload_mi = state->payload_miR = 42;
    state->audio_out_temp_buf[0] = 1.0f;
    state->audio_out_temp_bufR[0] = 1.0f;
}

static void
acquire_cc(dsd_opts* opts, dsd_state* state) {
    state->nac = 0x456;
    state->p2_cc = 0x456;
    state->p2_rfssid = 2;
    state->p2_siteid = 3;
    state->synctype = state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->p25_last_cc_msg_time_m = dsd_time_now_monotonic_s() + 0.001;
    p25_sm_event_t sync = {0};
    sync.type = P25_SM_EV_CC_SYNC;
    p25_sm_event(p25_sm_get_ctx(), opts, state, &sync);
    assert(!p25_sm_get_ctx()->cc_sync_pending);
    assert(p25_sm_get_ctx()->expected_cc_nac == 0x456);
}

static void
test_selection(dsd_opts* opts, dsd_state* state, int voice, int cc_type) {
    setup(opts, state);
    if (voice) {
        start_voice(opts, state);
    }
    state->p25_cc_is_tdma = cc_type;
    assert(p25_cc_add_candidate(state, CC_A + 25000, 1) == 1);
    dsd_trunk_cc_candidates_set_cooldown(state, CC_A + 25000, 900.0);
    state->p25_pending_announcement_count = 1;
    state->p25_secondary_cc_count = 1;
    state->p25_cc_eval_freq = CC_A + 25000;
    state->p25_last_cc_msg_time_m = 42.0;
    const uint32_t grants = p25_sm_get_ctx()->grant_count;
    const uint32_t releases = p25_sm_get_ctx()->release_count;
    const uint64_t generation = dsd_trunk_tuning_generation();
    select_cc(opts, state, CC_B);
    assert(cc_calls == 1 && last_freq == CC_B && return_calls == 0);
    assert(last_sps
           == dsd_opts_compute_sps_rate(opts, cc_type == 1 ? 6000 : 4800, dsd_opts_current_input_timing_rate(opts)));
    assert(state->samplesPerSymbol == last_sps);
    assert(state->sps_hunt_idx
           == (cc_type == 1 ? DSD_FRAME_SYNC_SPS_PROFILE_6000_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4));
    assert(p25_sm_get_ctx()->grant_count == grants && p25_sm_get_ctx()->release_count == releases);
    assert(opts->trunk_enable == 1 && opts->trunk_is_tuned == 0);
    assert(state->p25_cc_freq == CC_B && state->trunk_cc_freq == CC_B && opts->rtlsdr_center_freq == CC_B);
    assert(state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);
    assert(state->trunk_lcn_freq[0] == CC_B && state->lcn_freq_count == 1);
    assert(state->p2_wacn == 0xBEE00 && state->p2_sysid == 0x37D);
    assert(state->p2_cc == 0 && state->nac == 0 && p25_sm_get_ctx()->expected_cc_nac == 0);
    assert(state->p2_rfssid == 0 && state->p2_siteid == 0);
    assert(state->p25_iden_tdma[1].base_freq == VC / 5 && state->p25_iden_tdma[1].trust == 2);
    assert(state->trunk_chan_map[0x1000] == VC && state->p25_sys_is_tdma == 1);
    assert(dsd_trunk_cc_candidates_peek(state)->count == 0);
    assert(state->p25_pending_announcement_count == 0 && state->p25_secondary_cc_count == 0);
    assert(!state->p25_sm_force_release && !state->payload_mi && !state->payload_miR);
    assert(fabsf(state->audio_out_temp_buf[0]) < 0.0001f && fabsf(state->audio_out_temp_bufR[0]) < 0.0001f);
    assert(!dsd_trunk_tuning_frame_is_current(generation));
    if (voice) {
        for (int slot = 0; slot < 2; slot++) {
            dsd_call_snapshot call;
            assert(dsd_call_state_get(state, (uint8_t)slot, &call) == 1 && call.phase == DSD_CALL_PHASE_ENDED);
            assert(state->event_history_s[slot].Event_History_Items[1].target_id == (uint32_t)(1200 + slot));
        }
    }
    acquire_cc(opts, state);
    start_voice(opts, state);
    p25_sm_release(p25_sm_get_ctx(), opts, state, "test-return");
    assert(return_calls == 1 && last_freq == CC_B);
    freeState(state);
}

static void
test_rejected(dsd_opts* opts, dsd_state* state, dsd_trunk_tune_result result) {
    setup(opts, state);
    start_voice(opts, state);
    tune_result = result;
    const p25_sm_ctx_t before = *p25_sm_get_ctx();
    select_cc(opts, state, CC_B);
    const p25_sm_ctx_t* after = p25_sm_get_ctx();
    assert(after->state == before.state && after->vc_freq_hz == before.vc_freq_hz);
    assert(after->cc_tune_request_id == before.cc_tune_request_id && after->cc_sync_pending == before.cc_sync_pending);
    assert(after->expected_cc_nac == before.expected_cc_nac && after->grant_count == before.grant_count);
    assert(fabs(after->t_tune_m - before.t_tune_m) < 0.0001);
    assert(fabs(after->t_cc_sync_m - before.t_cc_sync_m) < 0.0001);
    for (int slot = 0; slot < 2; slot++) {
        assert(after->slots[slot].voice_active == before.slots[slot].voice_active);
        assert(after->slots[slot].target_id == before.slots[slot].target_id);
        assert(fabs(after->slots[slot].last_active_m - before.slots[slot].last_active_m) < 0.0001);
    }
    assert(state->p25_cc_freq == CC_A && state->p2_siteid == 1 && state->nac == 0x123);
    assert(opts->trunk_is_tuned == 1 && state->p25_vc_freq[0] == VC);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 0, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE);
    freeState(state);
}

static void
test_pending(dsd_opts* opts, dsd_state* state, int early, int fail) {
    setup(opts, state);
    start_voice(opts, state);
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    early_completion = 0;
    if (early) {
        early_completion = fail ? -1 : 1;
    }
    select_cc(opts, state, CC_B);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    const uint64_t request = last_request;
    assert(ctx->cc_sync_pending);
    if (early && !fail) {
        assert(!ctx->cc_tune_pending); // wrapper observes completion before returning
    } else {
        assert(ctx->cc_tune_pending && ctx->cc_tune_request_id == request);
    }
    if (!early) {
        assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
        ctx->t_cc_sync_m = dsd_time_now_monotonic_s() - 100.0;
        p25_sm_tick_ctx(ctx, opts, state);
        assert(ctx->cc_tune_pending && cc_calls == 1);
        dsd_trunk_tuning_request_publish(request, fail ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_OK);
    }
    if (fail) {
        tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    p25_sm_tick_ctx(ctx, opts, state);
    assert(!ctx->cc_tune_pending && state->p25_cc_freq == CC_B);
    if (fail) {
        assert(ctx->state == P25_SM_HUNTING);
        assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
        assert(last_freq == CC_B && cc_calls == 2);
        tune_result = DSD_TRUNK_TUNE_RESULT_OK;
        ctx->t_hunt_try_m = 0.0;
        p25_sm_tick_ctx(ctx, opts, state);
        assert(last_freq == CC_B && cc_calls == 3);
    } else {
        double completed_m = 0.0;
        assert(dsd_trunk_tuning_request_status(request, &completed_m) == DSD_TRUNK_TUNE_RESULT_OK);
        assert(fabs(ctx->t_cc_tune_m - completed_m) < 0.01);
        assert(ctx->state == P25_SM_ON_CC && ctx->cc_sync_pending);
        p25_sm_tick_ctx(ctx, opts, state);
        assert(cc_calls == 1);
    }
    const uint64_t generation = dsd_trunk_tuning_generation();
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_generation() == generation);
    freeState(state);
}

static void
test_superseded_completion(dsd_opts* opts, dsd_state* state) {
    setup(opts, state);
    tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    select_cc(opts, state, CC_B);
    const uint64_t older = last_request;
    select_cc(opts, state, CC_B + 25000);
    const uint64_t newer = last_request;
    assert(newer != older);
    dsd_trunk_tuning_request_publish(older, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick_ctx(p25_sm_get_ctx(), opts, state);
    assert(p25_sm_get_ctx()->cc_tune_pending && p25_sm_get_ctx()->cc_tune_request_id == newer);
    assert(state->p25_cc_freq == CC_B + 25000);
    assert(!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    dsd_trunk_tuning_request_publish(newer, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick_ctx(p25_sm_get_ctx(), opts, state);
    assert(!p25_sm_get_ctx()->cc_tune_pending && state->p25_cc_freq == CC_B + 25000);
    const uint64_t generation = dsd_trunk_tuning_generation();
    dsd_trunk_tuning_request_publish(older, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_frame_is_current(generation));
    freeState(state);
}

static void
test_overrides(dsd_opts* opts, dsd_state* state) {
    setup(opts, state);
    state->p2_hardset = 1;
    opts->mod_cli_lock = 1;
    opts->mod_qpsk = 1;
    opts->mod_c4fm = 0;
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "user.csv");
    state->p25_bandplan_row_count = 1;
    state->p25_bandplan_rows[0].iden = 1;
    state->p25_bandplan_rows[0].is_tdma = 1;
    state->p25_bandplan_rows[0].entry = state->p25_iden_tdma[1];
    state->keyloader = 0;
    state->R = 0x12345;
    const dsd_tg_policy_entry group = {.id_start = 1200, .id_end = 1200, .mode = "A", .name = "test"};
    assert(dsd_tg_policy_append_exact(state, &group) == 0);
    select_cc(opts, state, CC_B);
    assert(state->p2_cc == 0x123 && state->p2_hardset == 1);
    assert(opts->mod_cli_lock == 1 && opts->mod_qpsk == 1 && opts->mod_c4fm == 0);
    assert(state->trunk_lcn_freq[0] == CC_A && state->p25_bandplan_row_count == 1);
    dsd_tg_policy_lookup lookup;
    assert(state->R == 0x12345);
    assert(dsd_tg_policy_lookup_id(state, 1200, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(p25_update_system_identity(state, 0xBEE00, 0x37D) == 1);
    assert(state->p25_iden_tdma[1].base_freq == VC / 5 && state->trunk_chan_map[0x1000] == VC);
    assert(p25_update_system_identity(state, 0xBEE00, 0x123) == 1);
    assert(state->p25_iden_tdma[1].populated == 0);
    assert(state->trunk_chan_map[0x1000] == 0 && state->p25_bandplan_row_count == 1);
    freeState(state);
}

static void
test_cache(dsd_opts* opts, dsd_state* state) {
    char dir[DSD_TEST_PATH_MAX];
    char legacy[DSD_TEST_PATH_MAX];
    char site[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_manual_cc"));
    assert(dsd_test_path_join(legacy, sizeof(legacy), dir, "p25_cc_BEE00_37D.txt") == 0);
    assert(dsd_test_path_join(site, sizeof(site), dir, "p25_cc_BEE00_37D_R002_S003.txt") == 0);
    FILE* fp = dsd_fopen_private(legacy, "w");
    assert(fp);
    DSD_FPRINTF(fp, "cc %ld\n", CC_A);
    assert(fclose(fp) == 0);
    fp = dsd_fopen_private(site, "w");
    assert(fp);
    DSD_FPRINTF(fp, "cc %ld\n", CC_B + 25000);
    assert(fclose(fp) == 0);
    assert(dsd_test_setenv("DSD_NEO_CACHE_DIR", dir, 1) == 0);
    assert(dsd_test_setenv("DSD_NEO_CC_CACHE", "1", 1) == 0);
    dsd_neo_config_init();
    setup(opts, state);
    select_cc(opts, state, CC_B);
    const long no_neighbor = 0;
    p25_cc_record_neighbor_frequencies(opts, state, &no_neighbor, 1);
    assert(state->p25_cc_cache_loaded == 0);
    noCarrier(opts, state);
    p25_cc_record_neighbor_frequencies(opts, state, &no_neighbor, 1);
    assert(state->p25_cc_cache_loaded == 0);
    assert(dsd_trunk_cc_candidates_peek(state)->count == 0);
    state->p2_rfssid = 2;
    state->p2_siteid = 3;
    p25_cc_record_neighbor_frequencies(opts, state, &no_neighbor, 1);
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    assert(state->p25_cc_cache_loaded && cc && cc->count == 1 && cc->candidates[0] == CC_B + 25000);
    freeState(state);
    assert(remove(legacy) == 0 && remove(site) == 0);
}

#ifdef USE_RADIO
static void
test_command_scope(dsd_opts* opts, dsd_state* state) {
    setup(opts, state);
    opts->trunk_scan_enabled = 1;
    select_cc(opts, state, CC_B);
    assert(cc_calls == 0 && state->p25_cc_freq == CC_A);
    opts->trunk_scan_enabled = 0;
    select_cc(opts, state, 0);
    assert(cc_calls == 0);
#if LONG_MAX < UINT32_MAX
    select_cc(opts, state, UINT32_MAX);
    assert(cc_calls == 0);
#endif
    opts->trunk_enable = 0;
    select_cc(opts, state, CC_B);
    assert(cc_calls == 0 && state->p25_cc_freq == CC_A);
    opts->scanner_mode = 1;
    select_cc(opts, state, CC_B);
    assert(cc_calls == 0);
    opts->scanner_mode = 0;
    opts->trunk_enable = 1;
    state->synctype = state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    select_cc(opts, state, CC_B);
    assert(cc_calls == 0 && state->p25_cc_freq == CC_A);
    state->synctype = state->lastsynctype = DSD_SYNC_NONE;
    select_cc(opts, state, CC_B); // mixed -ft without P25 evidence stays generic
    assert(cc_calls == 0);
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P1, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    select_cc(opts, state, CC_B);
    assert(cc_calls == 1 && state->p25_cc_freq == CC_B);
    freeState(state);
}
#endif

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    for (int voice = 0; voice < 2; voice++) {
        for (int type = -1; type <= 1; type++) {
            test_selection(opts, state, voice, type);
        }
    }
    test_rejected(opts, state, DSD_TRUNK_TUNE_RESULT_FAILED);
    test_rejected(opts, state, DSD_TRUNK_TUNE_RESULT_DEFERRED);
    test_pending(opts, state, 0, 0);
    test_pending(opts, state, 1, 0);
    test_pending(opts, state, 0, 1);
    test_pending(opts, state, 1, 1);
    test_superseded_completion(opts, state);
    test_overrides(opts, state);
    test_cache(opts, state);
#ifdef USE_RADIO
    test_command_scope(opts, state);
#endif
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    free(state);
    free(opts);
    return 0;
}
