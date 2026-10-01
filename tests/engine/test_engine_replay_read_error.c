// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief An I/Q replay whose capture cannot be read, run through the engine (issue #572).
 *
 * The real engine runs a fast replay the way the CLI does. The capture read (dsd_iq_replay_read(), wrapped with
 * GNU ld --wrap) fails after a few chunks. The replay has to end by itself, within a bound, with exit code 1 and the
 * read error reported as a file input failure. A watchdog raises the global exit flag if the engine is still running
 * after the bound, so a replay that hangs fails the test instead of the ctest timeout.
 *
 * With the argument "truncate" the wrapper fails no read itself: it cuts the data file to the chunks already read,
 * under the open replay source, before the next real read, as a capture file that shrinks during its replay. The real
 * read has to report that as the same read error.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

enum {
    kReplayChunkBytes = 65536, /* one replay reader read */
    kChunks = 8,
    kReadsBeforeError = 3,
};

static const unsigned int kWatchdogMs = 20000U;

static atomic_int g_reads;
static atomic_int g_engine_done;
static atomic_int g_watchdog_fired;

/* The capture's payload, and the data file it is written to. */
static uint8_t g_payload[kChunks * kReplayChunkBytes];
static char g_data_path[DSD_TEST_PATH_MAX];
/* "truncate": cut the data file instead of failing the read. */
static int g_truncate_mode;
static atomic_int g_cut_ok;

/* Write the data file again with only the chunks already read. */
static int
cut_data_file(void) {
    FILE* fp = dsd_fopen_private(g_data_path, "wb");
    if (!fp) {
        return -1;
    }
    const size_t keep = (size_t)kReadsBeforeError * (size_t)kReplayChunkBytes;
    const size_t wrote = fwrite(g_payload, 1, keep, fp);
    const int close_rc = fclose(fp);
    return (wrote == keep && close_rc == 0) ? 0 : -1;
}

// GNU ld --wrap entry points must keep the reserved __wrap_*/__real_* symbol names.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_iq_replay_read(dsd_iq_replay_source* src, void* out, size_t max_bytes, size_t* out_bytes);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_dsd_iq_replay_read(dsd_iq_replay_source* src, void* out, size_t max_bytes, size_t* out_bytes);

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_iq_replay_read(dsd_iq_replay_source* src, void* out, size_t max_bytes, size_t* out_bytes) {
    const int read_index = atomic_fetch_add(&g_reads, 1);
    if (read_index >= kReadsBeforeError) {
        if (g_truncate_mode) {
            if (read_index == kReadsBeforeError) {
                atomic_store(&g_cut_ok, cut_data_file() == 0);
            }
            return __real_dsd_iq_replay_read(src, out, max_bytes, out_bytes);
        }
        if (out_bytes) {
            *out_bytes = 0U;
        }
        return DSD_IQ_ERR_IO;
    }
    return __real_dsd_iq_replay_read(src, out, max_bytes, out_bytes);
}

static DSD_THREAD_RETURN_TYPE
watchdog_fn(void* arg) {
    (void)arg;
    for (unsigned int waited = 0U; waited < kWatchdogMs; waited += 10U) {
        if (atomic_load(&g_engine_done)) {
            DSD_THREAD_RETURN;
        }
        dsd_sleep_ms(10U);
    }
    if (!atomic_load(&g_engine_done)) {
        atomic_store(&g_watchdog_fired, 1);
        DSD_FPRINTF(stderr, "FAIL: the replay was still running %u ms after it started; raising the exit flag\n",
                    kWatchdogMs);
        dsd_exitflag_store(1);
    }
    DSD_THREAD_RETURN;
}

