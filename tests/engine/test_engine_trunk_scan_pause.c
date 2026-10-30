// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* A trunk scan paused and resumed (issue #671) through the real P25 and DMR trunk state machines, the real carrier
 * boundary, the real tuning ledger and the real, non-reentrant P25 SM tick guard, driven by the app-control services
 * and commands a frequency entry, `y`, a tuner release and SCAN_RESUME run. Only the tuner is simulated: the trunk
 * tuning hooks answer for the backend, and the frequency entry's tune leg stands in for io_control (a rigctl peer or
 * an RTL stream). ENGINE_TRUNK_SCAN covers the coordinator's own rules with all of these stubbed. */
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/services.h"

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
#include "test_support.h"
#include "trunk_scan_internal.h"
#include "trunk_scan_test_support.h"

/* Built into this test with its test hooks (key_set.c): a resume whose key preparation fails. */
void dsd_key_set_test_alloc_fail_after(long count);

static dsd_opts g_opts;
static dsd_state g_state;
static char g_dir[DSD_TEST_PATH_MAX];
static char g_targets[DSD_TEST_PATH_MAX];
static char g_channels[DSD_TEST_PATH_MAX];
static char g_keys[DSD_TEST_PATH_MAX];

/* The simulated tuner. */
static long g_frequency;
static int g_cc_tunes;
static int g_vc_tunes;
static uint64_t g_request;
static dsd_trunk_tune_result g_result;
static int g_entry_tunes;
static int g_entry_result;

enum { k_entry_hz = 860000000, k_p25_cc_hz = 851000000, k_p25_vc_hz = 852000000, k_dmr_cc_hz = 451000000 };

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

/* A park or control-channel tune: an accepted one moves the radio at once, as the backend will once it answers. */
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
    ++g_vc_tunes;
    g_frequency = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_request = request_id;
    g_frequency = state->p25_cc_freq > 0 ? state->p25_cc_freq : state->trunk_cc_freq;
    opts->rtlsdr_center_freq = (uint32_t)g_frequency;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

#ifdef USE_RADIO
/* The frequency entry's tune (svc_rtl_tune_only()'s shape): applied at once (a rigctl peer, a landed RTL tune) or
   accepted with its completion still to come (an RTL tune whose wait timed out). */
static int
entry_leg(dsd_opts* opts, dsd_state* state, uint32_t hz) {
    (void)state;
    ++g_entry_tunes;
    if (g_entry_result == RTL_STREAM_TUNE_OK || g_entry_result == RTL_STREAM_TUNE_TIMEOUT) {
        g_frequency = (long)hz;
        opts->rtlsdr_center_freq = hz;
    }
    return g_entry_result;
}
#endif

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
    dsd_app_frontend_runtime_stop();
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    dsd_frame_sync_hooks_set(NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_trunk_tuning_requests_reset();
    trunk_scan_test_clear_now();
    freeState(&g_state);
    (void)remove(g_targets);
    (void)remove(g_channels);
    (void)remove(g_keys);
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(g_dir);
#else
    (void)rmdir(g_dir);
#endif
}

/* A keyed P25 system first (parked at start, its key set what a resume prepares), then a DMR Tier III system. The
   operator hold keeps the dwell from rotating under the steps. */
static int
setup(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    initOpts(&g_opts);
    initState(&g_state);
    g_opts.verbose = 0;
    g_opts.audio_in_type = AUDIO_IN_NULL;
    g_opts.audio_out_type = 9;
    g_opts.use_rigctl = 1;
    g_opts.trunk_scan_enabled = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_cc_tunes = g_vc_tunes = g_entry_tunes = 0;
    g_request = 0U;
    g_frequency = 0;
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_entry_result = RTL_STREAM_TUNE_OK;
    dsd_trunk_tuning_requests_reset();
    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);
    dsd_engine_frame_sync_hooks_install();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = vc_tune,
        .tune_to_cc_request = cc_tune,
        .return_to_cc_request = return_to_cc,
    });
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = current_frequency});
    if (!dsd_test_mkdtemp(g_dir, sizeof(g_dir), "dsdneo_scan_pause")
        || dsd_test_path_join(g_targets, sizeof(g_targets), g_dir, "targets.csv") != 0
        || dsd_test_path_join(g_channels, sizeof(g_channels), g_dir, "channels.csv") != 0
        || dsd_test_path_join(g_keys, sizeof(g_keys), g_dir, "hexkeys.csv") != 0
        || write_file(g_channels, "channel,frequency\n1,451000000\n2,452000000\n")
        || write_file(g_keys, "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA\n")
        || write_file(g_targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv\n"
                                 "p25,p25-trunk,851000000,,250,1200,,hexkeys.csv\n"
                                 "tiii,dmr-trunk,451000000,channels.csv,250,1200,,\n")) {
        return 1;
    }
    DSD_SNPRINTF(g_opts.trunk_scan_targets_csv, sizeof(g_opts.trunk_scan_targets_csv), "%s", g_targets);
    trunk_scan_test_set_now(0.0);
    char error[256] = {0};
    if (dsd_engine_trunk_scan_init(&g_opts, &g_state, error, sizeof(error)) != 0) {
        DSD_FPRINTF(stderr, "scan init: %s\n", error);
        return 1;
    }
    dsd_app_frontend_runtime_start(&g_opts, &g_state);
    return expect(dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 1,
                  "hold enabled")
           | expect(g_frequency == k_p25_cc_hz && dsd_engine_trunk_scan_active_p25_ctx() != NULL,
                    "the scan parked on the P25 system");
}

