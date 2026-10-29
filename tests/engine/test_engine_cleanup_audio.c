// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <dsd-neo/runtime/config.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/platform/audio_stream_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    // dsd_state is multi-megabyte; keep it off the function stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Engine stop is a boundary the received tone (issue #522) must not cross: a locked tone and
 * the detector session behind it are gone after cleanup, and the publication's generation
 * moves on so a frontend can tell the next session's first verdict from the old one.
 */
static int
test_cleanup_forgets_received_tone(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->rtl_pwr = 1.0;
    opts->rtl_squelch_level = 0.0;
    double phase = 0.0;
    float block[960];
    for (int b = 0; b < 30; b++) {
        for (int i = 0; i < 960; i++) {
            block[i] = (float)(3000.0 * cos(phase));
            phase += 2.0 * M_PI * 100.0 / 48000.0;
        }
        dsd_analog_rx_tap(opts, state, block, 960U);
    }
    rc |= expect_true("rx-tone-locked-before-cleanup", state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                                           && state->analog_rx.ctcss_tenths_hz == 1000);
    rc |= expect_true("rx-tone-session-before-cleanup",
                      dsd_state_ext_get_const(state, DSD_STATE_EXT_DSP_ANALOG_RX) != NULL);
    const uint32_t generation = state->analog_rx.generation;

    dsd_engine_cleanup(opts, state);

    rc |= expect_true("rx-tone-cleared-by-cleanup", state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE
                                                        && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE
                                                        && state->analog_rx.ctcss_tenths_hz == 0
                                                        && state->analog_rx.carrier_open == 0);
    rc |= expect_true("rx-tone-generation-moves-on-cleanup", state->analog_rx.generation != generation);
    rc |= expect_true("rx-tone-session-freed-by-cleanup",
                      dsd_state_ext_get_const(state, DSD_STATE_EXT_DSP_ANALOG_RX) == NULL);
    return rc;
}

/*
 * Issue #671 (B25): the exit summary of NXDN channels missing from the channel map. A running trunk scan logs one per
 * target from the targets' own ledgers as it shuts down, so the engine skips the live one; a paused scan's live ledger
 * is the manual session's own, which no target holds, so the engine logs it.
 */
static void
free_runtime_quietly(dsd_opts* opts, dsd_state* state) {
    freeState(state);
    free(state);
    free(opts);
}

static int
cleanup_logs_session_nxdn_summary(int paused, int* out_logged) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_paused = paused ? 1U : 0U;
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "manual-session.csv");
    (void)nxdn_trunk_diag_note_missing_channel(state, 9);
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "cleanupnxdn") != 0) {
        freeState(state);
        free(state);
        free(opts);
        return 1;
    }
    dsd_engine_cleanup(opts, state);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[4096];
    const int read_rc = dsd_test_capture_stderr_read(&cap, log, sizeof log);
    *out_logged = read_rc == 0 && strstr(log, "missing frequency mapping in chan_csv (manual-session.csv)") != NULL;
    freeState(state);
    free(state);
    free(opts);
    return read_rc != 0;
}

static int
test_cleanup_nxdn_summary_under_trunk_scan(void) {
    int rc = 0;
    int logged = -1;
    rc |= cleanup_logs_session_nxdn_summary(0, &logged);
    rc |= expect_true("running-trunk-scan-leaves-the-summary-to-its-targets", logged == 0);
    logged = -1;
    rc |= cleanup_logs_session_nxdn_summary(1, &logged);
    rc |= expect_true("paused-trunk-scan-logs-the-manual-session-summary", logged == 1);
    return rc;
}

/*
 * Issue #671 (D7, Review Focus): the exit autosave of a session whose -Y scan the operator paused, with no decoder tick
 * between the pause and the shutdown. The scan is still the session's, so the save writes scanner = true and the next
 * launch runs it; a running scan saves true and a stopped one false. @p how: 0 running, 1 paused, 2 stopped.
 */