static int
write_fixture(const char* dir, char* out_metadata_path, size_t out_metadata_path_size) {
    if (dsd_test_path_join(g_data_path, sizeof(g_data_path), dir, "read_error.iq") != 0
        || dsd_test_path_join(out_metadata_path, out_metadata_path_size, dir, "read_error.iq.json") != 0) {
        return -1;
    }

    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", g_data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", out_metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 1536000U;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 48000U;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");

    dsd_iq_capture_writer* writer = NULL;
    char err[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open the capture writer: %s\n", err[0] ? err : "unknown");
        return -1;
    }
    for (size_t i = 0; i < sizeof(g_payload); i++) {
        g_payload[i] = (uint8_t)(96U + ((i * 17U) % 65U));
    }
    if (dsd_iq_capture_submit(writer, g_payload, sizeof(g_payload)) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return -1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

int
main(int test_argc, char** test_argv) {
    g_truncate_mode = test_argc > 1 && strcmp(test_argv[1], "truncate") == 0;
    const char* const test_name = g_truncate_mode ? "ENGINE_REPLAY_TRUNCATED_CAPTURE" : "ENGINE_REPLAY_READ_ERROR";
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_replay_read_error")) {
        DSD_FPRINTF(stderr, "FAIL: could not create the fixture directory\n");
        return 1;
    }
    const char* const files[] = {"read_error.iq", "read_error.iq.json", NULL};
    char metadata_path[DSD_TEST_PATH_MAX];
    if (write_fixture(dir, metadata_path, sizeof(metadata_path)) != 0) {
        (void)dsd_test_remove_temp_dir(dir, files);
        return 1;
    }

    /* The arguments of a DECODE_IQ_* case (iq_decode_check.cmake), writable because the CLI parser compacts them. */
    char arg0[] = "dsd-neo";
    char arg_frontend[] = "--frontend";
    char arg_none[] = "none";
    char arg_mode[] = "-fa";
    char arg_replay[] = "--iq-replay";
    char arg_out[] = "-o";
    char arg_null[] = "null";
    char* argv[] = {arg0, arg_frontend, arg_none, arg_mode, arg_replay, metadata_path, arg_out, arg_null, NULL};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    int rc = 0;
    int engine_rc = -1;
    uint64_t elapsed_ms = 0U;
    if (!opts || !state) {
        rc = 1;
    } else {
        initOpts(opts);
        initState(state);
        dsd_thread_t watchdog;
        int watchdog_started = dsd_thread_create(&watchdog, watchdog_fn, NULL) == 0;
        rc |= expect_true("watchdog started", watchdog_started);
        const uint64_t start_ns = dsd_time_monotonic_ns();
        int boot_rc = 1;
        if (dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &boot_rc) == DSD_BOOTSTRAP_CONTINUE) {
            engine_rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
        } else {
            DSD_FPRINTF(stderr, "FAIL: bootstrap did not continue (rc %d)\n", boot_rc);
            rc = 1;
        }
        elapsed_ms = (dsd_time_monotonic_ns() - start_ns) / 1000000ULL;
        atomic_store(&g_engine_done, 1);
        if (watchdog_started) {
            (void)dsd_thread_join(watchdog);
        }
        freeState(state);
    }

    dsd_input_failure failure;
    dsd_input_failure_get(&failure);
    rc |= expect_true("the replay ended by itself, before the watchdog", !atomic_load(&g_watchdog_fired));
    rc |= expect_true("the read error was reached", atomic_load(&g_reads) > kReadsBeforeError);
    if (g_truncate_mode) {
        rc |= expect_true("the data file was cut under the replay", atomic_load(&g_cut_ok));
    }
    if (engine_rc != 1) {
        DSD_FPRINTF(stderr, "FAIL: the engine returned %d for a replay %s (want 1)\n", engine_rc,
                    g_truncate_mode ? "whose data file was cut short" : "read error");
        rc = 1;
    }
    rc |= expect_true("the read error was reported as a file input failure",
                      failure.kind == DSD_INPUT_FAILURE_FILE && failure.native_code == DSD_IQ_ERR_IO);
    if (rc == 0) {
        DSD_FPRINTF(stderr, "%s: OK (engine rc %d after %llu ms)\n", test_name, engine_rc,
                    (unsigned long long)elapsed_ms);
    }

    free(state);
    free(opts);
    if (dsd_test_remove_temp_dir(dir, files) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not remove the fixture directory %s\n", dir);
        rc = 1;
    }
    return rc ? 1 : 0;
}