/* One command through the queue and its drain, as a frontend's key or button submits it. */
static int
run_action(int cmd_id) {
    g_state.ui_msg[0] = '\0';
    if (dsd_app_command_action(cmd_id) <= 0) {
        return 0;
    }
    return dsd_app_drain_cmds(&g_opts, &g_state);
}

static int
toast_is(const char* want) {
    if (strcmp(g_state.ui_msg, want) != 0) {
        DSD_FPRINTF(stderr, "toast \"%s\", want \"%s\"\n", g_state.ui_msg, want);
        return 0;
    }
    return 1;
}

static int
frames_current(void) {
    return dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation());
}

/* The paused state every consumer reads: the flag, no state machine behind the hooks, no target named, trunking and the
   target's scope left. */
static int
expect_paused(const char* stage) {
    const int ok = g_state.trunk_scan_paused == 1U && dsd_engine_trunk_scan_paused(&g_state) == 1
                   && g_state.operator_tuned == 1U && dsd_trunk_scan_hook_p25_ctx() == NULL
                   && dsd_trunk_scan_hook_dmr_ctx() == NULL && !dsd_trunk_p25_recovery_allowed(&g_opts, &g_state)
                   && !dsd_trunk_dmr_recovery_allowed(&g_opts, &g_state) && g_state.trunk_scan_active_id[0] == '\0'
                   && g_opts.trunk_enable == 0 && g_opts.trunk_is_tuned == 0
                   && dsd_scan_mode_configured_view(&g_state) == NULL && g_state.trunk_scan_hold == 1U
                   && !dsd_trunk_scan_owns_tuner(&g_opts, &g_state);
    if (!ok) {
        DSD_FPRINTF(stderr,
                    "%s: not a coherent pause: paused=%u/%d operator=%u p25=%d dmr=%d id='%s' trunk=%d tuned=%d "
                    "scope=%d hold=%u\n",
                    stage, (unsigned)g_state.trunk_scan_paused, dsd_engine_trunk_scan_paused(&g_state),
                    (unsigned)g_state.operator_tuned, dsd_trunk_scan_hook_p25_ctx() != NULL,
                    dsd_trunk_scan_hook_dmr_ctx() != NULL, g_state.trunk_scan_active_id, g_opts.trunk_enable,
                    g_opts.trunk_is_tuned, dsd_scan_mode_configured_view(&g_state) != NULL,
                    (unsigned)g_state.trunk_scan_hold);
    }
    return expect(ok, stage);
}

static int
expect_running_on(const char* stage, const char* id) {
    const int ok = g_state.trunk_scan_paused == 0U && dsd_engine_trunk_scan_paused(&g_state) == 0
                   && g_state.operator_tuned == 0U && strcmp(g_state.trunk_scan_active_id, id) == 0
                   && g_opts.trunk_enable == 1 && dsd_scan_mode_configured_view(&g_state) != NULL
                   && dsd_trunk_scan_owns_tuner(&g_opts, &g_state)
                   && (dsd_trunk_scan_hook_p25_ctx() != NULL || dsd_trunk_scan_hook_dmr_ctx() != NULL);
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: not running on '%s': paused=%u operator=%u id='%s' trunk=%d scope=%d\n", stage, id,
                    (unsigned)g_state.trunk_scan_paused, (unsigned)g_state.operator_tuned, g_state.trunk_scan_active_id,
                    g_opts.trunk_enable, dsd_scan_mode_configured_view(&g_state) != NULL);
    }
    return expect(ok, stage);
}

/* ---- The P25 Phase 2 call a pause interrupts ------------------------------------------------------------------- */

static struct {
    int flushes;
    int call_active;
    int under_target;
    int queued;
    int audio_blocks;
} g_flush;

static void
count_audio(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    if (bytes > 0 && data) {
        ++g_flush.audio_blocks;
    }
}

/* The release's delivery of the partial superframe, observed: it must run while the call and its target still stand. */
static void
observe_flush(dsd_opts* opts, dsd_state* state) {
    ++g_flush.flushes;
    dsd_call_snapshot call;
    g_flush.call_active = dsd_call_state_get(state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
    g_flush.under_target = strcmp(state->trunk_scan_active_id, "p25") == 0 && dsd_trunk_scan_owns_tuner(opts, state);
    g_flush.queued = dsd_p25p2_playout_level(state, 0);
    dsd_p25p2_flush_partial_audio(opts, state);
}

/* A control message decoded on the P25 target's control channel: the acquisition its park began is complete. */
static void
decoded_p25_control(void) {
    dsd_sleep_ms(2U);
    g_state.p25_last_cc_msg_time_m = dsd_decode_now_mono_s();
}

/* A followed group call on a TDMA voice channel, with one admitted frame waiting in the Phase 2 playout. */
static int
seed_phase2_call(void) {
    g_opts.floating_point = 0;
    g_opts.pulse_digi_rate_out = 8000;
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.slot1_on = g_opts.slot2_on = 1;
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_state.p25_chan_tdma_explicit[1] = 2;
    dsd_state_set_trunk_chan_freq(&g_state, 0x1234, k_p25_vc_hz);
    dsd_trunk_recovery_note_protocol(&g_state, DSD_TRUNK_RECOVERY_P25);
    decoded_p25_control();
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, k_p25_vc_hz, 1234, 42, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    int rc = expect(p25_sm_emit_active_call(&g_opts, &g_state, 0, 1234, 0, 42, 1, 0), "the voice is accepted");
    rc |= expect(ctx == dsd_trunk_scan_hook_p25_ctx() && ctx->state == P25_SM_TUNED && ctx->vc_is_tdma
                     && g_frequency == k_p25_vc_hz,
                 "the target's machine follows the TDMA grant");
    g_state.p25_p2_audio_allowed[0] = 1;
    for (int i = 0; i < 160; ++i) {
        g_state.s_l[i] = 1000;
    }
    g_state.mbe_short_silenced[0] = 0;
    dsd_p25p2_playout_reset(&g_state, -1);
    dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 0, 1U, NULL);
    rc |= expect(dsd_p25p2_playout_level(&g_state, 0) > 0, "one frame waits in the playout");
    DSD_MEMSET(&g_flush, 0, sizeof(g_flush));
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){.p25p2_flush_partial_audio = observe_flush});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = count_audio});
    return rc;
}

