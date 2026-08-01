// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runs the full bootstrap/engine lifecycle twice in one process.
 *
 * Hosts that embed the decoder (an Android foreground service, a GUI shell)
 * stop and restart the engine inside a single process and re-run CLI parsing on
 * every start. That shape is not covered by ENGINE_RUN_SETUP, which stubs the
 * RTL stream out, so the radio-path statics stay unproven. This test drives the
 * real replay path — the same fixture DECODE_IQ_P25P1_C4FM_CC uses — and
 * asserts that the second run decodes as well as the first.
 *
 * It also exercises DSD_NEO_NO_SIGNAL_HANDLERS: an embedded decoder must not
 * take over the process signal dispositions.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR must be defined by the build"
#endif

#define EXPECTED_PAYLOAD "NAC/CC: 140"
#define RUN_COUNT        2

/* Captures stdout and stderr into one temporary file for the duration of a run. */
typedef struct {
    FILE* sink;
    int saved_stdout;
    int saved_stderr;
} output_capture;

static void
capture_init(output_capture* cap) {
    cap->sink = NULL;
    cap->saved_stdout = -1;
    cap->saved_stderr = -1;
}

static int
capture_begin(output_capture* cap) {
    cap->sink = tmpfile();
    if (!cap->sink) {
        DSD_FPRINTF(stderr, "FAIL: tmpfile() failed\n");
        return -1;
    }

    cap->saved_stdout = dsd_dup(dsd_fileno(stdout));
    cap->saved_stderr = dsd_dup(dsd_fileno(stderr));
    if (cap->saved_stdout < 0 || cap->saved_stderr < 0) {
        DSD_FPRINTF(stderr, "FAIL: dup of stdout/stderr failed\n");
        return -1;
    }

    (void)fflush(stdout);
    (void)fflush(stderr);

    if (dsd_dup2(dsd_fileno(cap->sink), dsd_fileno(stdout)) < 0
        || dsd_dup2(dsd_fileno(cap->sink), dsd_fileno(stderr)) < 0) {
        return -1;
    }
    return 0;
}

static void
capture_end(output_capture* cap) {
    /* Redirected stdout is a file, so stdio buffers it fully; an unflushed tail
     * would otherwise be lost when the descriptor is restored. */
    (void)fflush(stdout);
    (void)fflush(stderr);

    if (cap->saved_stdout >= 0) {
        (void)dsd_dup2(cap->saved_stdout, dsd_fileno(stdout));
        (void)dsd_close(cap->saved_stdout);
        cap->saved_stdout = -1;
    }
    if (cap->saved_stderr >= 0) {
        (void)dsd_dup2(cap->saved_stderr, dsd_fileno(stderr));
        (void)dsd_close(cap->saved_stderr);
        cap->saved_stderr = -1;
    }
}

static void
capture_destroy(output_capture* cap) {
    if (cap->sink) {
        (void)fclose(cap->sink);
        cap->sink = NULL;
    }
}

/* Chunked scan with overlap: decode output easily outgrows any single buffer. */
static int
capture_contains(output_capture* cap, const char* needle) {
    const size_t needle_len = strlen(needle);
    char buf[8192];

    if (!cap->sink || needle_len == 0 || needle_len >= sizeof buf) {
        return 0;
    }
    if (fseek(cap->sink, 0, SEEK_SET) != 0) {
        return 0;
    }

    const size_t overlap = needle_len - 1;
    size_t carried = 0;
    for (;;) {
        size_t n = fread(buf + carried, 1, sizeof buf - carried - 1, cap->sink);
        if (n == 0) {
            return 0;
        }
        buf[carried + n] = '\0';
        if (strstr(buf, needle)) {
            return 1;
        }
        if (carried + n <= overlap) {
            carried += n;
            continue;
        }
        /* Keep the trailing bytes so a needle straddling two reads still matches. */
        DSD_MEMMOVE(buf, buf + (carried + n - overlap), overlap);
        carried = overlap;
    }
}

