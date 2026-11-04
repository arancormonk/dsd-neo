// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief The run's FTZ/DAZ mode on the threads an I/Q replay decode starts (issue #672 B12).
 *
 * Runs the real engine three times on one thread, as an embedding host does, on the arguments dsd-neo would get
 * (argv[1..]: the decoder flags, `--iq-replay FIXTURE` and the rest): with DSD_NEO_FTZ_DAZ=1, then =0, then unset.
 * Before the first two runs the calling thread is put in the opposite mode, so a thread that only inherited it would
 * report the wrong one. Every thread the decode starts (the decoder itself, the replay feeder, the demod) must report
 * the run's mode at its entry, and the host thread must have its own mode back when each run returns, so the unset run
 * after an "on" one leaves every thread off. Prints "FP MODE REPLAY: OK" on success.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_env.h"

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

#define FP_SEEN_MAX 64

static struct {
    dsd_mutex_t lock;
    int count;
    char tag[FP_SEEN_MAX][24];
    unsigned int bits[FP_SEEN_MAX];
} g_seen;

static void
observe(const char* thread_tag, unsigned int csr) {
    dsd_mutex_lock(&g_seen.lock);
    if (g_seen.count < FP_SEEN_MAX) {
        DSD_SNPRINTF(g_seen.tag[g_seen.count], sizeof g_seen.tag[0], "%s", thread_tag);
        g_seen.bits[g_seen.count] = csr & DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS;
        g_seen.count++;
    }
    dsd_mutex_unlock(&g_seen.lock);
}

static void
set_caller_bits(unsigned int bits) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr((_mm_getcsr() & ~DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS) | bits);
#else
    (void)bits;
#endif
}

/* 0 when every apply seen on @p tag (at least one) left @p want_bits. */
static int
check_tag(const char* tag, unsigned int want_bits) {
    int seen = 0;
    int wrong = 0;
    for (int i = 0; i < g_seen.count; i++) {
        if (strcmp(g_seen.tag[i], tag) == 0) {
            seen++;
            wrong += (g_seen.bits[i] != want_bits) ? 1 : 0;
        }
    }
#if !DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    wrong = 0;
#endif
    if (seen == 0 || wrong != 0) {
        DSD_FPRINTF(stderr, "FP MODE REPLAY FAIL: %s seen %d times, %d with FTZ/DAZ other than 0x%x\n", tag, seen,
                    wrong, want_bits);
        return 1;
    }
    return 0;
}

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
static unsigned int
caller_bits_now(void) {
    return _mm_getcsr() & DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS;
}
#endif

/* Passed as caller_bits: start the run from whatever mode the host thread has. */
#define KEEP_CALLER_BITS (~0U)

/* One run with DSD_NEO_FTZ_DAZ=@p env_value (NULL: unset), from a host thread put in @p caller_bits first. */
static int
run_once(int argc, char** argv, const char* env_value, unsigned int caller_bits, unsigned int want_bits) {
    if ((env_value ? dsd_test_setenv("DSD_NEO_FTZ_DAZ", env_value, 1) : dsd_test_unsetenv("DSD_NEO_FTZ_DAZ")) != 0) {
        return 1;
    }
    g_seen.count = 0;
    if (caller_bits != KEEP_CALLER_BITS) {
        set_caller_bits(caller_bits);
    }
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const unsigned int host_bits = caller_bits_now();
#endif
    dsd_exitflag_store(0);
    /* Each run parses its own copy: the bootstrap rearranges the vector it is handed. */
    char** args = (char**)calloc((size_t)argc + 1U, sizeof(*args));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    int rc = 1;
    if (args != NULL && opts != NULL && state != NULL) {
        for (int i = 0; i < argc; i++) {
            args[i] = argv[i];
        }
        initOpts(opts);
        initState(state);
        if (dsd_runtime_bootstrap(argc, args, opts, state, NULL, &rc) == DSD_BOOTSTRAP_CONTINUE) {
            rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
        }
        freeState(state);
    }
    free(state);
    free(opts);
    free((void*)args);
    const char* shown = env_value ? env_value : "(unset)";
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FP MODE REPLAY FAIL: run with DSD_NEO_FTZ_DAZ=%s returned %d\n", shown, rc);
        return 1;
    }
    static const char* const kTags[] = {"decoder", "iq_replay", "rtl_demod"};
    int failed = 0;
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    if (caller_bits_now() != host_bits) {
        DSD_FPRINTF(stderr,
                    "FP MODE REPLAY FAIL: DSD_NEO_FTZ_DAZ=%s left the host thread's FTZ/DAZ at 0x%x, not 0x%x\n", shown,
                    caller_bits_now(), host_bits);
        failed = 1;
    }
#endif
    for (size_t i = 0; i < sizeof kTags / sizeof kTags[0]; i++) {
        failed |= check_tag(kTags[i], want_bits);
    }
    return failed;
}

int
main(int argc, char** argv) {
    if (argc < 2 || dsd_mutex_init(&g_seen.lock) != 0) {
        return 2;
    }
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const unsigned int original = _mm_getcsr();
#endif
    dsd_runtime_fp_mode_set_observer(observe);
    int failed = run_once(argc, argv, "1", 0U, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    failed |= run_once(argc, argv, "0", DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS, 0U);
    /* On, then unset on the same thread: the unset run starts from the host's own mode (off), not the last run's. */
    failed |= run_once(argc, argv, "1", 0U, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    failed |= run_once(argc, argv, NULL, KEEP_CALLER_BITS, 0U);
    dsd_runtime_fp_mode_set_observer(NULL);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr(original);
#endif
    dsd_mutex_destroy(&g_seen.lock);
    if (failed) {
        return 1;
    }
    (void)fputs("FP MODE REPLAY: OK\n", stdout);
    return 0;
}