static int
history_rows_for(uint32_t target) {
    int rows = 0;
    for (int i = 1; i < DSD_EVENT_HISTORY_LEN; ++i) {
        rows += g_state.event_history_s[0].Event_History_Items[i].target_id == target;
    }
    return rows;
}

/* A pause over a Phase 2 call, for a frequency entry (@p entry 1) or in place (`y`): the partial superframe plays into
   the call while it and its target stand, the call ends once and commits under the target, and the target is saved
   idle. The entry's real TUNE boundary then runs on the manual carrier with no target machine behind the hooks; in
   place, the pause forgets the carrier itself. SCAN_RESUME then brings the target back with no call. */
static int
pause_over_a_phase2_call(int entry) {
    if (setup()) {
        cleanup();
        return 1;
    }
    int rc = seed_phase2_call();
    const uint64_t carrier_seq = g_state.carrier_seq;
    g_state.carrier_change_seen = 0U;
    if (entry) {
#ifdef USE_RADIO
        int paused = 0;
        rc |= expect(svc_pause_trunk_scan_for_tune(&g_opts, &g_state, k_entry_hz, entry_leg, &paused)
                             == RTL_STREAM_TUNE_OK
                         && paused == 1 && g_entry_tunes == 1 && g_frequency == k_entry_hz,
                     "the entry tunes and pauses the scan");
        rc |= expect(g_state.carrier_seq == carrier_seq + 1U, "the TUNE boundary ran once");
#endif
    } else {
        rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"), "y pauses");
        rc |= expect(g_state.carrier_seq == carrier_seq && g_frequency == k_p25_vc_hz, "nothing tuned");
    }
    rc |= expect(!p25_sm_tick_guard_held_by_caller() && p25_sm_tick_guard_try_enter(), "the pause leaves the guard");
    p25_sm_tick_guard_leave();
    rc |= expect(g_flush.flushes == 1 && g_flush.call_active && g_flush.under_target && g_flush.queued > 0,
                 "the partial superframe is delivered while the call and its target stand");
    rc |= expect(g_flush.audio_blocks > 0 && dsd_p25p2_playout_level(&g_state, 0) == 0,
                 "the queued frame plays into the call");
    dsd_call_snapshot call;
    rc |= expect(dsd_call_state_get(&g_state, 0, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE, "the call ended");
    rc |= expect(history_rows_for(1234) == 1
                     && strcmp(g_state.event_history_s[0].Event_History_Items[1].channel_label, "p25") == 0,
                 "the call committed once, under the target");
    rc |= expect_paused("pause over a call");
    rc |= expect(g_state.carrier_change_seen == 1U && g_state.trunk_chan_map[0x1234] == 0,
                 "the carrier left is forgotten, and no target table stays live");
    rc |= expect(g_vc_tunes == 1 && frames_current(), "no voice tune besides the grant's, and no tune left pending");

    const int tunes = g_cc_tunes;
    rc |= expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1 && toast_is("Trunk scan resumed"), "SCAN_RESUME resumes");
    rc |= expect_running_on("resume", "p25");
    p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_trunk_scan_hook_p25_ctx();
    rc |= expect(g_cc_tunes == tunes + 1 && g_frequency == k_p25_cc_hz && ctx != NULL && ctx->state != P25_SM_TUNED
                     && g_state.trunk_vc_freq[0] == 0 && g_state.trunk_chan_map[0x1234] == k_p25_vc_hz,
                 "the target comes back on its control channel, its tables kept, its call not");
    rc |= expect(dsd_call_state_get(&g_state, 0, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE,
                 "the resumed target has no call");
    rc |= expect(history_rows_for(1234) == 1, "the call ended once");
    cleanup();
    return rc;
}

/* ---- Requests in flight at a pause ----------------------------------------------------------------------------- */

/* `y` twice from a running scan: paused in place, then resumed onto a park the backend has not answered yet, which the
   P25 target's machine waits on as its own control-channel tune. Returns that park's request. */
static uint64_t
resume_onto_a_pending_park(int* rc) {
    *rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"), "y pauses");
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    *rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan resumed"), "y resumes");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    const uint64_t park = g_request;
    const p25_sm_ctx_t* ctx = (const p25_sm_ctx_t*)dsd_trunk_scan_hook_p25_ctx();
    p25_sm_tick_guard_enter();
    const int in_flight = dsd_engine_trunk_scan_resume_locked(&g_opts, &g_state) == 1;
    p25_sm_tick_guard_leave();
    *rc |=
        expect(in_flight && ctx && ctx->cc_tune_pending && ctx->cc_tune_request_id == park
                   && dsd_trunk_tuning_request_status(park, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING && !frames_current(),
               "the resume runs with its park in flight, which the target's machine waits on");
    return park;
}

/* After its pause the P25 target's machine is clean: resumed, it follows a grant at once (a pending tune it was left
   waiting on would hold the grant back). */
static int
expect_p25_target_follows_a_grant(const char* stage) {
    p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_trunk_scan_hook_p25_ctx();
    if (!ctx) {
        return expect(0, stage);
    }
    decoded_p25_control();
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_state.p25_chan_tdma_explicit[1] = 2;
    dsd_state_set_trunk_chan_freq(&g_state, 0x1234, k_p25_vc_hz);
    const int tunes = g_vc_tunes;
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, k_p25_vc_hz, 1234, 42, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    const int ok = !ctx->cc_tune_pending && ctx->cc_tune_request_id == 0U && ctx->state == P25_SM_TUNED
                   && g_vc_tunes == tunes + 1 && g_frequency == k_p25_vc_hz;
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: state=%d pending=%d request=%llu vc_tunes=%d freq=%ld\n", stage, ctx->state,
                    ctx->cc_tune_pending, (unsigned long long)ctx->cc_tune_request_id, g_vc_tunes - tunes, g_frequency);
    }
    return expect(ok, stage);
}

