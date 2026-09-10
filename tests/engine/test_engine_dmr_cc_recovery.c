// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Real scan coordinator, control-message parser and sync-loss callbacks, with
 * only the tuner replaced. In particular, neither protocol SM is stubbed. */
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
static uint64_t g_request;
static dsd_trunk_tune_result g_result;

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
    return g_frequency;
}

static dsd_trunk_tune_result
cc_tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)sps;
    ++g_cc_tunes;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_result)) {
        g_frequency = freq;
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
        g_frequency = state->trunk_cc_freq;
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
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});
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
    g_cc_tunes = g_returns = 0;
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
    ctx->cc_acquire_start_m = ctx->t_cc_sync_m = dsd_time_now_monotonic_s() - 3.0;
    ctx->cc_retry_after_m = 0.0;
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
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
    ctx->t_cc_sync_m -= 3.0;
    aloha(1);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_ON_CC && ctx->cc_confirmed, "decoded ALOHA keeps DMR on CC");
    rc |= expect(!g_opts.frame_p25p1 && !g_opts.frame_p25p2, "DMR row excludes P25");
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == 451000000L, "control-like messages on a followed VC cannot replace CC");
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0);
    ctx->slots[0].last_active_m -= 3.0;
    ctx->t_voice_m -= 3.0;
    g_state.last_vc_sync_time = time(NULL) - 3;
    g_state.last_vc_sync_time_m = dsd_time_now_monotonic_s() - 3.0;
    g_state.p25_last_vc_tune_time_m = dsd_time_now_monotonic_s() - 4.0;
    dsd_frame_sync_test_handle_no_sync_timeout(&g_opts, &g_state, 1800);
    rc |= expect(g_returns == 1 && g_frequency == 451000000L, "DMR returned to original control channel");
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
    rc |= expect(g_frequency == 453000000L, "hunt skips duplicate channel frequencies");
    aloha(1);
    rc |= expect(!ctx->cc_acquiring && ctx->cc_confirmed && g_state.trunk_cc_freq == 453000000L,
                 "decoded control channel replaces the old anchor");
    ctx->t_cc_sync_m -= 3.0;
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
    rc |= expect(ctx->cc_tune_request_id == pending && g_cc_tunes == count && ctx->cc_acquiring,
                 "pending tune neither times out nor accepts old-frequency control messages");
    dsd_trunk_tuning_request_publish(pending, DSD_TRUNK_TUNE_RESULT_OK);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->cc_tune_request_id == 0U && g_cc_tunes == count
                     && dsd_time_now_monotonic_s() - ctx->cc_acquire_start_m < 1.0,
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
cap_plus_idle(unsigned int rest) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 0x3e;
    bytes[1] = 0x10;
    bytes[2] = (uint8_t)(0xc0U | rest); // Single complete status, all channel banks idle.
    for (int i = 0; i < 24; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1, 0);
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
    g_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    cap_plus_idle(2U);
    rc |= expect(g_frequency == 451000000L && g_state.trunk_cc_freq == 451000000L,
                 "failed CAP+ rest move retains the previous anchor");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    cap_plus_idle(2U);
    rc |= expect(g_frequency == 452000000L && g_state.trunk_cc_freq == 452000000L && dmr_sm_get_ctx()->cc_acquiring,
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
    return rc;
}
