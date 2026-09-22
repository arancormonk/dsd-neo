// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Real P25 watchdog, policy stores, command drain and partial-audio playback.
 * Only tuning and the audio output boundary are replaced. */
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "commands_internal.h"
#include "engine_hooks_install.h"
#include "test_support.h"

static dsd_opts g_opts;
static dsd_state g_state;
static long g_frequency;
static int g_phase;
static char g_block_csv[DSD_TEST_PATH_MAX];
static char g_allow_csv[DSD_TEST_PATH_MAX];

typedef struct {
    uint64_t context;
    unsigned int generation;
    int allowed;
} policy_view;

static struct {
    dsd_mutex_t mutex;
    dsd_cond_t condition;
    int entered;
    int resume;
    dsd_atomic_u64 drain_done;
    int baseline;
    int stalled;
    int failures;
    int flushes;
    int audio_blocks;
    policy_view observed;
} g_flush;

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL phase %d: %s\n", g_phase, message);
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
    (void)request_id;
    g_frequency = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    state->trunk_cc_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
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
    (void)request_id;
    g_frequency = state->p25_cc_freq;
    opts->rtlsdr_center_freq = (uint32_t)g_frequency;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static policy_view
read_policy(const dsd_opts* opts, const dsd_state* state) {
    policy_view view = {0};
    dsd_tg_policy_decision decision;
    dsd_tg_policy_table_version(state, &view.context, &view.generation);
    if (dsd_tg_policy_evaluate_group_call(opts, state, 1234, 42, 0, 0, &decision) == 0) {
        view.allowed = decision.audio_allowed;
    }
    return view;
}

static int
same_version(policy_view a, policy_view b) {
    return a.context == b.context && a.generation == b.generation;
}

static void
audio_sink(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    if (bytes > 0 && data) {
        ++g_flush.audio_blocks;
    }
}

static void
barrier_flush(dsd_opts* opts, dsd_state* state) {
    dsd_mutex_lock(&g_flush.mutex);
    g_flush.entered = 1;
    dsd_cond_broadcast(&g_flush.condition);
    while (!g_flush.resume) {
        dsd_cond_wait(&g_flush.condition, &g_flush.mutex);
    }
    dsd_mutex_unlock(&g_flush.mutex);
    g_flush.observed = read_policy(opts, state);
    int left = -1, right = -1;
    g_flush.failures |= dsd_audio_group_gate_dual(opts, state, 1234, 0, 0, 1, &left, &right);
    g_flush.failures |= left != !g_flush.observed.allowed;
    ++g_flush.flushes;
    dsd_p25p2_flush_partial_audio(opts, state);
}

static void
release_barrier(void) {
    dsd_mutex_lock(&g_flush.mutex);
    g_flush.resume = 1;
    dsd_cond_broadcast(&g_flush.condition);
    dsd_mutex_unlock(&g_flush.mutex);
}

static DSD_THREAD_RETURN_TYPE
resume_flush(void* opaque) {
    (void)opaque;
    const double deadline = dsd_time_now_monotonic_s() + 10.0;
    /* Neither observation publishes the drain's writes to the watchdog. In the
     * red run, TSan can therefore observe the unguarded store replacement. */
    while (dsd_app_command_test_policy_guard_waits() <= g_flush.baseline
           && dsd_atomic_u64_load_relaxed(&g_flush.drain_done) == 0U) {
        if (dsd_time_now_monotonic_s() >= deadline) {
            g_flush.stalled = 1; /* Fixture failure, never evidence of contention. */
            break;
        }
        dsd_thread_yield();
    }
    release_barrier();
    DSD_THREAD_RETURN;
}

static int
seed_call(void) {
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
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 852000000, 1234, 42, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    int rc = expect(p25_sm_emit_active_call(&g_opts, &g_state, 0, 1234, 0, 42, 1, 0), "seed voice accepted");
    rc |= expect(ctx->state == P25_SM_TUNED && ctx->vc_is_tdma, "seed follows TDMA");
    g_state.p25_p2_audio_allowed[0] = 1;
    g_state.voice_counter[0] = 1;
    for (int i = 0; i < 160; ++i) {
        g_state.s_l4[0][i] = 1000;
    }
    g_state.ui_msg[0] = '\0';
    return rc;
}

