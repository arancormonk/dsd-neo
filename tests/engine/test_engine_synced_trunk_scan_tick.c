// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
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

int
ui_start(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    return 0;
}

void
ui_stop(void) { // NOLINT(misc-use-internal-linkage)
}

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
    g_process_frame_calls++;
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_get_frame_sync_calls++;
    if (g_get_frame_sync_calls <= 3) {
        return DSD_SYNC_P25P1_POS;
    }
    exitflag = 1;
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

    int rc = dsd_engine_run(opts, state);
    dsd_trunk_scan_hooks_set((dsd_trunk_scan_hooks){0});

    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "engine run failed rc=%d\n", rc);
        test_rc = 1;
    }
    if (g_process_frame_calls == 0) {
        DSD_FPRINTF(stderr, "stubbed synced frames were not processed\n");
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
