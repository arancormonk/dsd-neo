// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Real scan coordinator, control-message parser and sync-loss callbacks, with
 * only the tuner replaced. In particular, neither protocol SM is stubbed. */
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_cc_activity.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
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
static int g_queries;
static int g_query_failed;
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
    ++g_queries;
    return g_query_failed ? 0 : g_frequency;
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
    g_cc_tunes = g_returns = g_queries = g_query_failed = 0;
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
    ctx->cc_acquire_start_m = ctx->t_cc_sync_m = dsd_time_now_monotonic_s() - ctx->cc_grace_s - 1.0;
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
    ctx->t_cc_sync_m -= ctx->cc_grace_s + 1.0;
    aloha(1);
    dmr_sm_tick_ctx(ctx, &g_opts, &g_state);
    rc |= expect(ctx->state == DMR_SM_ON_CC && ctx->cc_confirmed, "decoded ALOHA keeps DMR on CC");
    rc |= expect(!g_opts.frame_p25p1 && !g_opts.frame_p25p2, "DMR row excludes P25");
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    aloha(1);
    rc |= expect(g_state.trunk_cc_freq == 451000000L, "control-like messages on a followed VC cannot replace CC");
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0);
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
    g_state.last_vc_sync_time_m = dsd_time_now_monotonic_s() - 3.0;
    g_state.p25_last_vc_tune_time_m = dsd_time_now_monotonic_s() - 4.0;
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
    const double old_try = dsd_time_now_monotonic_s() - 20.0;
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
    const double old_try = dsd_time_now_monotonic_s() - 20.0;
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
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0);
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
    dmr_sm_emit_voice_sync(&g_opts, &g_state, 0);
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
    ctx->t_cc_sync_m = dsd_time_now_monotonic_s() - 3.0;
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
    ctx->cc_tune_deadline_m = dsd_time_now_monotonic_s() - 1.0;
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
    ctx->cc_tune_deadline_m = dsd_time_now_monotonic_s() - 1.0;
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
    dsd_scan_voice_gate_note_retune(&g_state, dsd_time_now_monotonic_s() - 2.0);
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
    rc |= probe_grant_and_missing_anchor();
    rc |= cap_plus_idle_preserves_hangtime();
    rc |= hunt_fade_and_avoids();
    rc |= pending_probe_timeout();
    rc |= pending_park_timeout();
    rc |= visit_hook_defers_tuning_until_decoder_unwinds();
    return rc;
}
