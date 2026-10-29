// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Real scan coordinator, control-message parser and sync-loss callbacks, with
 * only the tuner replaced. In particular, neither protocol SM is stubbed. */
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_cc_activity.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/snapshot_internal.h"

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/platform/platform.h"
#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "engine_hooks_install.h"
#include "frame_sync_test_support.h"
#include "test_env.h"
#include "test_support.h"
#include "trunk_scan_internal.h"

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static dsd_opts g_opts;
static dsd_state g_state;
static char g_dir[DSD_TEST_PATH_MAX];
static char g_targets[DSD_TEST_PATH_MAX];
static char g_channels[DSD_TEST_PATH_MAX];
static long g_frequency;
static int g_cc_tunes;
static int g_returns;
static int g_queries;
static int g_query_failed;
static uint64_t g_request;
static dsd_trunk_tune_result g_result;
static int g_hw_model; // 1: the radio moves only when a tune succeeds (startup-hunt cases, issue #703)

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

static long
current_frequency(const dsd_opts* opts) {
    (void)opts;
    ++g_queries;
    return g_query_failed ? 0 : g_frequency;
}

static dsd_trunk_tune_result
cc_tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)sps;
    ++g_cc_tunes;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_result)) {
        if (!g_hw_model || g_result == DSD_TRUNK_TUNE_RESULT_OK) {
            g_frequency = freq;
        }
        opts->rtlsdr_center_freq = (uint32_t)freq;
        state->trunk_cc_freq = freq; // The real shared tune helper stages this field.
        dsd_mark_cc_sync(state);
    }
    return g_result;
}

static dsd_trunk_tune_result
vc_tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)sps;
    (void)request_id;
    g_frequency = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    ++g_returns;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_result)) {
        g_frequency = state->p25_cc_freq > 0 ? state->p25_cc_freq : state->trunk_cc_freq;
        opts->rtlsdr_center_freq = (uint32_t)g_frequency;
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        dsd_mark_cc_sync(state);
    }
    return g_result;
}

static int
write_file(const char* path, const char* content) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return 1;
    }
    const size_t len = strlen(content);
    const int failed = fwrite(content, 1, len, fp) != len;
    return fclose(fp) != 0 || failed;
}

static void
cleanup(void) {
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    dsd_frame_sync_hooks_set(NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_trunk_tuning_requests_reset();
    trunk_scan_test_clear_now();
    freeState(&g_state);
    (void)remove(g_targets);
    (void)remove(g_channels);
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(g_dir);
#else
    (void)rmdir(g_dir);
#endif
}

static int
setup(float hangtime) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    initOpts(&g_opts);
    initState(&g_state);
    g_opts.verbose = 0;
    g_opts.audio_in_type = AUDIO_IN_NULL;
    g_opts.audio_out_type = 9;
    g_opts.use_rigctl = 1;
    g_opts.trunk_scan_enabled = 1;
    g_opts.trunk_hangtime = hangtime;
    g_cc_tunes = g_returns = g_queries = g_query_failed = 0;
    g_hw_model = 0;
    g_request = 0U;
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_trunk_tuning_requests_reset();
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);
    dsd_engine_frame_sync_hooks_install();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = vc_tune, .tune_to_cc_request = cc_tune, .return_to_cc_request = return_to_cc});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = current_frequency});
    if (!dsd_test_mkdtemp(g_dir, sizeof(g_dir), "dsdneo_dmr_cc")
        || dsd_test_path_join(g_targets, sizeof(g_targets), g_dir, "targets.csv") != 0
        || dsd_test_path_join(g_channels, sizeof(g_channels), g_dir, "channels.csv") != 0
        || write_file(g_channels, "channel,frequency\n1,451000000\n2,452000000\n3,452000000\n4,453000000\n")
        || write_file(g_targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                 "tiii,dmr-trunk,451000000,channels.csv,250,1200,\n"
                                 "p25,p25-trunk,851000000,,250,1200,\n"
                                 "nxdn,nxdn-trunk,454000000,,250,1200,\n")) {
        return 1;
    }
    DSD_SNPRINTF(g_opts.trunk_scan_targets_csv, sizeof(g_opts.trunk_scan_targets_csv), "%s", g_targets);
    trunk_scan_test_set_now(0.0);
    char error[256] = {0};
    const int rc = dsd_engine_trunk_scan_init(&g_opts, &g_state, error, sizeof(error));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "scan init: %s\n", error);
        return 1;
    }
    return expect(dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 1,
                  "hold enabled");
}

static void
aloha(int crc_ok) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 25;
    for (int i = 0; i < 8; ++i) {
        bits[i] = (uint8_t)((bytes[0] >> (7 - i)) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, (uint32_t)crc_ok, 0);
}

static void
expire_acquisition(dmr_sm_ctx_t* ctx) {
    ctx->cc_acquire_start_m = ctx->t_cc_sync_m = dsd_decode_now_mono_s() - ctx->cc_grace_s - 1.0;
    ctx->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
}

/* A voice sync on a slot whose own voice passed the colour-code gate, as an accepted voice burst leaves it: only that
   holds a followed call (a voice sync alone never does). */
static void
test_accepted_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    state->dmr_confidence_voice_open[slot] = 1;
    state->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, slot);
}

static int
held_call_return(float hangtime) {
    if (setup(hangtime)) {
        cleanup();
        return 1;
    }
    int rc = 0;
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    ctx->t_cc_sync_m -= ctx->cc_grace_s + 1.0;
    aloha(1);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_ON_CC && ctx->cc_confirmed, "decoded ALOHA keeps DMR on CC");
    rc |= expect(!g_opts.frame_p25p1 && !g_opts.frame_p25p2, "DMR row excludes P25");
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == 451000000L, "control-like messages on a followed VC cannot replace CC");
    test_accepted_voice_sync(&g_opts, &g_state, 0);
    ctx->slots[0].last_active_m -= 1.0;
    ctx->t_voice_m -= 1.0;
    g_state.is_con_plus = 1;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(g_returns == (hangtime == 0.0f ? 1 : 0), "one-second gap distinguishes zero and two-second hangtime");
    if (hangtime > 0.0f) {
        ctx->slots[0].last_active_m -= 2.0;
        ctx->t_voice_m -= 2.0;
    }
    g_state.last_vc_sync_time = time(NULL) - 3;
    g_state.last_vc_sync_time_m = dsd_decode_now_mono_s() - 3.0;
    g_state.p25_last_vc_tune_time_m = dsd_decode_now_mono_s() - 4.0;
    dsd_frame_sync_test_handle_no_sync_timeout(&g_opts, &g_state, 1800);
    rc |= expect(g_returns == 1 && g_frequency == 451000000L, "DMR returned to original control channel");
    rc |= expect(!g_state.is_con_plus, "DMR-owned release clears Con+ follow latch");
    rc |= expect(p25->state == P25_SM_IDLE && !p25->cc_sync_pending && !g_state.p25_sm_force_release,
                 "DMR loss did not start P25 recovery or leave a forced-release latch");
    const int tunes = g_cc_tunes;
    for (int i = 0; i < 3; ++i) {
        ctx->cc_acquire_start_m -= 6.0;
        aloha(1);
        p25_sm_try_tick(&g_opts, &g_state);
        trunk_scan_test_set_now(10.0 + i);
        dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    }
    rc |= expect(g_cc_tunes == tunes && g_state.trunk_cc_freq == 451000000L && g_state.trunk_scan_hold,
                 "held target stays on decoded idle CC");
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    trunk_scan_test_set_now(20.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    trunk_scan_test_set_now(20.26);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_engine_trunk_scan_active_p25_ctx() != NULL, "idle dwell rotates after hold release");
    rc |= expect(dsd_trunk_p25_recovery_allowed(&g_opts, &g_state), "P25 target retains its recovery owner");
    cleanup();
    return rc;
}

static int
hunt_and_reacquire(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    int rc = 0;
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    aloha(1);
    expire_acquisition(ctx);
    rc |= expect(g_frequency == 452000000L && g_state.trunk_cc_freq == 451000000L,
                 "probe moved tuner without replacing CC anchor");
    g_opts.dmr_crc_relaxed_default = 1;
    aloha(0);
    rc |= expect(ctx->cc_acquiring && g_state.trunk_cc_freq == 451000000L,
                 "relaxed heartbeat cannot validate a new candidate");
    expire_acquisition(ctx);
    rc |= expect(g_frequency == 451000000L, "hunt revisits the anchor between alternate probes");
    expire_acquisition(ctx);
    rc |= expect(g_frequency == 453000000L, "hunt skips adjacent duplicate channel frequencies");
    aloha(1);
    rc |= expect(!ctx->cc_acquiring && ctx->cc_confirmed && g_state.trunk_cc_freq == 453000000L,
                 "decoded control channel replaces the old anchor");
    ctx->t_cc_sync_m -= ctx->cc_grace_s + 1.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_HUNTING, "expired heartbeat enters recovery");
    aloha(0);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_ON_CC, "relaxed heartbeat retains an established CC");
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    rc |= expect(!dsd_trunk_p25_recovery_allowed(&g_opts, &g_state), "NXDN row has no P25 recovery");
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    rc |= expect(g_frequency == 453000000L, "revisit starts on learned CC");
    ctx = dmr_sm_get_ctx();
    rc |= expect(ctx->cc_acquiring && ctx->state == DMR_SM_ON_CC && !ctx->vc_freq_hz,
                 "revisit starts fresh acquisition without stale call state");
    cleanup();
    return rc;
}

static int
pending_and_failed_probes(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    int rc = 0;
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    aloha(1);
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    expire_acquisition(ctx);
    const uint64_t pending = g_request;
    const int count = g_cc_tunes;
    rc |= expect(pending != 0U && ctx->cc_tune_request_id == pending, "probe tracks pending request");
    expire_acquisition(ctx);
    aloha(1);
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 453000000L, 4, 0, 1001, 2001);
    rc |= expect(!g_opts.trunk_is_tuned, "grant cannot interrupt an unresolved CC tune");
    rc |= expect(ctx->cc_tune_request_id == pending && g_cc_tunes == count && ctx->cc_acquiring,
                 "pending tune waits within its backend deadline and rejects old-frequency control messages");
    dsd_trunk_tuning_request_publish(pending, DSD_TRUNK_TUNE_RESULT_OK);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->cc_tune_request_id == 0U && g_cc_tunes == count
                     && dsd_decode_now_mono_s() - ctx->cc_acquire_start_m < 1.0,
                 "acquisition grace begins at backend completion");
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == 452000000L, "completed probe accepts decoded CC");
    g_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    expire_acquisition(ctx);
    rc |= expect(g_frequency == 452000000L && g_state.trunk_cc_freq == 452000000L,
                 "deferred probe preserves tuner and anchor");
    const int deferred_count = g_cc_tunes;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == deferred_count, "failed/deferred probes are paced");
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    ctx->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    const uint64_t failed = g_request;
    dsd_trunk_tuning_request_publish(failed, DSD_TRUNK_TUNE_RESULT_FAILED);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_HUNTING && !ctx->cc_tune_request_id && g_state.trunk_cc_freq == 452000000L,
                 "backend failure enters paced recovery without changing anchor");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    ctx->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
                 "successful replacement clears the failed frame gate");
    cleanup();
    return rc;
}