/* `y` on a resume whose park is still in flight (a pending resume counts as running): the scan pauses in place, lets
   go of the park without completing it, and clears the target machine's pending tune. Frames wait for the backend's
   answer only: once it answers, success or failure, nothing holds them, nothing is rolled back, and the paused scan
   does not move. The resume after it parks afresh, and the target's machine follows a grant. */
static int
pause_on_a_pending_park(dsd_trunk_tune_result outcome) {
    if (setup()) {
        cleanup();
        return 1;
    }
    int rc = 0;
    const uint64_t park = resume_onto_a_pending_park(&rc);
    const p25_sm_ctx_t* ctx = (const p25_sm_ctx_t*)dsd_trunk_scan_hook_p25_ctx();
    g_state.carrier_change_seen = 0U;
    rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"),
                 "y over the pending resume pauses in place");
    rc |= expect_paused("pause over a pending park");
    rc |= expect(ctx && !ctx->cc_tune_pending && ctx->cc_tune_request_id == 0U && g_state.carrier_change_seen == 1U,
                 "the target's machine waits on nothing, and the carrier's decoding is forgotten");
    rc |= expect(dsd_trunk_tuning_request_status(park, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
                     && dsd_trunk_tuning_pending_request() == park && !frames_current(),
                 "the park stays the backend's: frames wait for its answer");

    const long frequency = g_frequency;
    const uint32_t centre = g_opts.rtlsdr_center_freq;
    dsd_trunk_tuning_request_publish(park, outcome);
    rc |= expect(dsd_trunk_tuning_pending_request() == 0U && frames_current(),
                 "the backend's answer releases the frames");
    rc |= expect(g_frequency == frequency && g_opts.rtlsdr_center_freq == centre,
                 "nothing is rolled back for a park let go of");
    const int tunes = g_cc_tunes;
    trunk_scan_test_set_now(30.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes, "the paused scan does not move on the answer");
    rc |= expect_paused("after the answer");

    rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan resumed"), "y resumes again");
    rc |= expect_running_on("resume after the answer", "p25");
    rc |= expect(g_cc_tunes == tunes + 1 && g_frequency == k_p25_cc_hz && frames_current(), "a fresh park lands");
    rc |= expect_p25_target_follows_a_grant("resumed P25 machine");
    cleanup();
    return rc;
}

/* A pause lets go of a park, and the next resume's park supersedes it: the old park's late answer changes nothing of
   the target now parking, which still waits on its own request, and frames wait for both answers. */