static void
capture_dump(output_capture* cap) {
    char buf[4096];

    if (!cap->sink || fseek(cap->sink, 0, SEEK_SET) != 0) {
        return;
    }
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf - 1, cap->sink);
        if (n == 0) {
            break;
        }
        buf[n] = '\0';
        DSD_FPRINTF(stderr, "%s", buf);
    }
}

/* Mirrors apps/dsd-cli/main.c: allocate, init, bootstrap, run, free. */
static int
run_decode_once(const char* fixture_path, output_capture* cap) {
    char arg_prog[] = "dsd-neo";
    char arg_frontend[] = "--frontend";
    char arg_none[] = "none";
    char arg_mode[] = "-f1";
    char arg_replay[] = "--iq-replay";
    char arg_out[] = "-o";
    char arg_null[] = "null";
    char arg_fixture[1024];

    DSD_STRNCPY(arg_fixture, fixture_path, sizeof(arg_fixture) - 1);
    arg_fixture[sizeof(arg_fixture) - 1] = '\0';

    char* argv[] = {arg_prog, arg_frontend, arg_none, arg_mode, arg_replay, arg_fixture, arg_out, arg_null};
    const int argc = (int)(sizeof argv / sizeof argv[0]);

    dsd_opts* opts = calloc(1, sizeof(dsd_opts));
    dsd_state* state = calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "FAIL: opts/state allocation failed\n");
        free(opts);
        free(state);
        return -1;
    }

    initOpts(opts);
    initState(state);

    int rc = -1;
    if (capture_begin(cap) == 0) {
        int exit_rc = 1;
        int bootstrap_rc = dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &exit_rc);
        if (bootstrap_rc != DSD_BOOTSTRAP_CONTINUE) {
            capture_end(cap);
            DSD_FPRINTF(stderr, "FAIL: bootstrap returned %d (exit_rc=%d), expected DSD_BOOTSTRAP_CONTINUE\n",
                        bootstrap_rc, exit_rc);
        } else {
            int run_rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
            capture_end(cap);
            if (run_rc != 0) {
                DSD_FPRINTF(stderr, "FAIL: engine run returned %d, expected 0\n", run_rc);
            } else {
                rc = 0;
            }
        }
    }

    freeState(state);
    free(opts);
    free(state);
    return rc;
}

int
main(void) {
    char fixture_path[1024];
    int rc = 0;

    /* The embedding host runs this way, so the restart path must too. */
#if defined(_WIN32)
    if (_putenv_s("DSD_NEO_NO_SIGNAL_HANDLERS", "1") != 0) {
#else
    if (setenv("DSD_NEO_NO_SIGNAL_HANDLERS", "1", 1) != 0) {
#endif
        DSD_FPRINTF(stderr, "FAIL: could not set DSD_NEO_NO_SIGNAL_HANDLERS\n");
        return 1;
    }

    DSD_SNPRINTF(fixture_path, sizeof fixture_path, "%s/p25p1_c4fm_cc.iq.json", DSD_NEO_TEST_IQ_FIXTURE_DIR);

    for (int iteration = 1; iteration <= RUN_COUNT; iteration++) {
        output_capture cap;
        capture_init(&cap);

        /* A completed run leaves the shutdown flag raised; an embedding host
         * clears it before restarting, exactly as done here. */
        dsd_exitflag_store(0);

        int run_rc = run_decode_once(fixture_path, &cap);
        if (run_rc != 0) {
            DSD_FPRINTF(stderr, "FAIL: run %d did not complete cleanly\n", iteration);
            rc = 1;
        } else if (!capture_contains(&cap, EXPECTED_PAYLOAD)) {
            DSD_FPRINTF(stderr, "FAIL: run %d did not decode \"%s\"; captured output follows:\n", iteration,
                        EXPECTED_PAYLOAD);
            capture_dump(&cap);
            rc = 1;
        } else {
            printf("ENGINE_RESTART: run %d decoded \"%s\"\n", iteration, EXPECTED_PAYLOAD);
        }

        capture_destroy(&cap);
        if (rc != 0) {
            break;
        }
    }

    if (rc == 0) {
        printf("ENGINE_RESTART: OK\n");
    }
    return rc;
}