static int
setup(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    DSD_MEMSET(&g_flush, 0, sizeof(g_flush));
    dsd_atomic_u64_init(&g_flush.drain_done, 0U);
    dsd_mutex_init(&g_flush.mutex);
    dsd_cond_init(&g_flush.condition);
    initOpts(&g_opts);
    initState(&g_state);
    g_opts.verbose = 0;
    g_opts.audio_in_type = AUDIO_IN_NULL;
    g_opts.use_rigctl = 1;
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    dsd_trunk_tuning_requests_reset();
    dsd_engine_frame_sync_hooks_install();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = vc_tune, .tune_to_cc_request = cc_tune, .return_to_cc_request = return_to_cc});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = current_frequency});
    dsd_app_frontend_runtime_start(&g_opts, &g_state);
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){.p25p2_flush_partial_audio = barrier_flush});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = audio_sink});
    return seed_call();
}

static void
cleanup(dsd_scan_row_profile* row) {
    dsd_scan_groups_leave(&g_state);
    dsd_tg_policy_release(row->groups);
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_app_frontend_runtime_stop();
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_trunk_tuning_requests_reset();
    freeState(&g_state);
    dsd_cond_destroy(&g_flush.condition);
    dsd_mutex_destroy(&g_flush.mutex);
}

static int
prepare_policy(dsd_scan_row_profile* row, policy_view* baseline) {
    int rc = 0;
    /* Install blocking policies after grant admission but before thread start. */
    if (g_phase == 2 || g_phase == 4 || g_phase == 5) {
        rc |= expect(dsd_tg_policy_set_mode(&g_state, 1234, 1234, "B") == 0, "seed blocked baseline");
    } else if (g_phase == 7) {
        rc |= expect(dsd_tg_policy_set_mode(&g_state, 1234, 1234, "A") == 0, "seed editable row");
    }
    *baseline = read_policy(&g_opts, &g_state);
    if (g_phase >= 3 && g_phase <= 5) {
        row->values.present = DSD_SCAN_OPT_GROUP;
        rc |= expect(dsd_tg_policy_load(g_allow_csv, &row->groups) == 0, "load row policy");
        rc |= expect(dsd_scan_groups_begin(&g_state) == 0, "reserve row scope");
        dsd_scan_groups_enter(&g_state, row);
        rc |= expect(dsd_scan_groups_row_active(&g_state), "row scope active");
    }
    return rc;
}

static int
submit_phase(void) {
    switch (g_phase) {
        case 1:
        case 3: return dsd_app_command_set_string(DSD_APP_CMD_IMPORT_GROUP_LIST, g_block_csv);
        case 2: return dsd_app_command_submit(DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR, NULL, 0);
        case 4: return dsd_app_command_submit(DSD_APP_CMD_TUNER_RELEASE, NULL, 0);
        case 5: return dsd_app_command_submit(DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR, NULL, 0);
        case 6: {
            dsdneoUserConfig cfg = {0};
            cfg.has_trunking = 1;
            cfg.trunk_enabled = 1;
            cfg.trunk_tune_group_calls = 1;
            DSD_SNPRINTF(cfg.trunk_group_csv, sizeof(cfg.trunk_group_csv), "%s", g_block_csv);
            return dsd_app_command_apply_config(&cfg);
        }
        case 7: {
            const dsd_app_tg_listen_payload edit = {1234, 1234, 0};
            return dsd_app_command_set_tg_listen(&edit);
        }
        case 8: return dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, 0);
        default: return -1;
    }
}