static int
pending_park_superseded_by_a_resume(dsd_trunk_tune_result old_outcome) {
    if (setup()) {
        cleanup();
        return 1;
    }
    int rc = 0;
    const uint64_t old_park = resume_onto_a_pending_park(&rc);
    rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"), "y pauses");
    g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan resumed"), "y resumes");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    const uint64_t park = g_request;
    p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_trunk_scan_hook_p25_ctx();
    rc |= expect(ctx && park != old_park && ctx->cc_tune_pending && ctx->cc_tune_request_id == park,
                 "the resume's park supersedes the one let go of");
    const int tunes = g_cc_tunes;

    dsd_trunk_tuning_request_publish(old_park, old_outcome);
    trunk_scan_test_set_now(0.1);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    p25_sm_try_tick(&g_opts, &g_state);
    p25_sm_tick_guard_enter();
    const int still_pending = dsd_engine_trunk_scan_resume_locked(&g_opts, &g_state) == 1;
    p25_sm_tick_guard_leave();
    rc |= expect(still_pending && ctx && ctx->cc_tune_pending && ctx->cc_tune_request_id == park
                     && dsd_trunk_tuning_request_status(park, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
                     && g_cc_tunes == tunes && dsd_engine_trunk_scan_active_index(&g_state) == 0U,
                 "the old park's late answer leaves the new park in flight");
    rc |= expect_running_on("old park answered", "p25");
    rc |= expect(!frames_current(), "frames still wait for the new park");

    dsd_trunk_tuning_request_publish(park, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_try_tick(&g_opts, &g_state);
    trunk_scan_test_set_now(0.2);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect(frames_current() && ctx && !ctx->cc_tune_pending && g_cc_tunes == tunes,
                 "the new park's answer lands it and releases the frames");
    rc |= expect_p25_target_follows_a_grant("superseding park");
    cleanup();
    return rc;
}

/* ---- The DMR machine's own request ----------------------------------------------------------------------------- */

static void
aloha(void) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 25;
    for (int i = 0; i < 8; ++i) {
        bits[i] = (uint8_t)((bytes[0] >> (7 - i)) & 1);
    }
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    dmr_cspdu(&g_opts, &g_state, bits, bytes, 1U, 0);
}

/* A pause taken while the DMR target's control-channel tune is in flight: the coordinator's park (@p probe 0) or the
   machine's own hunt probe after the park landed (@p probe 1), a request only the machine holds, which its release
   forgets: the pause took it first and lets go of it. Once the backend fails it, frames flow; the resume's park starts
   a clean acquisition that a decoded ALOHA confirms, and a grant is followed. */
static int
pause_on_a_pending_dmr_tune(int probe) {
    if (setup()) {
        cleanup();
        return 1;
    }
    g_result = probe ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_PENDING;
    int rc = expect(dsd_engine_trunk_scan_control(&g_opts, &g_state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) >= 0,
                    "advance to the DMR system");
    dmr_sm_ctx_t* dmr = (dmr_sm_ctx_t*)dsd_trunk_scan_hook_dmr_ctx();
    if (!dmr) {
        rc |= expect(0, "the DMR system is parked");
        cleanup();
        return rc;
    }
    if (probe) {
        /* The control channel goes quiet past its grace: the machine hunts the target's map on its own request. */
        aloha();
        g_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        dmr->cc_acquire_start_m = dmr->t_cc_sync_m = dsd_decode_now_mono_s() - dmr->cc_grace_s - 1.0;
        dmr->cc_retry_after_m = 0.0;
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
        dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    }
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    const uint64_t request = g_request;
    rc |= expect(request != 0U && dmr->cc_tune_request_id == request && !frames_current(),
                 probe ? "the DMR machine waits on its own probe" : "the DMR machine waits on the park");
    rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"), "y pauses");
    rc |= expect_paused("pause over a DMR tune");
    rc |= expect(dmr->cc_tune_request_id == 0U
                     && dsd_trunk_tuning_request_status(request, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
                     && !frames_current(),
                 "the machine's request is let go of, still the backend's");
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect(frames_current(), "the backend's failure releases the frames");
    p25_sm_try_tick(&g_opts, &g_state);
    trunk_scan_test_set_now(30.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    rc |= expect_paused("after the failure");

    const int tunes = g_cc_tunes;
    rc |= expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1 && toast_is("Trunk scan resumed"), "SCAN_RESUME resumes");
    rc |= expect_running_on("DMR resume", "tiii");
    rc |= expect(dsd_trunk_scan_hook_dmr_ctx() == dmr && g_cc_tunes == tunes + 1 && g_frequency == k_dmr_cc_hz
                     && dmr->cc_tune_request_id == 0U && frames_current(),
                 "the resume parks on the DMR control channel");
    aloha();
    dmr_sm_tick_ctx(dmr, &g_opts, &g_state);
    rc |= expect(dmr->state == DMR_SM_ON_CC && dmr->cc_confirmed && !dmr->cc_acquiring
                     && g_state.trunk_cc_freq == k_dmr_cc_hz,
                 "a decoded ALOHA confirms the control channel");
    dmr_sm_emit_group_grant_slot(&g_opts, &g_state, 452000000L, 2, 0, 1001, 2001);
    rc |= expect(g_opts.trunk_is_tuned && g_frequency == 452000000L, "the resumed DMR machine follows a grant");
    cleanup();
    return rc;
}

/* ---- Resume failures -------------------------------------------------------------------------------------------- */

/* Both resume failures leave a coherent pause: a refused retune, then a failed preparation. In between, ticks of the
   coordinator and the watchdog move nothing, frames are not held by the refused park, and an edit of the target is
   stored for its next visit, which the resume that succeeds then runs. */
static int
resume_failures_stay_paused(void) {
    if (setup()) {
        cleanup();
        return 1;
    }
    /* The session the frontends saw the target under, which a paused scan no longer publishes. */
    const uint32_t session = g_state.scan_row_session;
    g_state.p2_wacn = 0xBEE00;
    dsd_state_set_trunk_chan_freq(&g_state, 0x1234, k_p25_vc_hz);
    int rc = expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"), "y pauses");
    rc |= expect(g_state.p2_wacn == 0U && g_state.trunk_chan_map[0x1234] == 0, "the pause leaves no target table live");
    /* The configured modulation the pause put back (final review, Minor 9): a refused resume's rollback leaves the
       target's scope, which must bring back exactly these, never the target's. An operator's modulation lock made
       while paused is the configured one now. */
    g_opts.mod_c4fm = 0;
    g_opts.mod_qpsk = 1;
    g_opts.mod_gfsk = 0;
    g_opts.mod_cli_lock = 1;
    const int mod_c4fm = g_opts.mod_c4fm;
    const int mod_qpsk = g_opts.mod_qpsk;
    const int mod_gfsk = g_opts.mod_gfsk;
    const int mod_cli_lock = g_opts.mod_cli_lock;
    /* What the manual carrier decoded since. */
    g_state.p2_wacn = 0xCAFE1;
    int tunes = g_cc_tunes;
    g_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    rc |=
        expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1 && toast_is("Scan stays paused: tune to 851.000000 MHz failed"),
               "a refused retune keeps the pause");
    g_result = DSD_TRUNK_TUNE_RESULT_OK;
    rc |= expect(g_cc_tunes == tunes + 1, "the resume asked the tuner once");
    rc |= expect_paused("refused retune");
    rc |= expect(g_state.p2_wacn == 0xCAFE1 && g_state.trunk_chan_map[0x1234] == 0,
                 "the manual session's state is back, the target's tables are not live");
    rc |= expect(g_opts.mod_c4fm == mod_c4fm && g_opts.mod_qpsk == mod_qpsk && g_opts.mod_gfsk == mod_gfsk
                     && g_opts.mod_cli_lock == mod_cli_lock,
                 "the refused retune's rollback keeps the configured modulation");
    rc |= expect(frames_current(), "the refused park holds no frames back");
    tunes = g_cc_tunes;
    trunk_scan_test_set_now(30.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes, "ticks after the refusal move nothing");

    const double squelch = g_opts.rtl_squelch_level;
    dsd_scan_row_edit_value value;
    DSD_MEMSET(&value, 0, sizeof(value));
    value.squelch_db = -45;
    rc |= expect(dsd_engine_trunk_scan_edit_target(&g_opts, &g_state, session, "p25", DSD_SCAN_ROW_FIELD_SQUELCH,
                                                   DSD_SCAN_ROW_EDIT_SET, &value, NULL)
                         == DSD_SCAN_ROW_EDIT_STORED
                     && fabs(g_opts.rtl_squelch_level - squelch) < 1e-12,
                 "an edit of the paused target is stored for its next visit");

    dsd_key_set_test_alloc_fail_after(0);
    rc |= expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1
                     && toast_is("Scan stays paused: scan channel 851.000000 MHz could not be set up"),
                 "a failed preparation keeps the pause");
    dsd_key_set_test_alloc_fail_after(-1);
    rc |= expect(g_cc_tunes == tunes, "the failed preparation tuned nothing");
    rc |= expect_paused("failed preparation");
    rc |= expect(g_opts.mod_c4fm == mod_c4fm && g_opts.mod_qpsk == mod_qpsk && g_opts.mod_gfsk == mod_gfsk
                     && g_opts.mod_cli_lock == mod_cli_lock,
                 "the failed preparation keeps the configured modulation");
    trunk_scan_test_set_now(60.0);
    dsd_engine_trunk_scan_tick(&g_opts, &g_state);
    p25_sm_try_tick(&g_opts, &g_state);
    rc |= expect(g_cc_tunes == tunes && frames_current(), "ticks after the failed preparation move nothing");

    rc |= expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1 && toast_is("Trunk scan resumed"), "the next resume parks");
    rc |= expect_running_on("resume after both failures", "p25");
    const double want = dsd_squelch_level_from_sql(-45.0);
    rc |= expect(g_cc_tunes == tunes + 1 && fabs(g_opts.rtl_squelch_level - want) <= 1e-9 * fabs(want),
                 "the resumed target runs the stored edit");
    rc |= expect(g_state.p2_wacn == 0xBEE00 && g_state.trunk_chan_map[0x1234] == k_p25_vc_hz,
                 "the resumed target's tables are back");
    rc |= expect_p25_target_follows_a_grant("resume after both failures");
    cleanup();
    return rc;
}