static void
cap_plus_status(unsigned int rest, int busy) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 0x3e;
    bytes[1] = 0x10;
    bytes[2] = (uint8_t)(0xc0U | rest); // Single complete status.
    bytes[3] = busy ? 0x80 : 0;         // LSN 1 carries a group call when busy.
    bytes[4] = busy ? 42 : 0;
    for (int i = 0; i < 80; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1, 0);
}

static void
cap_plus_idle(unsigned int rest) {
    cap_plus_status(rest, 0);
}

static int
cap_plus_rest_recovery(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    int rc = 0;
    cap_plus_idle(1U);
    rc |= expect(!g_opts.trunk_is_tuned && dmr_sm_get_ctx()->cc_confirmed,
                 "idle CAP+ status confirms rest channel without claiming a followed call");
    g_state.p25_cc_freq = 451000000L; // A stale generic alias must not override the announcement.
    g_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    cap_plus_idle(2U);
    rc |= expect(g_frequency == 451000000L && g_state.trunk_cc_freq == 451000000L,
                 "failed CAP+ rest move retains the previous anchor");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    cap_plus_idle(2U);
    rc |= expect(g_frequency == 452000000L && g_state.trunk_cc_freq == 452000000L
                     && dmr_sm_get_ctx()->cc_probe_freq_hz == g_frequency && dmr_sm_get_ctx()->cc_acquiring,
                 "idle CAP+ rest announcement actually retunes and starts acquisition");
    cap_plus_idle(2U);
    rc |= expect(dmr_sm_get_ctx()->cc_confirmed && !dmr_sm_get_ctx()->cc_acquiring,
                 "received CAP+ rest status completes acquisition");
    trunk_scan_test_set_now(0.5);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    trunk_scan_test_set_now(1.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    trunk_scan_test_set_now(1.26);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_engine_trunk_scan_active_p25_ctx() != NULL, "idle CAP+ allows target rotation");
    cleanup();
    return rc;
}

static int
switch_while_probe_pending(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    expire_acquisition(dmr_sm_get_ctx());
    const uint64_t old_request = g_request;
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    dsd_trunk_tuning_request_publish(old_request, DSD_TRUNK_TUNE_RESULT_OK);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    int rc = expect(g_frequency == 451000000L && ctx->cc_probe_freq_hz == 451000000L && ctx->cc_tune_request_id == 0U
                        && ctx->cc_acquiring,
                    "late completion from a parked target cannot restore its old probe");
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1, 0);
    rc |= expect(ctx->cc_acquiring && !ctx->cc_confirmed, "arbitrary valid CSBK is not CC acquisition proof");
    g_opts.trunk_enable = 0;
    g_state.trunk_cc_freq = 0;
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == 451000000L && ctx->cc_acquiring,
                 "monitor mode retains decoded CC attribution without starting recovery");
    cleanup();
    return rc;
}

static int
standalone_recovery(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    g_opts.trunk_scan_enabled = 0;
    g_opts.trunk_enable = 1;
    g_opts.frame_dmr = g_opts.frame_p25p1 = g_opts.frame_p25p2 = 1;
    g_state.trunk_cc_freq = 451000000L;
    g_state.trunk_lcn_freq[0] = 451000000L;
    g_state.trunk_lcn_freq[1] = 452000000L;
    g_state.lcn_freq_count = 2;
    DSD_SNPRINTF(g_opts.chan_in_file, sizeof(g_opts.chan_in_file), "%s", g_channels);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_sm_init_ctx(dmr, &g_opts, &g_state);
    aloha(1);
    g_state.synctype = DSD_SYNC_NONE;
    noCarrier(&g_opts, &g_state);
    int rc = expect(dsd_trunk_dmr_recovery_allowed(&g_opts, &g_state), "standalone AUTO retains decoded DMR owner");
    if (dsd_trunk_dmr_recovery_allowed(&g_opts, &g_state)) {
        expire_acquisition(dmr);
    }
    rc |= expect(g_frequency == 452000000L, "standalone AUTO hunts after complete sync loss");

    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    g_state.p25_cc_freq = 851000000L;
    p25_sm_init_ctx(p25, &g_opts, &g_state);
    p25->state = P25_SM_HUNTING;
    const double old_try = dsd_decode_now_mono_s() - 20.0;
    p25->t_hunt_try_m = old_try;
    const int tunes = g_cc_tunes;
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(fabs(p25->t_hunt_try_m - old_try) < 1e-9 && g_cc_tunes == tunes,
                 "watchdog cannot hunt on a DMR owner even with P25 enabled and stale P25 context");

    g_state.trunk_cc_freq = g_state.p25_cc_freq = g_frequency = 851000000L;
    g_state.trunk_lcn_freq[0] = 851000000L;
    g_state.trunk_lcn_freq[1] = 852000000L;
    g_state.p25_cc_is_tdma = 0;
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_P25P1_POS;
    g_state.p25_cc_freq = 0;
    p25_sm_init_ctx(p25, &g_opts, &g_state);
    g_state.p25_cc_freq = 851000000L;
    p25_sm_note_cc_activity(&g_state); // Actual TSBK/MBT/LCCH production activity boundary.
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(p25->state == P25_SM_ON_CC && dsd_trunk_p25_recovery_allowed(&g_opts, &g_state),
                 "decoded P25 control activity starts standalone recovery before the first voice grant");
    g_opts.scanner_mode = 1;
    rc |= expect(!dsd_trunk_p25_recovery_allowed(&g_opts, &g_state), "conventional scanner excludes P25 recovery");
    g_opts.scanner_mode = 0;
    g_state.p25_last_cc_msg_time_m = old_try;
    p25->t_cc_sync_m = g_state.last_cc_sync_time_m = old_try;
    p25->t_hunt_try_m = old_try;
    g_state.synctype = DSD_SYNC_NONE;
    g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    noCarrier(&g_opts, &g_state);
    rc |= expect(g_state.p25_cc_is_tdma == 0 && dsd_trunk_p25_recovery_allowed(&g_opts, &g_state),
                 "stray non-P25 sync and noCarrier preserve the established P25 owner and format");
    const int before_dmr = g_cc_tunes;
    /* Decoder-side ticks used to bypass the engine ownership gate. Exercise
     * both the transition out of ON_CC and an already-hunting stale context. */
    expire_acquisition(dmr);
    dmr->state = DMR_SM_HUNTING;
    dmr->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == before_dmr && g_frequency == 851000000L,
                 "decoder-side DMR ticks cannot retune a P25-owned receiver");
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(p25->t_hunt_try_m > old_try && g_cc_tunes > tunes,
                 "watchdog advances P25 CC recovery after stray sync and noCarrier");
    cleanup();
    return rc;
}

static int
heartbeat_and_single_candidate(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    aloha(1);
    int rc = 0;
    const int queries = g_queries;
    g_query_failed = 1;
    ctx->t_cc_sync_m -= ctx->cc_grace_s + 1.0;
    for (int i = 0; i < 30; ++i) {
        dmr_sm_note_cc_heartbeat(&g_opts, &g_state);
    }
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(g_queries == queries && ctx->state == DMR_SM_ON_CC,
                 "established heartbeats use completed tune attribution without rigctl queries");
    dsd_trunk_tuning_generation_advance();
    ctx->t_cc_sync_m -= ctx->cc_grace_s + 1.0;
    dmr_sm_note_cc_heartbeat(&g_opts, &g_state);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_HUNTING, "heartbeat from a different tune generation cannot retain CC");

    g_query_failed = 0;
    g_state.lcn_freq_count = 1;
    g_state.trunk_cc_freq = 0;
    g_state.p25_cc_freq = 451000000L;
    dmr_sm_begin_cc_acquisition(ctx, &g_opts, &g_state, 451000000L, 0U);
    ctx->cc_acquire_start_m -= 3.0;
    dmr_sm_event(ctx, &g_opts, &g_state, &(dmr_sm_event_t){.type = DMR_SM_EV_CC_SYNC});
    rc |= expect(ctx->cc_acquiring && !ctx->cc_confirmed, "raw CC_SYNC event cannot confirm acquisition");
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_HUNTING, "acquisition deadline works without a prior CC sync timestamp");
    const int tunes = g_cc_tunes;
    for (int i = 0; i < 3; ++i) {
        dmr_sm_note_cc_heartbeat(&g_opts, &g_state);
        expire_acquisition(ctx);
    }
    rc |= expect(g_cc_tunes == tunes && ctx->cc_acquiring && !ctx->cc_confirmed,
                 "single-candidate acquisition keeps listening without repeated retunes or false confirmation");
    aloha(1);
    rc |= expect(ctx->cc_confirmed && !ctx->cc_acquiring, "decoded CC evidence completes uninterrupted acquisition");
    cleanup();
    return rc;
}

static int
watchdog_ownership_thread(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    g_opts.trunk_scan_enabled = 0;
    g_opts.trunk_enable = 1;
    g_opts.frame_dmr = g_opts.frame_p25p1 = g_opts.frame_p25p2 = 1;
    g_state.trunk_cc_freq = g_state.p25_cc_freq = 851000000L;
    g_state.trunk_lcn_freq[0] = 851000000L;
    g_state.trunk_lcn_freq[1] = 852000000L;
    g_state.lcn_freq_count = 2;
    DSD_SNPRINTF(g_opts.chan_in_file, sizeof(g_opts.chan_in_file), "%s", g_channels);
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    p25_sm_init_ctx(p25, &g_opts, &g_state);
    p25->state = P25_SM_HUNTING;
    const double old_try = dsd_decode_now_mono_s() - 20.0;
    p25->t_hunt_try_m = old_try;
    dsd_trunk_recovery_note_protocol(&g_state, DSD_TRUNK_RECOVERY_DMR);
    p25_sm_watchdog_start(&g_opts, &g_state);
    /* Acquisition owns these volatile fields outside the SM guard. TSan must
     * see no watchdog read of them while a DMR owner excludes P25 recovery. */
    for (int i = 0; i < 450; ++i) {
        g_state.synctype = (i & 1) ? DSD_SYNC_P25P1_POS : DSD_SYNC_DMR_BS_DATA_POS;
        g_state.lastsynctype = g_state.synctype;
        g_state.p25_cc_is_tdma = i & 1;
        dsd_sleep_ms(1U);
    }
    p25_sm_watchdog_stop();
    int rc =
        expect(fabs(p25->t_hunt_try_m - old_try) < 1e-9, "background watchdog excludes DMR despite changing raw hints");
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_NONE;
    g_state.p25_cc_is_tdma = 0;
    dsd_trunk_recovery_note_protocol(&g_state, DSD_TRUNK_RECOVERY_P25);
    p25_sm_watchdog_start(&g_opts, &g_state);
    dsd_sleep_ms(50U);
    p25_sm_watchdog_stop();
    rc |= expect(p25->t_hunt_try_m > old_try, "background watchdog positive control drives P25 recovery");
    cleanup();
    return rc;
}

