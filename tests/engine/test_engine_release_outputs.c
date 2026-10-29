// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A configured session that never runs leaves no output file open (#782 gate r2 H3).
 *
 * The Android host configures a session (dsd_runtime_bootstrap()), and can then give it up before nativeRun():
 * DecoderService retires a start that a newer one superseded, a configure fails part way, or the service frees the
 * options. The bootstrap has already opened the -c symbol capture, the -w static WAV and the -6 raw WAV by then, and
 * only the run's dsd_engine_cleanup() closes them; -P opens its per-call WAVs only at the run's start (issue #708), so
 * a configure leaves none. This runs the same configure, then dsd_engine_release_outputs() as the host's discard does,
 * and checks every stream is closed: the pointers are cleared, the symbol header reached the file, no per-call WAV is
 * left in the -7 folder and the directory can be removed (which an open file prevents on Windows), and on Linux the
 * process holds no more descriptors than before.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__linux__)
#include <dirent.h>
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

/* The descriptors the process holds, or -1 where that cannot be read. */
static int
open_descriptor_count(void) {
#if defined(__linux__)
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) {
        return -1;
    }
    int count = 0;
    while (readdir(dir) != NULL) {
        count++;
    }
    (void)closedir(dir);
    return count;
#else
    return -1;
#endif
}

static long
file_size(const char* path) {
    FILE* fp = dsd_fopen_private(path, "rb");
    if (!fp) {
        return -1;
    }
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) {
        size = ftell(fp);
    }
    (void)fclose(fp);
    return size;
}

static int
new_session(dsd_opts** opts_out, dsd_state** state_out) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return -1;
    }
    initOpts(opts);
    initState(state);
    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_session(dsd_opts* opts, dsd_state* state) {
    freeState(state);
    free(opts);
    free(state);
}

/* -c, -w and -6: the files the options name, opened by the configure and closed by the release. */
static int
static_outputs_close(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    char symbols[DSD_TEST_PATH_MAX];
    char wav[DSD_TEST_PATH_MAX];
    char raw[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_release_static")
        || dsd_test_path_join(symbols, sizeof symbols, dir, "symbols.bin") != 0
        || dsd_test_path_join(wav, sizeof wav, dir, "static.wav") != 0
        || dsd_test_path_join(raw, sizeof raw, dir, "raw.wav") != 0) {
        return expect_true("temp paths", 0);
    }
    const int before = open_descriptor_count();
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (new_session(&opts, &state) != 0) {
        return expect_true("alloc", 0);
    }
    char* argv[] = {"dsd-neo", "-i", "-", "-c", symbols, "-w", wav, "-6", raw, NULL};
    int exit_rc = 1;
    rc |= expect_true("the static outputs configure",
                      dsd_runtime_bootstrap(9, argv, opts, state, NULL, &exit_rc) == DSD_BOOTSTRAP_CONTINUE);
    rc |= expect_true("the configure opened all three",
                      opts->symbol_out_f != NULL && opts->wav_out_f != NULL && opts->wav_out_raw != NULL);

    dsd_engine_release_outputs(opts, state);
    rc |= expect_true("the release closes them",
                      opts->symbol_out_f == NULL && opts->wav_out_f == NULL && opts->wav_out_raw == NULL);
    rc |= expect_true("the symbol header reached its file", file_size(symbols) == 16);
    dsd_engine_release_outputs(opts, state);
    rc |= expect_true("a second release finds nothing", opts->symbol_out_f == NULL && opts->wav_out_f == NULL);
    free_session(opts, state);
    if (before >= 0) {
        rc |= expect_true("no descriptor stays open", open_descriptor_count() == before);
    }
    static const char* const kFiles[] = {"symbols.bin", "static.wav", "raw.wav", NULL};
    rc |= expect_true("the directory can be removed", dsd_test_remove_temp_dir(dir, kFiles) == 0);
    return rc;
}

/* -P: the configure only selects per-call mode; the slot files open at the run's start, so none is left behind. */
static int
per_call_outputs_close(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_release_per_call")) {
        return expect_true("temp dir", 0);
    }
    const int before = open_descriptor_count();
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (new_session(&opts, &state) != 0) {
        return expect_true("alloc", 0);
    }
    char* argv[] = {"dsd-neo", "-i", "-", "-7", dir, "-P", NULL};
    int exit_rc = 1;
    rc |= expect_true("the per-call outputs configure",
                      dsd_runtime_bootstrap(6, argv, opts, state, NULL, &exit_rc) == DSD_BOOTSTRAP_CONTINUE);
    rc |= expect_true("the configure selects per-call mode", opts->dmr_stereo_wav == 1);
    rc |= expect_true("the configure opens no slot", opts->wav_out_f == NULL && opts->wav_out_fR == NULL);

    dsd_engine_release_outputs(opts, state);
    rc |= expect_true("the release closes both", opts->wav_out_f == NULL && opts->wav_out_fR == NULL);
    free_session(opts, state);
    if (before >= 0) {
        rc |= expect_true("no descriptor stays open", open_descriptor_count() == before);
    }
    static const char* const kNone[] = {NULL};
    rc |= expect_true("no recording is left behind", dsd_test_remove_temp_dir(dir, kNone) == 0);
    return rc;
}

int
main(void) {
    int rc = static_outputs_close();
    rc |= per_call_outputs_close();
    if (rc == 0) {
        printf("ENGINE_RELEASE_OUTPUTS: OK\n");
    }
    return rc;
}
