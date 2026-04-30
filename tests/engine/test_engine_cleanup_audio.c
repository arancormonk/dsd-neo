// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#undef DSD_NEO_MAIN

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/audio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <stdio.h>
#include <stdlib.h>

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

int
ui_start(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

void
ui_stop(void) {}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    // dsd_state is multi-megabyte; keep it off the function stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        fprintf(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);

    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state != NULL) {
        freeState(state);
    }
    free(state);
    free(opts);
}

int
main(void) {
    int rc = 0;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;

    // Test 1: audio stream pointers are NULL after cleanup.
    // Allocate zeroed blocks to simulate open audio streams.
    // dsd_audio_close() checks stream->handle before calling backend
    // functions; a calloc'd block has handle==NULL so close() just free()s it.
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->audio_out_stream = (dsd_audio_stream*)calloc(1, 64);
    opts->audio_out_streamR = (dsd_audio_stream*)calloc(1, 64);
    opts->audio_raw_out = (dsd_audio_stream*)calloc(1, 64);
    opts->audio_in_stream = (dsd_audio_stream*)calloc(1, 64);

    dsd_engine_cleanup(opts, state);

    rc |= expect_true("audio_out_stream-null", opts->audio_out_stream == NULL);
    rc |= expect_true("audio_out_streamR-null", opts->audio_out_streamR == NULL);
    rc |= expect_true("audio_raw_out-null", opts->audio_raw_out == NULL);
    rc |= expect_true("audio_in_stream-null", opts->audio_in_stream == NULL);

    free_test_runtime(opts, state);
    opts = NULL;
    state = NULL;

    // Test 2: cleanup with all-NULL audio pointers does not crash.
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    dsd_engine_cleanup(opts, state);

    rc |= expect_true("null-safe-audio_out_stream", opts->audio_out_stream == NULL);
    rc |= expect_true("null-safe-audio_out_streamR", opts->audio_out_streamR == NULL);
    rc |= expect_true("null-safe-audio_raw_out", opts->audio_raw_out == NULL);
    rc |= expect_true("null-safe-audio_in_stream", opts->audio_in_stream == NULL);

    free_test_runtime(opts, state);
    opts = NULL;
    state = NULL;

    // Test 3: double cleanup is idempotent (no crash or double-free).
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    dsd_engine_cleanup(opts, state);
    dsd_engine_cleanup(opts, state);

    rc |= expect_true("idempotent-audio_out_stream", opts->audio_out_stream == NULL);
    rc |= expect_true("idempotent-audio_out_streamR", opts->audio_out_streamR == NULL);
    rc |= expect_true("idempotent-audio_raw_out", opts->audio_raw_out == NULL);
    rc |= expect_true("idempotent-audio_in_stream", opts->audio_in_stream == NULL);

    free_test_runtime(opts, state);

    if (rc == 0) {
        printf("ENGINE_CLEANUP_AUDIO: OK\n");
    }
    return rc;
}