static struct {
    dsd_mutex_t mutex;
    dsd_cond_t condition;
    int pause_flush;
    int entered;
    int resume;
    int skip_started;
    atomic_int skip_finished;
    int skip_waited;
    int skip_stalled;
    int skip_wait_baseline; /* the drain's guard waits before the Skip was queued */
    int flushes;
    int blocked;
    int allowed;
    int failures;
    int audio_blocks;
} g_skip_flush;

static void
skip_audio_sink(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    if (bytes > 0 && data) {
        ++g_skip_flush.audio_blocks;
    }
}

static void
skip_watchdog_flush(dsd_opts* opts, dsd_state* state) {
    dsd_mutex_lock(&g_skip_flush.mutex);
    g_skip_flush.entered = 1;
    dsd_cond_broadcast(&g_skip_flush.condition);
    while (g_skip_flush.pause_flush && !g_skip_flush.resume) {
        dsd_cond_wait(&g_skip_flush.condition, &g_skip_flush.mutex);
    }
    dsd_mutex_unlock(&g_skip_flush.mutex);
    int left = -1, right = -1;
    dsd_tg_policy_decision decision;
    g_skip_flush.failures |= dsd_audio_group_gate_dual(opts, state, 1234, 0, 0, 1, &left, &right);
    g_skip_flush.failures |= dsd_tg_policy_evaluate_group_call(opts, state, 1234, 42, 0, 0, &decision);
    const int blocked = (decision.block_reasons & DSD_TG_POLICY_BLOCK_CALL_SKIP) != 0;
    g_skip_flush.failures |= left != blocked || decision.audio_allowed != !blocked;
    ++g_skip_flush.flushes;
    if (blocked) {
        ++g_skip_flush.blocked;
    } else {
        ++g_skip_flush.allowed;
    }
    dsd_p25p2_flush_partial_audio(opts, state);
}

static void
release_barrier(void) {
    dsd_mutex_lock(&g_skip_flush.mutex);
    g_skip_flush.resume = 1;
    dsd_cond_broadcast(&g_skip_flush.condition);
    dsd_mutex_unlock(&g_skip_flush.mutex);
}

static DSD_THREAD_RETURN_TYPE
resume_skip_flush(void* opaque) {
    (void)opaque;
    dsd_mutex_lock(&g_skip_flush.mutex);
    while (!g_skip_flush.skip_started) {
        if (dsd_cond_timedwait(&g_skip_flush.condition, &g_skip_flush.mutex, 10000U) != 0) {
            g_skip_flush.skip_stalled = 1; /* a fixture failure: released anyway, so the test fails rather than hangs */
            break;
        }
    }
    dsd_mutex_unlock(&g_skip_flush.mutex);
    /* Until the drain counts the Skip's wait for the guard the tick holds: from then on the drain is blocked on it. */
    const double deadline = dsd_realtime_mono_s() + 10.0;
    while (!g_skip_flush.skip_stalled && dsd_app_command_test_policy_guard_waits() <= g_skip_flush.skip_wait_baseline
           && !atomic_load(&g_skip_flush.skip_finished)) {
        if (dsd_realtime_mono_s() >= deadline) {
            g_skip_flush.skip_stalled = 1;
            break;
        }
        dsd_thread_yield();
    }
    g_skip_flush.skip_waited = !atomic_load(&g_skip_flush.skip_finished);
    release_barrier();
    DSD_THREAD_RETURN;
}

static int
seed_skip_watchdog_call(void) {
    g_opts.trunk_enable = 1;
    g_opts.frame_p25p1 = g_opts.frame_p25p2 = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.floating_point = 0;
    g_opts.pulse_digi_rate_out = 8000;
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.slot1_on = g_opts.slot2_on = 1;
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    g_state.p25_cc_freq = g_state.trunk_cc_freq = 851000000;
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_state.p25_chan_tdma_explicit[1] = 2;
    g_state.trunk_chan_map[0x1234] = 852000000;
    dsd_trunk_recovery_note_protocol(&g_state, DSD_TRUNK_RECOVERY_P25);
    dsd_tg_policy_call_skip_clear(&g_state);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 852000000, 1234, 42, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    int rc =
        expect(p25_sm_emit_active_call(&g_opts, &g_state, 0, 1234, 0, 42, 1, 0), "watchdog fixture voice accepted");
    rc |= expect(ctx->state == P25_SM_TUNED && ctx->vc_is_tdma, "watchdog fixture follows TDMA");
    g_state.p25_p2_audio_allowed[0] = 1;
    /* One admitted frame waits in the P25 Phase 2 playout (issue #651). */
    for (int i = 0; i < 160; ++i) {
        g_state.s_l[i] = 1000;
    }
    g_state.mbe_short_silenced[0] = 0;
    dsd_p25p2_playout_reset(&g_state, -1);
    dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 0, 1U, NULL);
    g_skip_flush.pause_flush = g_skip_flush.entered = g_skip_flush.resume = 0;
    g_skip_flush.skip_started = g_skip_flush.skip_waited = g_skip_flush.skip_stalled = 0;
    g_skip_flush.flushes = g_skip_flush.blocked = g_skip_flush.allowed = g_skip_flush.failures =
        g_skip_flush.audio_blocks = 0;
    atomic_store(&g_skip_flush.skip_finished, 0);
    g_state.ui_msg[0] = '\0';
    return rc;
}

static int
watchdog_call_skip_phases(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    g_opts.trunk_scan_enabled = 0;
    dsd_app_frontend_runtime_start(&g_opts, &g_state);
    dsd_mutex_init(&g_skip_flush.mutex);
    dsd_cond_init(&g_skip_flush.condition);
    atomic_init(&g_skip_flush.skip_finished, 0);
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){.p25p2_flush_partial_audio = skip_watchdog_flush});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = skip_audio_sink});
    int rc = 0;
    for (int phase = 0; phase < 3; ++phase) {
        rc |= seed_skip_watchdog_call();
        if (phase == 1) {
            // A refused manual tune leaves both the call and buffered audio for the watchdog.
            g_result = DSD_TRUNK_TUNE_RESULT_FAILED;
            rc |= expect(dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, 0) > 0, "watchdog skip queued");
            rc |= expect(dsd_app_drain_cmds(&g_opts, &g_state) == 1, "watchdog skip drained");
            rc |= expect(dsd_tg_policy_call_skip_active(&g_state, 1234, dsd_decode_now_mono_s()),
                         "watchdog skip armed before release");
            rc |= expect(p25_sm_get_ctx()->state == P25_SM_TUNED && dsd_p25p2_playout_level(&g_state, 0) > 0,
                         "refused Skip preserves releasable buffered carrier");
            g_result = DSD_TRUNK_TUNE_RESULT_OK;
        }
        g_skip_flush.pause_flush = phase != 0;
        g_state.p25_sm_force_release = 1;
        /* The frontends' view before the tick: trunking on. */
        dsd_app_telemetry_publish_opts_snapshot(&g_opts);
        dsd_app_telemetry_publish_snapshot(&g_state);
        p25_sm_watchdog_start(&g_opts, &g_state);
        dsd_mutex_lock(&g_skip_flush.mutex);
        while (!g_skip_flush.entered) {
            if (dsd_cond_timedwait(&g_skip_flush.condition, &g_skip_flush.mutex, 2000) != 0) {
                break;
            }
        }
        const int entered = g_skip_flush.entered;
        dsd_mutex_unlock(&g_skip_flush.mutex);
        rc |= expect(entered, "real watchdog reached partial-audio flush");
        if (phase != 0 && entered) {
            rc |= expect(dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, 0) > 0, "in-flight trunk off queued");
            rc |= expect(dsd_app_drain_cmds(&g_opts, &g_state) == 1 && !g_opts.trunk_enable,
                         "in-flight trunk off applied");
            /* The drain does not wait out the tick that holds the guard: its publication is put off. */
            const dsd_opts* published = dsd_app_get_latest_opts_snapshot();
            rc |= expect(dsd_app_telemetry_opts_snapshot_pending() && dsd_app_telemetry_snapshot_pending() && published
                             && published->trunk_enable == 1,
                         "in-flight trunk off publication put off while the tick holds the guard");
        }
        if (phase == 2 && entered) {
            g_skip_flush.skip_wait_baseline = dsd_app_command_test_policy_guard_waits();
            dsd_thread_t releaser;
            const int started = dsd_thread_create(&releaser, resume_skip_flush, NULL) == 0;
            rc |= expect(started, "flush barrier releaser started");
            rc |= expect(dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, 0) > 0, "in-flight skip queued");
            dsd_mutex_lock(&g_skip_flush.mutex);
            g_skip_flush.skip_started = 1;
            dsd_cond_broadcast(&g_skip_flush.condition);
            dsd_mutex_unlock(&g_skip_flush.mutex);
            if (!started) {
                release_barrier();
            }
            rc |= expect(dsd_app_drain_cmds(&g_opts, &g_state) == 1, "in-flight skip drained");
            atomic_store(&g_skip_flush.skip_finished, 1);
            if (started) {
                dsd_thread_join(releaser);
                rc |= expect(g_skip_flush.skip_waited && !g_skip_flush.skip_stalled
                                 && dsd_app_command_test_policy_guard_waits() == g_skip_flush.skip_wait_baseline + 1,
                             "Skip waits for admitted tick even after trunking is disabled");
            }
        }
        release_barrier();
        p25_sm_watchdog_stop();
        if (phase != 0 && entered) {
            /* The next drain, with nothing queued, is the put-off publication's next opportunity. */
            rc |= expect(dsd_app_drain_cmds(&g_opts, &g_state) == 0, "empty drain after the tick");
            const dsd_opts* published = dsd_app_get_latest_opts_snapshot();
            rc |= expect(!dsd_app_telemetry_opts_snapshot_pending() && !dsd_app_telemetry_snapshot_pending()
                             && published && published->trunk_enable == 0,
                         "the trunk-off reaches the frontends once the tick has left the guard");
        }
        rc |= expect(g_skip_flush.flushes > 0 && !g_skip_flush.failures, "watchdog flush verdicts consistent");
        dsd_call_snapshot call;
        rc |= expect(dsd_call_state_get(&g_state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED,
                     "watchdog release ends the call");
        rc |= expect(g_state.event_history_s[0].Event_History_Items[1].target_id == 1234,
                     "watchdog release commits the call row");
        rc |= expect(g_state.event_history_s[0].Event_History_Items[0].target_id == 0
                         && g_state.event_history_s[0].Event_History_Items[0].internal_str[0] == '\0',
                     "watchdog release leaves no staged call or skip text");
        if (phase == 1) {
            rc |= expect(g_skip_flush.blocked > 0 && g_skip_flush.audio_blocks == 0,
                         "fresh skip blocks real partial audio");
            rc |= expect(
                strcmp(g_state.event_history_s[0].Event_History_Items[1].internal_str, "Target: 1234; call skipped.")
                    == 0,
                "skip before release annotates the committed call row");
        } else {
            rc |= expect(g_skip_flush.allowed > 0 && g_skip_flush.audio_blocks > 0,
                         "positive control flush plays buffered audio");
            rc |=
                expect(strstr(g_state.event_history_s[0].Event_History_Items[1].internal_str, "call skipped.") == NULL,
                       "release before skip commits no skip text");
        }
        if (phase == 2) {
            rc |= expect(!dsd_tg_policy_call_skip_active(&g_state, 1234, dsd_decode_now_mono_s()),
                         "in-flight skip cannot arm an ended call");
            rc |= expect(g_state.ui_msg[0] == '\0', "in-flight skip of an ended call emits no toast");
        }
    }
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_cond_destroy(&g_skip_flush.condition);
    dsd_mutex_destroy(&g_skip_flush.mutex);
    dsd_app_frontend_runtime_stop();
    cleanup();
    return rc;
}

