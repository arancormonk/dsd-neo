// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 watchdog's thread start (issue #672 B15, B12): a thread that cannot start leaves the watchdog off, the stop
 * that follows joins nothing, and the next start creates the thread again. The thread applies the run's FTZ/DAZ mode
 * at its entry, which the test tells apart from inheritance by starting it from a thread in the opposite mode, across
 * an on-to-off restart.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

static int g_creates;
static int g_fail_create;
static atomic_int g_watchdog_entries;
static atomic_int g_watchdog_ftz_daz;

static int
create_seam(dsd_thread_t* thread, dsd_thread_fn entry, void* arg) {
    g_creates++;
    if (g_fail_create) {
        return 11; /* EAGAIN: a thread or ulimit cap */
    }
    return dsd_thread_create(thread, entry, arg);
}

static void
observe(const char* thread_tag, unsigned int csr) {
    if (strcmp(thread_tag, "p25_watchdog") == 0) {
        atomic_store(&g_watchdog_ftz_daz, (int)(csr & DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS));
        (void)atomic_fetch_add(&g_watchdog_entries, 1);
    }
}

static void
set_caller_ftz_daz(unsigned int bits) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr((_mm_getcsr() & ~DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS) | bits);
#else
    (void)bits;
#endif
}

static int
wait_for_entries(int want) {
    for (int i = 0; i < 5000; i++) {
        if (atomic_load(&g_watchdog_entries) >= want) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* One run: the run's mode published, the starting thread in the opposite mode, the watchdog's entry seen with the
 * run's mode, then a stop that joins it. */
static void
run_once(dsd_opts* opts, dsd_state* state, dsd_fp_mode mode, unsigned int caller_bits, unsigned int want_bits) {
    const int entries = atomic_load(&g_watchdog_entries);
    const int creates = g_creates;
    dsd_runtime_fp_mode_publish(mode);
    set_caller_ftz_daz(caller_bits);
    p25_sm_watchdog_start(opts, state);
    assert(g_creates == creates + 1);
    assert(wait_for_entries(entries + 1));
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    assert((unsigned int)atomic_load(&g_watchdog_ftz_daz) == want_bits);
#else
    (void)want_bits;
#endif
    p25_sm_watchdog_stop();
    assert(atomic_load(&g_watchdog_entries) == entries + 1);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts && state);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const unsigned int original_csr = _mm_getcsr();
#endif
    dsd_runtime_fp_mode_set_observer(observe);
    p25_sm_watchdog_set_thread_create_for_test(create_seam);

    /* The thread cannot start: the watchdog stays off, and the stop joins no handle that was never created. */
    g_fail_create = 1;
    p25_sm_watchdog_start(opts, state);
    assert(g_creates == 1);
    p25_sm_watchdog_stop();
    p25_sm_watchdog_stop();
    assert(atomic_load(&g_watchdog_entries) == 0);

    /* The next start creates it (a running flag left set would make this start a no-op), and an on-to-off restart
       applies each run's mode at the entry. */
    g_fail_create = 0;
    run_once(opts, state, DSD_FP_MODE_ON, 0U, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    run_once(opts, state, DSD_FP_MODE_OFF, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS, 0U);
    p25_sm_watchdog_stop();

    p25_sm_watchdog_set_thread_create_for_test(NULL);
    dsd_runtime_fp_mode_set_observer(NULL);
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr(original_csr);
#endif
    free(state);
    free(opts);
    printf("P25_SM_WATCHDOG_START: OK\n");
    return 0;
}