/* ---- The watchdog alongside ------------------------------------------------------------------------------------ */

/* The P25 watchdog's view: its tick (p25_sm_try_tick()) and, under the same guard, the context hooks it reads. A
   pause or resume runs under the guard, so a reader holding it never sees a half-done one: paused, no machine answers
   and no recovery is allowed; running, the parked target's machine answers. Within one hold the hooks never move. */
static struct {
    atomic_int run;
    atomic_int reads;
    atomic_int paused_reads;
    atomic_int torn;
} g_watchdog;

static DSD_THREAD_RETURN_TYPE
watchdog_reader(void* arg) {
    (void)arg;
    while (atomic_load(&g_watchdog.run)) {
        p25_sm_try_tick(&g_opts, &g_state);
        if (p25_sm_tick_guard_try_enter()) {
            const void* p25 = dsd_trunk_scan_hook_p25_ctx();
            const void* dmr = dsd_trunk_scan_hook_dmr_ctx();
            const int paused = g_state.trunk_scan_paused != 0U;
            const int allowed = dsd_trunk_p25_recovery_allowed(&g_opts, &g_state);
            const int coherent = paused ? (!p25 && !dmr && !allowed && g_opts.trunk_enable == 0)
                                        : ((p25 != NULL) != (dmr != NULL) && g_opts.trunk_enable == 1);
            const int stable = (!p25 || p25_sm_get_ctx() == p25) && dsd_trunk_scan_hook_p25_ctx() == p25
                               && dsd_trunk_scan_hook_dmr_ctx() == dmr;
            if (!coherent || !stable) {
                (void)atomic_fetch_add(&g_watchdog.torn, 1);
            }
            (void)atomic_fetch_add(&g_watchdog.reads, 1);
            if (paused) {
                (void)atomic_fetch_add(&g_watchdog.paused_reads, 1);
            }
            p25_sm_tick_guard_leave();
        }
        dsd_sleep_ms(1U);
    }
    DSD_THREAD_RETURN;
}