static int
pending_probe_dwell(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    trunk_scan_test_set_now(1.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    trunk_scan_test_set_now(1.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    expire_acquisition(dmr_sm_get_ctx());
    const uint64_t request = g_request;
    trunk_scan_test_set_now(10.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    int rc =
        expect(dsd_engine_trunk_scan_active_dmr_ctx() != NULL, "unresolved DMR probe holds the target past idle dwell");
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    trunk_scan_test_set_now(10.1);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_engine_trunk_scan_active_dmr_ctx() != NULL, "probe completion starts a fresh idle dwell");
    trunk_scan_test_set_now(10.36);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_engine_trunk_scan_active_p25_ctx() != NULL,
                 "decoded acquisition wait does not extend idle dwell after tune completion");
    cleanup();
    return rc;
}

static int
busy_cap_plus_rest(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_idle(1U);
    g_opts.trunk_tune_group_calls = 0;
    cap_plus_status(4U, 1);
    int rc = expect(g_frequency == 453000000L && !g_opts.trunk_is_tuned && dmr_sm_get_ctx()->cc_acquiring,
                    "busy CAP+ follows an announced rest channel when no allowed call owns the tuner");
    cap_plus_status(4U, 1);
    rc |= expect(dmr_sm_get_ctx()->cc_confirmed, "busy CAP+ rest signalling confirms acquisition");
    g_opts.trunk_is_tuned = 1;
    g_frequency = 452000000L;
    cap_plus_status(1U, 1);
    rc |= expect(g_frequency == 452000000L, "busy CAP+ rest following preserves an active followed call");
    cleanup();
    return rc;
}

static int
probe_grant_and_missing_anchor(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    aloha(1);
    expire_acquisition(ctx);
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 453000000L, 4, 0, 1001, 2001);
    int rc = expect(g_opts.trunk_is_tuned && g_frequency == 453000000L && g_state.trunk_cc_freq == 452000000L,
                    "validated grant on a probe establishes its return anchor before following");
    g_state.trunk_sm_force_release = 1;
    dmr_sm_emit_release(&g_opts, &g_state, -1);
    rc |= expect(g_frequency == 452000000L && !g_opts.trunk_is_tuned,
                 "probe grant releases to the channel that carried the grant");
    aloha(1);
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 453000000L, 4, 0, 1001, 2001);
    g_state.trunk_cc_freq = 0;
    g_state.trunk_sm_force_release = 1;
    dmr_sm_emit_release(&g_opts, &g_state, -1);
    rc |= expect(g_frequency == 452000000L && !g_opts.trunk_is_tuned,
                 "release recovers a cleared shared anchor from the DMR context");
    aloha(1);
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 453000000L, 4, 0, 1001, 2001);
    test_accepted_voice_sync(&g_opts, &g_state, 0);
    dsd_call_snapshot call;
    rc |= expect(dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE,
                 "missing-anchor release starts with an active canonical call");
    g_state.trunk_cc_freq = ctx->cc_freq_hz = 0;
    g_state.p25_cc_freq = 851000000L;
    g_state.trunk_sm_force_release = 1;
    const int returns = g_returns;
    dmr_sm_emit_release(&g_opts, &g_state, -1);
    rc |= expect(g_returns == returns && !g_opts.trunk_is_tuned && ctx->state == DMR_SM_HUNTING && !ctx->vc_freq_hz
                     && !g_state.trunk_vc_freq[0] && !g_state.trunk_sm_force_release,
                 "missing DMR anchor clears the call and enters recovery without using a stale P25 alias");
    rc |= expect(dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase != DSD_CALL_PHASE_ACTIVE,
                 "missing-anchor release ends canonical call activity");
    ctx->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(g_frequency == 451000000L && ctx->cc_acquiring, "missing-anchor release can recover from the map");
    cleanup();
    return rc;
}

static int
cap_plus_idle_preserves_hangtime(void) {
    if (setup(2.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_idle(1U);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    test_accepted_voice_sync(&g_opts, &g_state, 0);
    ctx->slots[0].last_active_m -= 1.0;
    ctx->t_voice_m -= 1.0;
    cap_plus_idle(1U);
    cap_plus_idle(4U);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    int rc = expect(g_returns == 0 && g_opts.trunk_is_tuned && g_frequency == 452000000L,
                    "idle CAP+ rest announcements preserve the followed call's hangtime");
    ctx->t_voice_m -= 2.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(g_returns == 1 && !g_opts.trunk_is_tuned && g_frequency == 453000000L,
                 "CAP+ call releases to the announced rest channel when hangtime expires");
    cleanup();
    return rc;
}

static int
hunt_fade_and_avoids(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    const int tunes = g_cc_tunes;
    ctx->t_cc_sync_m = dsd_decode_now_mono_s() - 3.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    int rc =
        expect(g_cc_tunes == tunes && ctx->state == DMR_SM_ON_CC, "three-second control fade retains the confirmed CC");
    for (int i = 0; i < g_state.lcn_freq_count; ++i) {
        if (*dsd_state_trunk_lcn_slot_const(&g_state, i) == 452000000L) {
            rc |= expect(dsd_state_trunk_lcn_avoid_set(&g_state, (size_t)i, 1) == 0, "avoid first alternate");
        }
    }
    expire_acquisition(ctx);
    rc |= expect(g_frequency == 453000000L, "hunt skips avoided channel rows");
    expire_acquisition(ctx);
    rc |= expect(g_frequency == 451000000L, "hunt returns to the anchor after one probe");
    aloha(1);
    const int recovered_tunes = g_cc_tunes;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == recovered_tunes && ctx->cc_confirmed && !ctx->cc_acquiring,
                 "control recovered on the anchor ends hunting");
    for (int i = 0; i < g_state.lcn_freq_count; ++i) {
        rc |= expect(dsd_state_trunk_lcn_avoid_set(&g_state, (size_t)i, 1) == 0, "avoid remaining rows");
    }
    expire_acquisition(ctx);
    rc |= expect(g_cc_tunes == recovered_tunes, "all-avoided map does not tune an avoided fallback");
    cleanup();
    return rc;
}

static int
pending_probe_timeout(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    expire_acquisition(ctx);
    const uint64_t request = g_request;
    trunk_scan_test_set_now(9.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    ctx->cc_tune_deadline_m = dsd_decode_now_mono_s() - 1.0;
    trunk_scan_test_set_now(10.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    int rc = expect(!ctx->cc_tune_request_id && ctx->state == DMR_SM_HUNTING
                        && dsd_trunk_tuning_request_status(request, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED,
                    "unanswered probe expires and releases the scan hold");
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect(dsd_trunk_tuning_request_status(request, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED
                     && !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
                 "late success cannot revive an expired request or open the frame gate");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    trunk_scan_test_set_now(10.26);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_engine_trunk_scan_active_p25_ctx() != NULL
                     && dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
                 "scan advances after timeout and replacement tune reopens the frame gate");
    cleanup();
    return rc;
}

static int
pending_park_timeout(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    (void)dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    const uint64_t request = ctx->cc_tune_request_id;
    int rc = expect(request != 0U, "initial DMR park tracks its backend request");
    ctx->cc_tune_deadline_m = dsd_decode_now_mono_s() - 1.0;
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(!ctx->cc_tune_request_id
                     && dsd_trunk_tuning_request_status(request, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED,
                 "coordinator resolves the DMR deadline while awaiting the initial park");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    trunk_scan_test_set_now(3.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
                 "held DMR target retries an expired initial park");
    cleanup();
    return rc;
}

static int
visit_hook_defers_tuning_until_decoder_unwinds(void) {
    if (setup(2.0f)) {
        cleanup();
        return 1;
    }
    g_opts.scan_max_visit_ms = 1000;
    aloha(1);
    const int parked_tunes = g_cc_tunes;
    p25_sm_tick_guard_enter();
    trunk_scan_test_set_now(0.98);
    int rc = expect(!dsd_frame_sync_hook_scan_visit_should_yield(&g_opts, &g_state),
                    "held DMR visit does not interrupt the decoder");
    p25_sm_tick_guard_leave();
    rc |= expect(dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 0,
                 "release visit hold");
    p25_sm_tick_guard_enter();
    trunk_scan_test_set_now(1.01);
    rc |= expect(!dsd_frame_sync_hook_scan_visit_should_yield(&g_opts, &g_state), "DMR loop re-arms visit while held");
    trunk_scan_test_set_now(1.981);
    rc |= expect(dsd_frame_sync_hook_scan_visit_should_yield(&g_opts, &g_state),
                 "continuous DMR yields after fresh visit limit");
    rc |= expect(g_cc_tunes == parked_tunes && dsd_engine_trunk_scan_active_dmr_ctx() != NULL,
                 "deadline hook does not retune under the decoder");
    p25_sm_tick_guard_leave();
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(dsd_engine_trunk_scan_active_p25_ctx() != NULL && g_cc_tunes > parked_tunes,
                 "coordinator advances after the decoder releases its guard");

    /* The same installed hook also services conventional -Y visits. */
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    g_opts.trunk_scan_enabled = 0;
    g_opts.trunk_enable = 0;
    g_opts.scanner_mode = 1;
    g_opts.scan_max_visit_ms = 1000;
    g_state.lcn_scan_hold = 0;
    g_state.lcn_freq_count = 2;
    g_state.trunk_lcn_freq[0] = 451000000L;
    g_state.trunk_lcn_freq[1] = 452000000L;
    dsd_scan_voice_gate_note_retune(&g_state, dsd_decode_now_mono_s() - 2.0);
    p25_sm_tick_guard_enter();
    rc |= expect(dsd_frame_sync_hook_scan_visit_should_yield(&g_opts, &g_state),
                 "continuous conventional DMR yields on an expired -Y visit");
    g_opts.scan_max_visit_ms = 0;
    rc |= expect(!dsd_frame_sync_hook_scan_visit_should_yield(&g_opts, &g_state),
                 "disabled -Y cap leaves continuous decoding alone");
    p25_sm_tick_guard_leave();
    cleanup();
    return rc;
}

/* Issue #651: a Skip or a lockout the tuner accepts ends the calls before the P25 Phase 2 playout drains, in the
   no-carrier pass of the return to the control channel. What the playout queued for the blocked call stays muted
   then: the block's verdict is taken while the call is still active. */
static int
user_block_mutes_queued_voice(void) {
    int rc = 0;
    for (int lockout = 0; lockout < 2; lockout++) {
        if (setup(0.0f)) {
            cleanup();
            return 1;
        }
        dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
        g_opts.trunk_scan_enabled = 0;
        dsd_app_frontend_runtime_start(&g_opts, &g_state);
        // The tuner accepts the return to the control channel.
        dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
            .tune_to_freq_request = vc_tune, .tune_to_cc_request = cc_tune, .return_to_cc_request = return_to_cc});
        dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = skip_audio_sink});
        rc |= seed_skip_watchdog_call();
        rc |= expect(dsd_p25p2_playout_level(&g_state, 0) == 1, "user block fixture queues a frame");
        // A Phase 1 control channel the return tunes to through the trunk tuning hook.
        g_state.p25_cc_is_tdma = 0;
        g_result = DSD_TRUNK_TUNE_RESULT_OK;
        rc |= expect(dsd_app_command_set_u8(lockout ? DSD_APP_CMD_LOCKOUT_SLOT : DSD_APP_CMD_SKIP_SLOT, 0) > 0,
                     "user block queued");
        rc |= expect(dsd_app_drain_cmds(&g_opts, &g_state) == 1, "user block drained");
        rc |= expect(g_skip_flush.audio_blocks == 0, lockout ? "lockout keeps the locked-out call's queued frame silent"
                                                             : "skip keeps the skipped call's queued frame silent");
        dsd_call_snapshot call;
        rc |= expect(dsd_call_state_get(&g_state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED,
                     "user block ends the call");
        rc |= expect(dsd_p25p2_playout_level(&g_state, 0) == 0, "user block consumes the muted frame");
        dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
        dsd_app_frontend_runtime_stop();
        cleanup();
    }
    return rc;
}

/* ---- Startup control-channel hunt (issue #703) ----
 * A single-system session whose saved control channel is dead at start: the harness's trunk scan is shut down and the
 * map loaded as a -C channel map. Tune counts are compared against the baseline left by setup()'s own scan start. */

static void
single_system_cold_start(int ft) {
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    g_opts.trunk_scan_enabled = 0;
    g_opts.trunk_enable = 1;
    /* -fs (DMR alone) or -ft (P25 + DMR), named explicitly: the default set is Auto, whose NXDN and ProVoice leave a
       startup hunt to validated content. */
    g_opts.frame_dstar = g_opts.frame_x2tdma = g_opts.frame_ysf = g_opts.frame_m17 = 0;
    g_opts.frame_nxdn48 = g_opts.frame_nxdn96 = g_opts.frame_dpmr = g_opts.frame_provoice = 0;
    g_opts.frame_dmr = 1;
    g_opts.frame_p25p1 = g_opts.frame_p25p2 = ft ? 1 : 0;
    g_state.trunk_cc_freq = g_state.p25_cc_freq = 0;
    g_state.trunk_recovery_protocol = DSD_TRUNK_RECOVERY_UNKNOWN;
    g_state.trunk_lcn_freq[0] = 451000000L;
    g_state.trunk_lcn_freq[1] = 452000000L;
    g_state.trunk_lcn_freq[2] = 452000000L;
    g_state.trunk_lcn_freq[3] = 453000000L;
    g_state.lcn_freq_count = 4;
    DSD_SNPRINTF(g_opts.chan_in_file, sizeof(g_opts.chan_in_file), "%s", g_channels);
    g_frequency = 450000000L; // the dead start frequency
    g_opts.rtlsdr_center_freq = 450000000U;
    dsd_trunk_tuning_requests_reset();
    dmr_sm_init_ctx(dmr_sm_get_ctx(), &g_opts, &g_state);
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);
}

/* Arm the DMR startup window and run it out: the first probe goes to row 0. */
static void
dmr_cold_start_first_probe(dmr_sm_ctx_t* dmr) {
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
}

static int
dmr_cold_start_hunts_the_map(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    const int base = g_cc_tunes;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    int rc = expect(dmr->state == DMR_SM_IDLE, "dead start: DMR starts idle");
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == base && dmr->cold_start_armed_m > 0.0, "the first tick only arms the startup window");
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == base + 1 && g_frequency == 451000000L, "the expired window probes the first map row");
    rc |= expect(dmr->cold_start_hunt == 1 && g_state.trunk_cc_freq == 0, "a startup probe is no control channel");
    expire_acquisition(dmr);
    rc |= expect(g_frequency == 452000000L, "the startup hunt walks the map in file order");
    expire_acquisition(dmr);
    rc |= expect(g_frequency == 453000000L, "the startup hunt skips an adjacent duplicate row");
    aloha(1);
    rc |= expect(g_state.trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR && g_state.trunk_cc_freq == 453000000L,
                 "validated control on a probe names it the control channel");
    rc |= expect(dmr->cold_start_hunt == 0 && dmr->cc_confirmed == 1, "validated control ends the startup hunt");
    cleanup();
    return rc;
}

