// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_process_frame_calls = 0;
static int g_get_frame_sync_calls = 0;
static int g_outer_tick_calls = 0;
static int g_synced_tick_calls = 0;
static int g_process_frame_guard_failures = 0;
static uint64_t g_pending_tune_request = 0U;
static uint64_t g_failed_tune_request = 0U;
static int g_failed_gate_seen = 0;

void
printFrameInfo(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

void
processFrame(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    if (p25_sm_tick_guard_try_enter()) {
        g_process_frame_guard_failures++;
        p25_sm_tick_guard_leave();
    }
    g_process_frame_calls++;
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_get_frame_sync_calls++;
    if (g_get_frame_sync_calls == 1) {
        // Frames assembled before and during an asynchronous retune must stay
        // blocked even though their completed generation still compares equal.
        g_pending_tune_request = dsd_trunk_tuning_request_begin();
    } else if (g_get_frame_sync_calls == 3) {
        // Completion while a frame is being assembled advances the generation;
        // that frame is stale too. Only the next freshly collected frame is valid.
        dsd_trunk_tuning_request_complete(g_pending_tune_request, DSD_TRUNK_TUNE_RESULT_OK);
    } else if (g_get_frame_sync_calls == 5) {
        // Start another correlated tune while trunking owns dispatch, then
        // leave it pending across the next continuously synchronized frame.
        opts->p25_trunk = 1;
        opts->trunk_enable = 1;
        g_failed_tune_request = dsd_trunk_tuning_request_begin();
        dsd_trunk_tuning_request_mark_ready(g_failed_tune_request);
    } else if (g_get_frame_sync_calls == 6) {
        // A terminal failure must be retired on this actual mode-exit path.
        // The synchronized loop will not call noCarrier() before dispatching
        // this or the following conventional frame.
        dsd_trunk_tuning_request_publish(g_failed_tune_request, DSD_TRUNK_TUNE_RESULT_FAILED);
        g_failed_gate_seen = !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation());
        opts->p25_trunk = 0;
        opts->trunk_enable = 0;
    }
    if (g_get_frame_sync_calls <= 7) {
        return DSD_SYNC_P25P1_POS;
    }
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
fake_trunk_scan_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    if (g_process_frame_calls > 0) {
        g_synced_tick_calls++;
    } else {
        g_outer_tick_calls++;
    }
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state) {
        freeState(state);
    }
    free(state);
    free(opts);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free_test_runtime(opts, state);
        return 1;
    }

    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;

    dsd_trunk_scan_hooks hooks = {0};
    hooks.tick = fake_trunk_scan_tick;
    dsd_trunk_scan_hooks_set(hooks);

    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    dsd_trunk_scan_hooks_set((dsd_trunk_scan_hooks){0});

    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "engine run failed rc=%d\n", rc);
        test_rc = 1;
    }
    if (g_process_frame_calls != 3) {
        DSD_FPRINTF(stderr, "pending/stale tune-generation frames were not discarded calls=%d\n",
                    g_process_frame_calls);
        test_rc = 1;
    }
    if (!g_failed_gate_seen || g_failed_tune_request == 0U || dsd_trunk_tuning_pending_request() != 0U
        || !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation())) {
        DSD_FPRINTF(stderr, "inactive synchronized mode did not retire the terminal failed tune gate\n");
        test_rc = 1;
    }
    if (g_process_frame_guard_failures != 0) {
        DSD_FPRINTF(stderr, "frame processing did not own the SM/scan guard failures=%d\n",
                    g_process_frame_guard_failures);
        test_rc = 1;
    }
    if (g_outer_tick_calls == 0) {
        DSD_FPRINTF(stderr, "outer trunk-scan tick hook was not called\n");
        test_rc = 1;
    }
    if (g_synced_tick_calls == 0) {
        DSD_FPRINTF(stderr, "synced-frame trunk-scan tick hook was not called\n");
        test_rc = 1;
    }

    free_test_runtime(opts, state);
    if (test_rc == 0) {
        printf("ENGINE_SYNCED_TRUNK_SCAN_TICK: OK\n");
    }
    return test_rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