/* Waits (bounded) until the watchdog has read the scan in the state just left for it, so every state each step leaves
   is read at least once. */
static int
watchdog_read_it(int paused) {
    atomic_int* counter = paused ? &g_watchdog.paused_reads : &g_watchdog.reads;
    const int before = atomic_load(counter);
    for (int i = 0; i < 2000 && atomic_load(counter) < before + 2; ++i) {
        dsd_sleep_ms(1U);
    }
    return expect(atomic_load(counter) >= before + 2,
                  paused ? "the watchdog read the pause" : "the watchdog read the scan");
}

/* Every pause and resume entry point, over and over, while the watchdog ticks and reads beside them. */
static int
watchdog_beside_pause_and_resume(void) {
    if (setup()) {
        cleanup();
        return 1;
    }
    /* The frontend's snapshot, published at the end of every drain, copies the state beside what the watchdog's tick
       writes: both hold the guard. */
    atomic_store(&g_watchdog.run, 1);
    atomic_store(&g_watchdog.reads, 0);
    atomic_store(&g_watchdog.paused_reads, 0);
    atomic_store(&g_watchdog.torn, 0);
    dsd_thread_t reader;
    int rc = expect(dsd_thread_create(&reader, watchdog_reader, NULL) == 0, "watchdog reader started");
    if (rc) {
        cleanup();
        return rc;
    }
    for (int i = 0; i < 20; ++i) {
        rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan paused"), "y pauses");
        rc |= watchdog_read_it(1);
        rc |= expect(run_action(DSD_APP_CMD_SCANNER_TOGGLE) == 1 && toast_is("Trunk scan resumed"), "y resumes");
        rc |= watchdog_read_it(0);
        rc |= expect(run_action(DSD_APP_CMD_TUNER_RELEASE) == 1 && toast_is("Automatic tuning paused"),
                     "a release pauses");
        rc |= watchdog_read_it(1);
        rc |= expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1 && toast_is("Trunk scan resumed"), "SCAN_RESUME resumes");
        rc |= watchdog_read_it(0);
#ifdef USE_RADIO
        /* An entry still completing pauses as one applied does. */
        g_entry_result = (i & 1) ? RTL_STREAM_TUNE_TIMEOUT : RTL_STREAM_TUNE_OK;
        int paused = 0;
        rc |= expect(svc_pause_trunk_scan_for_tune(&g_opts, &g_state, k_entry_hz, entry_leg, &paused) == g_entry_result
                         && paused == 1,
                     "an entry pauses");
        rc |= watchdog_read_it(1);
        rc |= expect(run_action(DSD_APP_CMD_SCAN_RESUME) == 1 && toast_is("Trunk scan resumed"), "SCAN_RESUME resumes");
        rc |= watchdog_read_it(0);
#endif
    }
    atomic_store(&g_watchdog.run, 0);
    dsd_thread_join(reader);
    rc |= expect(atomic_load(&g_watchdog.torn) == 0, "the watchdog never saw a half-done pause or resume");
    rc |= expect_running_on("after the cycles", "p25");
    cleanup();
    return rc;
}

/* ---- The guard held elsewhere ----------------------------------------------------------------------------------- */

/* A watchdog tick in progress (a Phase 2 release, say) holds the guard. A `y`, a SCAN_RESUME, a tuner release or a
   frequency entry that arrives then waits for it: while the tick holds the guard nothing of the pause or resume has
   happened, not even the entry's tune; once it leaves, the whole step runs. */
static struct {
    dsd_mutex_t mutex;
    dsd_cond_t condition;
    int held;
    atomic_int released;
    atomic_int done; /* the decoder thread's step returned: a step that never waits ends the holder's wait */
    int baseline;    /* the drain's guard waits before the command, or -1 for the entry, whose wait is not counted */
    int stalled;     /* the count never moved: a fixture failure, never evidence of a wait */
    int untouched;
    uint8_t paused;
} g_holder;

static DSD_THREAD_RETURN_TYPE
guard_holder(void* arg) {
    (void)arg;
    p25_sm_tick_guard_enter();
    dsd_mutex_lock(&g_holder.mutex);
    g_holder.held = 1;
    dsd_cond_broadcast(&g_holder.condition);
    dsd_mutex_unlock(&g_holder.mutex);
    if (g_holder.baseline < 0) {
        /* The entry's service enters the guard blocking, with no count to watch: long enough for the decoder thread to
           reach the guard, best effort (the watchdog case holds the entry to the guard too). */
        dsd_sleep_ms(50U);
    } else {
        /* Until the drain counts its wait for the guard: from then on the decoder thread is blocked on it. */
        const double deadline = dsd_realtime_mono_s() + 10.0;
        while (dsd_app_command_test_policy_guard_waits() <= g_holder.baseline && atomic_load(&g_holder.done) == 0) {
            if (dsd_realtime_mono_s() >= deadline) {
                g_holder.stalled = 1;
                break;
            }
            dsd_thread_yield();
        }
    }
    g_holder.untouched = g_state.trunk_scan_paused == g_holder.paused && g_state.ui_msg[0] == '\0' && g_entry_tunes == 0
                         && (g_holder.paused ? dsd_trunk_scan_hook_p25_ctx() == NULL
                                             : dsd_trunk_scan_hook_p25_ctx() != NULL && g_opts.trunk_enable == 1);
    atomic_store(&g_holder.released, 1);
    p25_sm_tick_guard_leave();
    DSD_THREAD_RETURN;
}