static int
dmr_live_cc_at_start_never_hunts(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    const int base = g_cc_tunes;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    aloha(1); // the saved control channel is live
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(g_cc_tunes == base && dmr->cc_confirmed && g_state.trunk_cc_freq == 450000000L,
                    "a live control channel at start is never hunted away from");
    cleanup();
    return rc;
}

static int
dmr_cold_start_holds_validated_content(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    const int base = g_cc_tunes;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s() - 1.0; // a call that passed the colour-code gate
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(g_cc_tunes == base && dmr->cold_start_hunt == 0, "validated DMR content holds the startup window");
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == base + 1, "the window runs out once the content is older than the grace");
    cleanup();
    return rc;
}

static int
dmr_stop_with_probe_in_flight(void) {
    int rc = 0;
    for (int fail = 0; fail <= 1; ++fail) {
        if (setup(0.0f)) {
            cleanup();
            return 1;
        }
        single_system_cold_start(0);
        g_hw_model = 1;
        dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
        g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        const uint64_t probe = g_request;
        const int tunes = g_cc_tunes;
        g_opts.chan_in_file[0] = '\0'; // eligibility lost with the probe in flight
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        rc |=
            expect(dmr->cold_start_hunt == 1 && g_cc_tunes == tunes, "a probe in flight is waited for, never replaced");
        g_result = DSD_TRUNK_TUNE_RESULT_OK;
        if (!fail) {
            g_frequency = 451000000L;
            dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_OK);
            dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
            rc |= expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_hunt == 0 && g_cc_tunes == tunes,
                         "a successful probe lets the hunt park");
        } else {
            dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_FAILED); // the radio stayed at 450 MHz
            dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
            rc |= expect(g_cc_tunes == tunes + 1 && g_frequency == (long)g_opts.rtlsdr_center_freq,
                         "a settle tune makes the radio and the options agree");
            dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
            rc |= expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_hunt == 0, "the hunt parks once settled");
        }
        rc |= expect(dsd_trunk_tuning_pending_request() == 0U, "no tune request is left gating frames");
        rc |= expect(g_state.trunk_cc_freq == 0, "stopping establishes no control channel");
        cleanup();
    }
    return rc;
}

static int
dmr_stop_after_deadline_then_late_success(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    g_hw_model = 1;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    const uint64_t late = g_request;
    dmr->cc_tune_deadline_m = dsd_decode_now_mono_s() - 0.1; // the backend never answered in time
    g_opts.chan_in_file[0] = '\0';
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state); // the deadline publishes FAILED, as today; a settle tune follows
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(g_cc_tunes == tunes + 1 && dmr->state == DMR_SM_IDLE && dsd_trunk_tuning_pending_request() == 0U,
                    "a missed deadline is settled before the park");
    dsd_trunk_tuning_request_publish(late, DSD_TRUNK_TUNE_RESULT_OK); // ignored, as today
    g_opts.trunk_enable = 0;
    g_state.trunk_cc_freq = 0;
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == g_frequency, "later control is attributed to the tuner, never a probe");
    cleanup();
    return rc;
}

/* A one-row map whose only probe failed: the row is probed again, since only a success retires the failure that gates
   every frame. */
static int
dmr_one_row_map_retries_a_failed_probe(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    g_hw_model = 1;
    g_state.trunk_lcn_freq[1] = g_state.trunk_lcn_freq[2] = g_state.trunk_lcn_freq[3] = 0;
    g_state.lcn_freq_count = 1;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr_cold_start_first_probe(dmr);
    const uint64_t probe = g_request;
    const int tunes = g_cc_tunes;
    dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_FAILED); // the radio stayed at 450 MHz
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dmr->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(g_cc_tunes == tunes + 1 && g_frequency == 451000000L, "the only row is probed again");
    rc |= expect(dsd_trunk_tuning_pending_request() == 0U, "its success retires the failure");
    expire_acquisition(dmr);
    rc |= expect(g_cc_tunes == tunes + 1, "a row the radio is on is listened to, never retuned");
    cleanup();
    return rc;
}

/* A grant followed during a startup hunt: a trunked channel cycle (an operator move, which drops the probe's
   attribution) staged the shared alias, as its control-channel tune does, so a Connect Plus grant resolves and is
   followed without confirming a control channel. The owner it names ends the startup bookkeeping, never the call. */
static int
dmr_cold_start_followed_grant_stands(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_cold_start_first_probe(dmr);
    g_state.trunk_cc_freq = 452000000L; // the cycle's tune to the next row
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 453000000L, 4, 0, 1001, 2001);
    int rc = expect(dmr->state == DMR_SM_TUNED && g_opts.trunk_is_tuned == 1, "the grant is followed");
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_TUNED && g_opts.trunk_is_tuned == 1 && dmr->vc_freq_hz == 453000000L,
                 "the followed call stands");
    rc |= expect(dmr->cold_start_hunt == 0 && dmr->cold_start_park_pending == 0, "the startup hunt is over");
    cleanup();
    return rc;
}

/* A grant followed while the startup window is still open (a trunked channel cycle staged the alias, so it resolves):
   the machine left IDLE, which keeps no startup window. */
static int
dmr_grant_from_idle_disarms_the_window(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_armed_m > 0.0, "the startup window is armed");
    g_state.trunk_cc_freq = 452000000L; // the cycle's tune to the next row
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 453000000L, 4, 0, 1001, 2001);
    rc |= expect(dmr->state == DMR_SM_TUNED, "the grant is followed from IDLE");
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_TUNED && !dmr_sm_cold_start_active(dmr), "a machine out of IDLE keeps no window");
    cleanup();
    return rc;
}

/* A failure followed by a tune in flight the hunt does not track (an operator's): that tune's outcome is waited for,
   never replaced, and its success retires the failure. */
static int
dmr_settle_waits_for_a_tune_in_flight(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    g_hw_model = 1;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr_cold_start_first_probe(dmr);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state); // the probe's failure resolved
    const uint64_t operator_tune = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_mark_ready(operator_tune);
    g_opts.chan_in_file[0] = '\0';
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(g_cc_tunes == tunes && dmr->cold_start_hunt == 1, "a tune in flight is waited for, never replaced");
    dsd_trunk_tuning_request_publish(operator_tune, DSD_TRUNK_TUNE_RESULT_OK);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes && dmr->state == DMR_SM_IDLE && dsd_trunk_tuning_pending_request() == 0U,
                 "its success settles the hunt, which parks");
    cleanup();
    return rc;
}