static int
cleanup_autosaves_scanner(int how, int* out_scanner) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    char dir[DSD_TEST_PATH_MAX];
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_cleanup_autosave") == NULL
        || dsd_test_path_join(path, sizeof path, dir, "autosave.ini") != 0) {
        free_runtime_quietly(opts, state);
        return 1;
    }
    opts->audio_out_type = 9;
    opts->scanner_mode = 1;
    state->lcn_freq_count = 3;
    *dsd_state_trunk_lcn_slot(state, 0) = 851375000;
    *dsd_state_trunk_lcn_slot(state, 1) = 852375000;
    *dsd_state_trunk_lcn_slot(state, 2) = 853375000;
    state->lcn_freq_roll = 2;
    if (how == 1) {
        /* What a tuner release runs; the engine's own shutdown leave keeps the pause. */
        (void)dsd_engine_channel_scan_pause(opts, state, 1);
    } else if (how == 2) {
        /* A stop: svc_leave_channel_scan() clears the pause the engine's leave keeps. */
        opts->scanner_mode = 0;
        (void)dsd_engine_channel_scan_leave(opts, state);
    }
    state->config_autosave_enabled = 1;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", path);
    dsd_engine_cleanup(opts, state);
    static dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    const int load_rc = dsd_user_config_load(path, &cfg);
    *out_scanner = load_rc == 0 ? cfg.trunk_scanner : -1;
    const char* files[] = {"autosave.ini", NULL};
    (void)dsd_test_remove_temp_dir(dir, files);
    free_runtime_quietly(opts, state);
    return load_rc != 0;
}

static int
test_cleanup_autosaves_a_paused_scan(void) {
    static const char* const k_tags[] = {
        "running-scan-autosaves-scanner-on",
        "paused-scan-autosaves-scanner-on",
        "stopped-scan-autosaves-scanner-off",
    };
    static const int k_want[] = {1, 1, 0};
    int rc = 0;
    for (int how = 0; how < 3; how++) {
        int scanner = -1;
        rc |= cleanup_autosaves_scanner(how, &scanner);
        if (scanner != k_want[how]) {
            DSD_FPRINTF(stderr, "%s: wrote %d, want %d\n", k_tags[how], scanner, k_want[how]);
            rc |= 1;
        }
    }
    return rc;
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

    opts->audio_out_stream = (dsd_audio_stream*)calloc(1, sizeof(*opts->audio_out_stream));
    opts->audio_out_streamR = (dsd_audio_stream*)calloc(1, sizeof(*opts->audio_out_streamR));
    opts->audio_raw_out = (dsd_audio_stream*)calloc(1, sizeof(*opts->audio_raw_out));
    opts->audio_in_stream = (dsd_audio_stream*)calloc(1, sizeof(*opts->audio_in_stream));
    // File input metadata outlives its libsndfile handle when playback reaches EOF.
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));

    dsd_engine_cleanup(opts, state);

    rc |= expect_true("audio_out_stream-null", opts->audio_out_stream == NULL);
    rc |= expect_true("audio_out_streamR-null", opts->audio_out_streamR == NULL);
    rc |= expect_true("audio_raw_out-null", opts->audio_raw_out == NULL);
    rc |= expect_true("audio_in_stream-null", opts->audio_in_stream == NULL);
    rc |= expect_true("audio_in_file_info-null", opts->audio_in_file_info == NULL);

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

    // Test 4: cleanup forgets the received tone and frees its detector.
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    rc |= test_cleanup_forgets_received_tone(opts, state);
    free_test_runtime(opts, state);

    // Test 5: the NXDN exit summary of a paused trunk scan's manual session (issue #671, B25).
    rc |= test_cleanup_nxdn_summary_under_trunk_scan();

    // Test 6: the exit autosave right after a -Y pause (issue #671, D7).
    rc |= test_cleanup_autosaves_a_paused_scan();

    if (rc == 0) {
        printf("ENGINE_CLEANUP_AUDIO: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