static int
start_guard_holder(dsd_thread_t* thread, int baseline) {
    g_state.ui_msg[0] = '\0';
    g_holder.held = 0;
    g_holder.baseline = baseline;
    g_holder.stalled = 0;
    g_holder.untouched = 0;
    g_holder.paused = g_state.trunk_scan_paused;
    atomic_store(&g_holder.released, 0);
    atomic_store(&g_holder.done, 0);
    if (dsd_thread_create(thread, guard_holder, NULL) != 0) {
        return 0;
    }
    dsd_mutex_lock(&g_holder.mutex);
    while (!g_holder.held) {
        if (dsd_cond_timedwait(&g_holder.condition, &g_holder.mutex, 2000) != 0) {
            break;
        }
    }
    const int held = g_holder.held;
    dsd_mutex_unlock(&g_holder.mutex);
    if (!held) {
        /* Not left running into the cleanup: whenever it takes the guard, it leaves at once. */
        atomic_store(&g_holder.done, 1);
        dsd_thread_join(*thread);
    }
    return held;
}

static int
drain_while_the_guard_is_held(int cmd_id, const char* toast, const char* stage) {
    const int baseline = dsd_app_command_test_policy_guard_waits();
    dsd_thread_t holder;
    if (!start_guard_holder(&holder, baseline)) {
        return expect(0, stage);
    }
    const int queued = dsd_app_command_action(cmd_id) > 0;
    const int drained = queued && dsd_app_drain_cmds(&g_opts, &g_state) == 1;
    atomic_store(&g_holder.done, 1);
    const int waited = atomic_load(&g_holder.released) == 1;
    dsd_thread_join(holder);
    const int counted = dsd_app_command_test_policy_guard_waits() == baseline + 1;
    if (!(drained && waited && counted && !g_holder.stalled && g_holder.untouched && toast_is(toast))) {
        DSD_FPRINTF(stderr, "%s: drained=%d waited=%d counted=%d stalled=%d untouched=%d\n", stage, drained, waited,
                    counted, g_holder.stalled, g_holder.untouched);
        return expect(0, stage);
    }
    return 0;
}

static int
pause_and_resume_wait_for_the_guard(void) {
    if (setup()) {
        cleanup();
        return 1;
    }
    dsd_mutex_init(&g_holder.mutex);
    dsd_cond_init(&g_holder.condition);
    int rc = drain_while_the_guard_is_held(DSD_APP_CMD_SCANNER_TOGGLE, "Trunk scan paused", "y waits, then pauses");
    rc |= expect_paused("y after the guard");
    rc |=
        drain_while_the_guard_is_held(DSD_APP_CMD_SCAN_RESUME, "Trunk scan resumed", "SCAN_RESUME waits, then resumes");
    rc |= expect_running_on("SCAN_RESUME after the guard", "p25");
    rc |= drain_while_the_guard_is_held(DSD_APP_CMD_TUNER_RELEASE, "Automatic tuning paused",
                                        "a release waits, then pauses");
    rc |= expect_paused("release after the guard");
    rc |= drain_while_the_guard_is_held(DSD_APP_CMD_SCAN_RESUME, "Trunk scan resumed",
                                        "SCAN_RESUME after the release waits, then resumes");
    rc |= expect_running_on("SCAN_RESUME after the release", "p25");
#ifdef USE_RADIO
    dsd_thread_t holder;
    if (!start_guard_holder(&holder, -1)) {
        rc |= expect(0, "guard holder for the entry");
    } else {
        int paused = 0;
        const int tuned =
            svc_pause_trunk_scan_for_tune(&g_opts, &g_state, k_entry_hz, entry_leg, &paused) == RTL_STREAM_TUNE_OK;
        atomic_store(&g_holder.done, 1);
        const int waited = atomic_load(&g_holder.released) == 1;
        dsd_thread_join(holder);
        rc |= expect(tuned && paused == 1 && waited && g_holder.untouched && g_entry_tunes == 1,
                     "an entry waits for the guard, then tunes and pauses");
        rc |= expect_paused("entry after the guard");
    }
#endif
    dsd_cond_destroy(&g_holder.condition);
    dsd_mutex_destroy(&g_holder.mutex);
    cleanup();
    return rc;
}

int
main(void) {
    dsd_neo_config_init();
    int rc = 0;
#ifdef USE_RADIO
    rc |= pause_over_a_phase2_call(1);
#endif
    rc |= pause_over_a_phase2_call(0);
    rc |= pause_on_a_pending_park(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= pause_on_a_pending_park(DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= pending_park_superseded_by_a_resume(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= pending_park_superseded_by_a_resume(DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= pause_on_a_pending_dmr_tune(0);
    rc |= pause_on_a_pending_dmr_tune(1);
    rc |= resume_failures_stay_paused();
    rc |= watchdog_beside_pause_and_resume();
    rc |= pause_and_resume_wait_for_the_guard();
    return rc;
}