/* A settle tune the backend never answers is given up at the probe deadline, as a probe is, and replaced: the hunt
   still parks, with nothing left gating frames. */
static int
dmr_settle_tune_deadline(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    g_hw_model = 1;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr_cold_start_first_probe(dmr);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    g_opts.chan_in_file[0] = '\0'; // eligibility lost after the failure
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(g_cc_tunes == tunes + 1 && dmr->cold_start_hunt == 1, "a settle tune replaces the failure");
    dmr->cc_tune_deadline_m = dsd_decode_now_mono_s() - 0.1; // the backend never answers it
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes + 2, "an unanswered settle tune is replaced at its deadline");
    rc |= expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_hunt == 0 && dsd_trunk_tuning_pending_request() == 0U,
                 "the hunt parks with nothing left gating frames");
    cleanup();
    return rc;
}

static int
dmr_stop_trunking_off_parks_at_once(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_cold_start_first_probe(dmr);
    g_opts.trunk_enable = 0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_hunt == 0 && dmr->cold_start_armed_m <= 0.0,
                    "turning trunking off parks a startup hunt at once");
    g_opts.trunk_enable = 1;
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes && dmr->cold_start_armed_m > 0.0, "turning it back on arms a fresh window");
    cleanup();
    return rc;
}

static int
dmr_operator_moves(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_cold_start_first_probe(dmr);
    const int tunes = g_cc_tunes;
    const double moved_m = dsd_decode_now_mono_s();
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_hunt == 0 && dmr->cold_start_armed_m >= moved_m,
                    "a settled operator move parks keeping a window from the move");
    rc |= expect(g_cc_tunes == tunes, "the move is not overridden");
    cleanup();

    // The probe's request completes in the backend before the decoder consumes it, then the operator moves.
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    g_hw_model = 1;
    dmr = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_OK);
    g_frequency = 454000000L; // the operator's tune lands after the probe
    g_opts.rtlsdr_center_freq = 454000000U;
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    aloha(1); // control decoded on the operator's frequency before the next DMR tick
    rc |= expect(g_state.trunk_cc_freq != 451000000L && !(dmr->cc_confirmed && dmr->cc_freq_hz == 451000000L),
                 "control after an operator move is never credited to the completed probe");
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->cc_rx_freq_hz != 451000000L, "the probe's attribution is dropped");
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == 454000000L && dmr->cc_confirmed,
                 "control on the operator's frequency confirms it, never the probe");
    cleanup();
    return rc;
}

static int
ft_cold_start_hands_dmr_over(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(1);
    const int base = g_cc_tunes;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    p25_sm_tick_guard_enter();
    p25_sm_cold_start_tick_locked(p25, &g_opts, &g_state);
    p25->t_cold_start_armed_m = dsd_decode_now_mono_s() - p25->config.cc_grace_s - 1.0;
    p25_sm_cold_start_tick_locked(p25, &g_opts, &g_state);
    p25_sm_tick_guard_leave();
    int rc = expect(g_cc_tunes == base + 1 && g_frequency == 451000000L, "-ft: P25 drives the startup hunt");
    rc |= expect(g_state.trunk_cc_freq == 0 && g_state.p25_cc_freq == 0, "the probe publishes no control channel");
    aloha(1); // the probed frequency is a DMR control channel
    rc |= expect(g_state.trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR && dmr->cc_confirmed
                     && g_state.trunk_cc_freq == 451000000L,
                 "validated DMR control on a P25-driven probe hands the trunk to DMR");
    p25_sm_tick_guard_enter();
    p25_sm_cold_start_tick_locked(p25, &g_opts, &g_state);
    p25_sm_tick_guard_leave();
    rc |= expect(p25->state == P25_SM_IDLE && p25->cold_start_hunt == 0, "P25 ends its startup hunt where it is");
    const int tunes = g_cc_tunes;
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes, "P25 does not retune a DMR-owned receiver");
    cleanup();
    return rc;
}

/* ---- Recovery that does not stall where no control channel is (Cap+ "won't move on without a rest channel") ---- */

/* A CRC-valid CSBK with no control-channel proof (opcode 0x3D, a data preamble) names DMR the owner. */
static void
dmr_preamble(void) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 0x3d;
    for (int i = 0; i < 80; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1, 0);
}

/* A CRC-valid Hytera XPT CSBK (FID 0x68) of @p opcode: 0x0A site status, 0x0B adjacent sites. */
static void
xpt_csbk(uint8_t opcode) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = opcode;
    bytes[1] = 0x68;
    for (int i = 0; i < 80; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1, 0);
}

/* A Capacity Plus status whose bank one shows LSN 1 (@p tg1) and LSN 2 (TG 43) busy, rest on @p rest. */
static void
cap_plus_calls(unsigned int rest, uint8_t tg1) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 0x3e;
    bytes[1] = 0x10;
    bytes[2] = (uint8_t)(0xc0U | rest);
    bytes[3] = 0xc0; // LSNs 1 and 2
    bytes[4] = tg1;
    bytes[5] = 43;
    for (int i = 0; i < 80; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1, 0);
}

static void
cap_plus_two_calls(unsigned int rest) {
    cap_plus_calls(rest, 42U);
}

/* The LSN map of the harness's channels.csv, as an RR import doubles each repeater's rows. */
static void
cap_plus_map(void) {
    g_state.trunk_chan_map[1] = 451000000L;
    g_state.trunk_chan_map[2] = 452000000L;
    g_state.trunk_chan_map[3] = 452000000L;
    g_state.trunk_chan_map[4] = 453000000L;
}

static int
close_to(double a, double b) {
    return fabs(a - b) < 1e-6;
}

/* The report: validated DMR content named the protocol without confirming a control channel (a preamble on a rest
   channel Cap+ moved away from), then the frequency went quiet. The hunt holds while that content runs, then goes on
   down the map; it never stays there for good. */
static int
dmr_owner_named_without_confirmation_keeps_hunting(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_cold_start_first_probe(dmr);
    const int tunes = g_cc_tunes;
    dmr_preamble();
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s(); // the burst passed the colour-code gate
    int rc = expect(g_state.trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR && !dmr->cc_confirmed,
                    "a valid CSBK names DMR without confirming a control channel");
    expire_acquisition(dmr);
    rc |= expect(g_cc_tunes == tunes, "validated content holds the frequency");
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes + 1 && g_frequency == 452000000L,
                 "once quiet, the hunt goes on to the next row instead of revisiting the unconfirmed frequency");
    rc |= expect(dsd_frame_sync_hook_decoder_ticks_due(&g_opts, &g_state), "the hunt is a decoder timer");
    cleanup();
    return rc;
}

/* A Hytera XPT site has no control channel: once identified it parks in place, by CSBK or by SLCO alone (an SLCO names
   DMR the owner too), and its identity is the system's. */
static int
dmr_xpt_site_stays_in_place(void) {
    int rc = 0;
    for (int opcode = 0x0A; opcode <= 0x0B; ++opcode) {
        if (setup(0.0f)) {
            cleanup();
            return 1;
        }
        single_system_cold_start(0);
        dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
        dmr_cold_start_first_probe(dmr);
        g_opts.trunk_is_tuned = 0;
        xpt_csbk((uint8_t)opcode);
        g_opts.trunk_is_tuned = 0; // the site status marks the carrier tuned, which would mask the exemption
        const int tunes = g_cc_tunes;
        rc |= expect(g_state.trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR && dmr->site_without_cc,
                     "a valid XPT CSBK identifies the site");
        expire_acquisition(dmr);
        rc |= expect(dmr->state == DMR_SM_IDLE && dmr->cold_start_hunt == 0 && g_cc_tunes == tunes,
                     "an XPT site ends the startup hunt where it is");
        dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        rc |= expect(g_cc_tunes == tunes, "an XPT site is not hunted again");
        dmr_sm_begin_cc_acquisition(dmr, &g_opts, &g_state, 451000000L, 0U);
        rc |= expect(dmr->site_without_cc, "a call's return on the same system keeps the identity");
        dmr_sm_note_system_replaced(dmr);
        rc |= expect(!dmr->site_without_cc, "another system forgets it");
        cleanup();
    }
    return rc;
}

/* With DMR named and a probe in flight, losing eligibility (the map cleared) still settles the probe. */
static int
dmr_named_owner_settles_a_probe_in_flight(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    g_hw_model = 1;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr->cold_start_armed_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    const uint64_t probe = g_request;
    dmr_preamble();
    g_opts.chan_in_file[0] = '\0';
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(dmr->cc_tune_request_id == probe && g_cc_tunes == tunes, "the probe in flight is waited for");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_FAILED);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes + 1, "its failure is replaced by a settle tune");
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dsd_trunk_tuning_pending_request() == 0U && dmr->cold_start_hunt == 0,
                 "the hunt parks once settled, with nothing gating frames");
    cleanup();
    return rc;
}

/* A call followed from a machine that never confirmed control, then given up at a carrier boundary: ON_CC has a window
   that runs out. After a long call on a confirmed one, the window is a fresh one from the move. */
static int
dmr_abandoned_carrier_gets_a_window(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_state.trunk_recovery_protocol = DSD_TRUNK_RECOVERY_DMR;
    g_state.trunk_cc_freq = 451000000L;
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    int rc = expect(dmr->state == DMR_SM_TUNED && dmr->t_cc_sync_m <= 0.0, "a grant followed without a CC stamp");
    dmr_sm_abandon_carrier(dmr, &g_opts, &g_state, "carrier-boundary");
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_ON_CC && dmr->t_cc_sync_m > 0.0, "an unstamped ON_CC gets a window");
    dmr->t_cc_sync_m -= dmr->cc_grace_s + 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes + 1, "and the window runs out into the hunt");
    cleanup();

    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    dmr = dmr_sm_get_ctx();
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    dmr->t_cc_sync_m -= 30.0; // a long call
    g_opts.trunk_scan_enabled = 0;
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state); // an operator move gives the call up
    const int returns = g_returns;
    const int cc_tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_ON_CC && g_returns == returns && g_cc_tunes == cc_tunes,
                 "the move gives the new frequency a fresh window, never an expired one");
    cleanup();
    return rc;
}

/* Every DMR window holds on validated content and only on it: a call on the parked control/rest frequency is not left
   after six seconds without control, nor while content keeps arriving just after the window ran out. */
static int
dmr_windows_hold_on_validated_content(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    const int tunes = g_cc_tunes;
    const double now = dsd_decode_now_mono_s();
    dmr->t_cc_sync_m = now - dmr->cc_grace_s - 1.0;
    g_state.dmr_confidence_lock_m = now - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(dmr->state == DMR_SM_ON_CC && g_cc_tunes == tunes, "validated content holds a confirmed CC");
    g_state.dmr_confidence_lock_m = now - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_HUNTING && dmr->cc_window_lost, "the window runs out once content stops");
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s(); // content arrives before the hunt step
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_ON_CC && g_cc_tunes == tunes, "content before the hunt step reopens the window");
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s() - 3.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_ON_CC && g_cc_tunes == tunes, "for the full grace, not a probe's two seconds");
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes + 1, "then the hunt probes");
    cleanup();
    return rc;
}