static int
drain_during_flush(void) {
    g_state.p25_sm_force_release = 1;
    p25_sm_watchdog_start(&g_opts, &g_state);
    dsd_mutex_lock(&g_flush.mutex);
    while (!g_flush.entered) {
        if (dsd_cond_timedwait(&g_flush.condition, &g_flush.mutex, 5000) != 0) {
            break;
        }
    }
    const int entered = g_flush.entered;
    dsd_mutex_unlock(&g_flush.mutex);
    int rc = expect(entered, "real watchdog reached flush");
    if (!entered) {
        release_barrier();
        p25_sm_watchdog_stop();
        return rc;
    }
    g_flush.baseline = dsd_app_command_test_policy_guard_waits();
    rc |= expect(submit_phase() > 0, "command queued");
    dsd_thread_t releaser;
    const int started = dsd_thread_create(&releaser, resume_flush, NULL) == 0;
    rc |= expect(started, "releaser started");
    if (!started) {
        release_barrier();
    }
    const int drained = dsd_app_drain_cmds(&g_opts, &g_state);
    dsd_atomic_u64_store_relaxed(&g_flush.drain_done, 1U);
    rc |= expect(drained == 1, "one command drained");
    if (started) {
        dsd_thread_join(releaser);
    }
    p25_sm_watchdog_stop();
    rc |= expect(!g_flush.stalled, "releaser did not reach fixture stall bound");
    rc |= expect(dsd_app_command_test_policy_guard_waits() == g_flush.baseline + 1, "probe == baseline + 1");
    return rc;
}

static int
check_postcondition(policy_view before, policy_view baseline) {
    const policy_view after = read_policy(&g_opts, &g_state);
    switch (g_phase) {
        case 1:
        case 6:
        case 7: return expect(!same_version(before, after) && !after.allowed, "command changed policy to blocked");
        case 2: return expect(after.allowed, "clear allows target");
        case 3: {
            int rc = expect(same_version(before, after) && before.allowed == after.allowed
                                && dsd_scan_groups_row_active(&g_state),
                            "scoped import retains row identity, version and verdict");
            dsd_scan_groups_leave(&g_state);
            rc |= expect(!read_policy(&g_opts, &g_state).allowed, "leaving row exposes imported blocked baseline");
            return rc;
        }
        case 4:
        case 5: {
            int rc = expect(same_version(baseline, after) && !after.allowed && !dsd_scan_groups_row_active(&g_state),
                            "row leave restores blocked baseline");
            if (g_phase == 4) {
                rc |= expect(g_opts.trunk_enable == 0, "tuner release disables trunking");
            }
            return rc;
        }
        case 8:
            return expect(!dsd_tg_policy_session_avoid_contains(&g_state, 1234) && g_state.ui_msg[0] == '\0',
                          "lockout after release adds no avoid and emits no toast");
        default: return 1;
    }
}

static int
run_phase(void) {
    dsd_scan_row_profile row = {0};
    policy_view baseline;
    int rc = setup();
    rc |= prepare_policy(&row, &baseline);
    const policy_view before = read_policy(&g_opts, &g_state);
    if (!rc) {
        rc |= drain_during_flush();
        rc |= expect(same_version(before, g_flush.observed), "flush saw the pre-command store");
        rc |= expect(before.allowed == g_flush.observed.allowed, "flush saw the pre-command verdict");
        rc |= expect((g_flush.audio_blocks > 0) == before.allowed, "audio blocks played iff pre-command allowed");
        rc |= expect(g_flush.flushes == 1 && !g_flush.failures, "flushes == 1 && !failures");
        rc |= check_postcondition(before, baseline);
    }
    cleanup(&row);
    return rc;
}

static int
write_csv(char* path, const char* content) {
    const int fd = dsd_test_mkstemp(path, DSD_TEST_PATH_MAX, "dsd_policy_guard");
    if (fd < 0) {
        return 1;
    }
    dsd_close(fd);
    FILE* file = dsd_fopen_private(path, "wb");
    if (!file) {
        return 1;
    }
    const size_t length = strlen(content);
    const int failed = fwrite(content, 1, length, file) != length;
    return fclose(file) != 0 || failed;
}

int
main(void) {
    dsd_neo_config_init();
    int rc = write_csv(g_block_csv, "id,mode,name\n1234,B,Blocked\n");
    rc |= write_csv(g_allow_csv, "id,mode,name\n1234,A,Row\n");
    if (!rc) {
        for (g_phase = 1; g_phase <= 8; ++g_phase) {
            rc |= run_phase();
        }
    }
    (void)remove(g_block_csv);
    (void)remove(g_allow_csv);
    return rc;
}