/* An operator move from a confirmed rest channel A to B: nothing heard on B is credited to A, a status there naming A
   returns to A, and B gets a full window that ends in the ordinary hunt. */
static int
dmr_operator_move_drops_attribution(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(1U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    int rc = expect(dmr->cc_confirmed && dmr->cc_freq_hz == 451000000L, "rest channel A confirmed");
    g_opts.trunk_scan_enabled = 0;
    g_frequency = 453000000L; // the operator's tune
    g_opts.rtlsdr_center_freq = 453000000U;
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    const double window = dmr->t_cc_sync_m;
    dmr_preamble(); // a CSBK heard on B
    rc |= expect(close_to(dmr->t_cc_sync_m, window), "a CSBK on B never refreshes A");
    const int returns = g_returns;
    cap_plus_idle(1U); // a status on B naming rest A
    rc |= expect(!(dmr->cc_rx_freq_hz == 451000000L && g_frequency == 453000000L), "A is never confirmed from B");
    rc |= expect(g_returns == returns + 1 && g_frequency == 451000000L, "the status takes the receiver back to A");
    cleanup();
    return rc;
}

/* A probe on B hears a status naming rest A, which the shared alias already names: the receiver goes to A at once. */
static int
dmr_status_naming_the_alias_returns_at_once(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(1U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    expire_acquisition(dmr);
    int rc = expect(dmr->cc_acquiring && g_frequency == 452000000L && g_state.trunk_cc_freq == 451000000L,
                    "probing B while the alias still names A");
    const int returns = g_returns;
    cap_plus_idle(1U);
    rc |= expect(g_returns == returns + 1 && g_frequency == 451000000L, "the status naming A returns there at once");
    cleanup();
    return rc;
}

/* A decode instant far from any platform clock reading (1e9 s). A double resolves about 1.2e-7 s there, so a case on
   it moves the clock in whole milliseconds. */
#define DMR_CC_RECOVERY_TEST_DECODE_T0_NS 1000000000000000000ULL

static void
decode_clock_advance_ms(uint64_t ms) {
    dsd_decode_clock_test_set_ns(dsd_decode_now_mono_ns() + (ms * 1000000ULL));
}

/* A case that orders the stamps of events it sends back to back runs on the decode clock's TEST source and moves it on
   between them: on the system clock the events can land in one reading of a coarse clock (issue #747). */
static int
run_on_test_decode_clock(int (*test)(void)) {
    dsd_decode_clock_use_test(DMR_CC_RECOVERY_TEST_DECODE_T0_NS);
    const int rc = test();
    dsd_decode_clock_use_system();
    return rc;
}

/* The machine owns a call a Capacity Plus status followed: repeated statuses neither retune it nor push its deadlines
   back, a voice sync alone (rejected bursts, or evidence from the carrier it was granted on) never holds it, and
   accepted voice holds it until a hangtime after the newest of either slot's. */
static int
dmr_status_follow_is_owned(void) {
    if (setup(2.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(3U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_state.last_vc_sync_time = 0;
    g_state.dmr_confidence_voice_open[0] = 1;
    g_state.dmr_confidence_voice_m[0] = dsd_decode_now_mono_s(); // the rest channel's own voice, before the follow
    decode_clock_advance_ms(10U);
    cap_plus_two_calls(3U);
    int rc = expect(dmr->state == DMR_SM_TUNED && dmr->vc_freq_hz == 451000000L && g_opts.trunk_is_tuned,
                    "the status follow is the machine's call");
    const double tuned_m = dmr->t_tune_m;
    for (int i = 0; i < 3; ++i) {
        decode_clock_advance_ms(10U); // a status that restamped the deadline would move it
        g_state.last_vc_sync_time = 0;
        cap_plus_two_calls(3U);
    }
    rc |= expect(dmr->vc_freq_hz == 451000000L && g_frequency == 451000000L && close_to(dmr->t_tune_m, tuned_m),
                 "repeated statuses keep the followed carrier and its deadline");
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0); // evidence from before the follow tune
    rc |= expect(dmr->t_voice_m <= 0.0, "voice evidence from before the follow holds nothing");
    const int returns = g_returns;
    dmr->t_tune_m -= dmr->grant_timeout_s + 0.5;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_returns == returns + 1 && g_frequency == 452000000L, "without accepted voice the grant times out");
    cleanup();

    if (setup(2.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(3U);
    dmr = dmr_sm_get_ctx();
    g_state.last_vc_sync_time = 0;
    cap_plus_two_calls(3U);
    dmr->t_tune_m -= 1.0; // the follow tune, a second ago
    const double now = dsd_decode_now_mono_s();
    g_state.dmr_confidence_voice_open[0] = g_state.dmr_confidence_voice_open[1] = 1;
    g_state.dmr_confidence_voice_m[1] = now - 0.6;
    g_state.dmr_confidence_voice_m[0] = now - 0.1;
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0);
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 1);
    rc |= expect(close_to(dmr->t_voice_m, now - 0.1), "the call timer is the newest accepted voice");
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 1);
    rc |= expect(close_to(dmr->t_voice_m, now - 0.1), "an older slot's evidence never moves it back");
    // Accepted voice stops; sync-only bursts and accepted data carry on.
    g_state.dmr_confidence_voice_m[0] -= 1.5;
    g_state.dmr_confidence_voice_m[1] -= 1.5;
    dmr->t_voice_m -= 2.5;
    dmr->slots[0].last_active_m -= 2.5;
    dmr->slots[1].last_active_m -= 2.5;
    g_state.dmr_confidence_lock_m = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0);
    dmr_sm_emit_data_sync(&g_opts, &g_state, 1);
    const int returns2 = g_returns;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_returns == returns2 + 1 && g_frequency == 452000000L,
                 "the call releases a hangtime after its last accepted voice, to the announced rest");
    cleanup();
    return rc;
}

/* An identified XPT site whose return fails, or is never answered, retunes only where it parks, until frames flow. */
static int
dmr_xpt_retries_its_own_frequency(void) {
    int rc = 0;
    for (int timeout = 0; timeout <= 1; ++timeout) {
        if (setup(0.0f)) {
            cleanup();
            return 1;
        }
        single_system_cold_start(0);
        g_hw_model = 1;
        dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
        dmr_preamble();
        xpt_csbk(0x0B);
        g_opts.trunk_is_tuned = 0;
        g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        uint64_t request = 0U;
        g_state.trunk_cc_freq = 452000000L;
        (void)dsd_trunk_tuning_hook_tune_to_cc(&g_opts, &g_state, 452000000L, 0, &request);
        dmr_sm_begin_cc_acquisition(dmr, &g_opts, &g_state, 452000000L, request);
        g_result = DSD_TRUNK_TUNE_RESULT_OK;
        if (timeout) {
            dmr->cc_tune_deadline_m = dsd_decode_now_mono_s() - 0.1;
        } else {
            dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
        }
        const int tunes = g_cc_tunes;
        for (int i = 0; i < 4; ++i) {
            dmr->cc_retry_after_m = 0.0;
            dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        }
        rc |= expect(g_cc_tunes == tunes + 1 && g_frequency == 452000000L,
                     timeout ? "an unanswered XPT return is retried where it parks"
                             : "a failed XPT return is retried where it parks");
        rc |= expect(dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()), "and frames flow again");
        dmr->t_cc_sync_m = dmr->cc_acquire_start_m = dsd_decode_now_mono_s() - 30.0;
        for (int i = 0; i < 4; ++i) {
            dmr->cc_retry_after_m = 0.0;
            dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        }
        rc |= expect(g_cc_tunes == tunes + 1, "a quiet XPT site never walks the map");
        cleanup();
    }
    return rc;
}

/* An operator move while a probe is in flight: its failure is settled where the receiver now is, and the new
   frequency's window starts when frames flow, never before the move. */
static int
dmr_operator_move_settles_a_superseded_probe(void) {
    int rc = 0;
    for (int done_before = 0; done_before <= 1; ++done_before) {
        if (setup(0.0f)) {
            cleanup();
            return 1;
        }
        aloha(1);
        g_opts.trunk_scan_enabled = 0;
        g_hw_model = 1;
        dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
        g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        dmr->t_cc_sync_m -= dmr->cc_grace_s + 1.0;
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        const uint64_t probe = g_request;
        g_result = DSD_TRUNK_TUNE_RESULT_OK;
        if (done_before) {
            dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_OK);
        }
        decode_clock_advance_ms(10U); // so a window started at the completion reads as before the move
        g_frequency = 453000000L;     // the operator's tune
        g_opts.rtlsdr_center_freq = 453000000U;
        dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
        const double moved_m = dmr->cc_moved_m;
        const int tunes = g_cc_tunes;
        decode_clock_advance_ms(10U);
        if (!done_before) {
            dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_FAILED);
            dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
            rc |= expect(g_cc_tunes == tunes + 1 && g_frequency == 453000000L,
                         "the superseded probe's failure is settled where the receiver is");
            dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_OK);
        }
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        rc |= expect(dmr->state == DMR_SM_ON_CC && !dmr->cc_tune_superseded && dmr->t_cc_sync_m >= moved_m
                         && dsd_trunk_tuning_pending_request() == 0U,
                     done_before ? "a completion from before the move never backdates the window"
                                 : "the window starts once the settle released frames");
        cleanup();
    }
    return rc;
}

/* A call followed after an operator move but before the machine's next tick (the synced-frame loop dispatches a frame
   first) stands: the move's settle never overwrites it, and its own deadline returns it. A channel-map import, which
   may load another system, forgets that the old one was an XPT site. */
static int
dmr_follow_after_a_move_stands(void) {
    int rc = 0;
    for (int status = 0; status <= 1; ++status) {
        if (setup(0.0f)) {
            cleanup();
            return 1;
        }
        cap_plus_map();
        aloha(1);
        dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
        g_opts.trunk_scan_enabled = 0;
        g_frequency = 453000000L;
        dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
        if (status) {
            g_state.last_vc_sync_time = 0;
            cap_plus_two_calls(3U);
        } else {
            dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
        }
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        rc |= expect(dmr->state == DMR_SM_TUNED && g_opts.trunk_is_tuned && !dmr->cc_tune_superseded,
                     status ? "a status follow after a move stands" : "a grant followed after a move stands");
        const int returns = g_returns;
        dmr->t_tune_m -= dmr->grant_timeout_s + 0.5;
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        rc |= expect(g_returns == returns + 1 && !g_opts.trunk_is_tuned, "and its own deadline returns it");
        cleanup();
    }

    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    dmr->site_without_cc = 1;
    dsd_app_frontend_runtime_start(&g_opts, &g_state);
    rc |= expect(dsd_app_command_set_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, g_channels) > 0, "map import queued");
    rc |= expect(dsd_app_drain_cmds(&g_opts, &g_state) == 1, "map import drained");
    rc |= expect(!dmr->site_without_cc, "a channel-map import forgets the XPT identity");
    dsd_app_frontend_runtime_stop();
    cleanup();
    return rc;
}

/* A later status naming another group on the carrier already followed: no retune, but the slot is named anew. An
   operator move whose settle had to wait for a tune nothing here tracks: the window starts when frames flowed. */
static int
dmr_followed_carrier_and_untracked_settle(void) {
    if (setup(2.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(3U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_state.last_vc_sync_time = 0;
    cap_plus_two_calls(3U);
    int rc = expect(dmr->state == DMR_SM_TUNED && dmr->capplus_seed[0].armed && dmr->capplus_seed[0].tg == 42U,
                    "the follow seeds the slot's group");
    g_state.last_vc_sync_time = 0;
    cap_plus_calls(3U, 44U);
    rc |= expect(dmr->vc_freq_hz == 451000000L && dmr->capplus_seed[0].armed && dmr->capplus_seed[0].tg == 44U,
                 "another group on the followed carrier is seeded without a retune");
    cleanup();

    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    g_opts.trunk_scan_enabled = 0;
    dmr = dmr_sm_get_ctx();
    const uint64_t untracked = dsd_trunk_tuning_request_begin(); // a follow tune still in flight
    dsd_trunk_tuning_request_mark_ready(untracked);
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr->cc_moved_m -= 7.0; // the move was seven seconds ago
    dmr->t_cc_sync_m -= 7.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->cc_tune_superseded && dmr->cc_settle_waited, "the move waits for the tune in flight");
    dsd_trunk_tuning_request_publish(untracked, DSD_TRUNK_TUNE_RESULT_OK);
    const int tunes = g_cc_tunes;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_ON_CC && !dmr->cc_tune_superseded && g_cc_tunes == tunes
                     && dsd_decode_now_mono_s() - dmr->t_cc_sync_m < 1.0,
                 "the window starts when frames flowed, not at the move");
    cleanup();
    return rc;
}

/* Ordinary recovery from a confirmed control channel A probes B, where valid XPT content identifies the site: it parks
   on B, where it identified itself, and never returns to A. */
static int
dmr_xpt_identified_on_a_probe_parks_there(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    single_system_cold_start(0);
    aloha(1);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    int rc = expect(dmr->cc_confirmed && dmr->cc_freq_hz == 450000000L, "control channel A confirmed");
    dmr->t_cc_sync_m -= dmr->cc_grace_s + 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->cc_acquiring && g_frequency == 451000000L, "the hunt probes B");
    xpt_csbk(0x0B);
    g_opts.trunk_is_tuned = 0;
    rc |= expect(dmr->site_without_cc && dmr->cc_freq_hz == 451000000L && g_state.trunk_cc_freq == 451000000L,
                 "the XPT site parks where it identified itself, the shared return included");
    const int tunes = g_cc_tunes;
    for (int i = 0; i < 4; ++i) {
        dmr->cc_acquire_start_m = dmr->t_cc_sync_m = dsd_decode_now_mono_s() - 30.0;
        dmr->cc_retry_after_m = 0.0;
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    }
    rc |= expect(g_cc_tunes == tunes && g_frequency == 451000000L, "and never returns to A");
    // A trunk-scan revisit restarts acquisition on the target's shared return.
    dmr_sm_begin_cc_acquisition(dmr, &g_opts, &g_state, g_state.trunk_cc_freq, 0U);
    rc |= expect(dmr->site_without_cc && dmr->cc_freq_hz == 451000000L, "a revisit parks where the site was found");
    cleanup();
    return rc;
}

/* A clean Capacity Plus group voice LC header (FID 0x10, FLCO 0x04) on slot 0 for TG 42 announcing @p rest. */
static void
cap_plus_voice_lc(unsigned int rest) {
    uint8_t bits[96] = {0};
    const uint32_t fields[][3] = {{2U, 6U, 0x04U}, {8U, 8U, 0x10U}, {40U, 8U, 42U}, {51U, 5U, 0U}, {56U, 16U, 2001U}};
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; ++f) {
        const uint32_t value = f == 3 ? rest : fields[f][2];
        for (uint32_t i = 0U; i < fields[f][1]; ++i) {
            bits[fields[f][0] + i] = (uint8_t)((value >> (fields[f][1] - 1U - i)) & 1U);
        }
    }
    uint32_t errors = 0U;
    g_state.currentslot = 0;
    g_state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dmr_flco(&g_opts, &g_state, bits, 1U, &errors, 1U);
}

/* A call a Capacity Plus status followed, whose voice LC announces a new rest channel no later status repeats: its
   release goes there, as the no-carrier return would. */
static int
dmr_status_follow_releases_to_the_announced_rest(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(3U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_state.last_vc_sync_time = 0;
    cap_plus_two_calls(3U);
    cap_plus_voice_lc(4U);
    int rc = expect(dmr->state == DMR_SM_TUNED && g_state.dmr_rest_channel == 4, "the voice LC announces rest LSN 4");
    const int returns = g_returns;
    dmr->t_tune_m -= dmr->grant_timeout_s + 0.5;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_returns == returns + 1 && g_frequency == 453000000L, "the release goes to the announced rest");
    cleanup();
    return rc;
}

/* A settle after an operator move whose superseded probe failed, under a rigctl peer that no longer answers frequency
   reads: the read is not repeated on every tick. */
static int
dmr_settle_read_backs_off(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    aloha(1);
    g_opts.trunk_scan_enabled = 0;
    g_hw_model = 1;
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    dmr->t_cc_sync_m -= dmr->cc_grace_s + 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    const uint64_t probe = g_request;
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_FAILED);
    g_query_failed = 1;
    const int queries = g_queries;
    for (int i = 0; i < 5; ++i) {
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    }
    int rc = expect(g_queries - queries <= 1 && dmr->cc_tune_superseded, "a failed read is not repeated per tick");
    cleanup();
    return rc;
}

/* A data call a Capacity Plus status followed with data calls followed and a zero hangtime: accepted data holds it
   while it flows. */
static int
dmr_status_followed_data_holds(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    g_opts.trunk_tune_data_calls = 1;
    cap_plus_map();
    cap_plus_idle(3U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_state.last_vc_sync_time = 0;
    cap_plus_two_calls(3U);
    const int returns = g_returns;
    dmr_sm_emit_data_sync(&g_opts, &g_state, 0);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    int rc = expect(dmr->state == DMR_SM_TUNED && g_returns == returns, "accepted data holds a zero-hangtime follow");
    dmr->slots[0].last_active_m -= 1.0;
    dmr->t_voice_m -= 1.0;
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(g_returns == returns + 1, "and the call returns once the data stops");
    cleanup();
    return rc;
}

/* After an operator move under rigctl, repeated statuses naming a rest elsewhere whose return is refused read the
   radio once, not per burst. */
static int
dmr_rigctl_read_once_per_generation(void) {
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(1U);
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    g_opts.trunk_scan_enabled = 0;
    g_frequency = 453000000L;
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    g_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    const int queries = g_queries;
    for (int i = 0; i < 5; ++i) {
        cap_plus_idle(1U);
    }
    int rc = expect(g_queries - queries <= 1, "the radio is read once per tuning generation");
    cleanup();

    // A peer that stops answering frequency reads is not asked again on every burst.
    if (setup(0.0f)) {
        cleanup();
        return 1;
    }
    cap_plus_map();
    cap_plus_idle(1U);
    dmr = dmr_sm_get_ctx();
    g_opts.trunk_scan_enabled = 0;
    dmr_sm_cold_start_note_retune(dmr, &g_opts, &g_state);
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    g_query_failed = 1;
    const int failed_queries = g_queries;
    for (int i = 0; i < 5; ++i) {
        xpt_csbk(0x0B);
        g_opts.trunk_is_tuned = 0;
    }
    rc |= expect(g_queries - failed_queries <= 1, "a failed read is not repeated per burst");
    cleanup();
    return rc;
}

int
main(void) {
    (void)dsd_test_unsetenv("DSD_NEO_DMR_HANGTIME");
    (void)dsd_test_unsetenv("DSD_NEO_DMR_GRANT_TIMEOUT");
    dsd_neo_config_init();
    int rc = held_call_return(0.0f);
    rc |= held_call_return(2.0f);
    rc |= hunt_and_reacquire();
    rc |= pending_and_failed_probes();
    rc |= cap_plus_rest_recovery();
    rc |= switch_while_probe_pending();
    rc |= standalone_recovery();
    rc |= heartbeat_and_single_candidate();
    rc |= busy_cap_plus_rest();
    rc |= pending_probe_dwell();
    rc |= watchdog_ownership_thread();
    rc |= watchdog_call_skip_phases();
    rc |= user_block_mutes_queued_voice();
    rc |= probe_grant_and_missing_anchor();
    rc |= cap_plus_idle_preserves_hangtime();
    rc |= hunt_fade_and_avoids();
    rc |= pending_probe_timeout();
    rc |= pending_park_timeout();
    rc |= visit_hook_defers_tuning_until_decoder_unwinds();
    rc |= dmr_cold_start_hunts_the_map();
    rc |= dmr_live_cc_at_start_never_hunts();
    rc |= dmr_cold_start_holds_validated_content();
    rc |= dmr_owner_named_without_confirmation_keeps_hunting();
    rc |= dmr_xpt_site_stays_in_place();
    rc |= dmr_named_owner_settles_a_probe_in_flight();
    rc |= dmr_abandoned_carrier_gets_a_window();
    rc |= dmr_windows_hold_on_validated_content();
    rc |= dmr_operator_move_drops_attribution();
    rc |= dmr_status_naming_the_alias_returns_at_once();
    rc |= run_on_test_decode_clock(dmr_status_follow_is_owned);
    rc |= dmr_xpt_retries_its_own_frequency();
    rc |= run_on_test_decode_clock(dmr_operator_move_settles_a_superseded_probe);
    rc |= dmr_rigctl_read_once_per_generation();
    rc |= dmr_follow_after_a_move_stands();
    rc |= dmr_followed_carrier_and_untracked_settle();
    rc |= dmr_xpt_identified_on_a_probe_parks_there();
    rc |= dmr_status_followed_data_holds();
    rc |= dmr_status_follow_releases_to_the_announced_rest();
    rc |= dmr_settle_read_backs_off();
    rc |= dmr_stop_with_probe_in_flight();
    rc |= dmr_stop_after_deadline_then_late_success();
    rc |= dmr_one_row_map_retries_a_failed_probe();
    rc |= dmr_cold_start_followed_grant_stands();
    rc |= dmr_grant_from_idle_disarms_the_window();
    rc |= dmr_settle_waits_for_a_tune_in_flight();
    rc |= dmr_settle_tune_deadline();
    rc |= dmr_stop_trunking_off_parks_at_once();
    rc |= dmr_operator_moves();
    rc |= ft_cold_start_hands_dmr_over();
    return rc;
}
