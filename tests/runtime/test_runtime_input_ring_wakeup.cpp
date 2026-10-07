// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A commit that lands between the reader's first look at an empty input ring and its lock must not leave the reader
 * asleep for the wait's timeout. input_ring_commit() signals only the empty-to-non-empty step, so that commit's signal
 * reaches no waiter: the reader has to look at the ring again under the lock before it waits.
 *
 * --wrap=dsd_mutex_lock puts the commit exactly there. The first time the reader takes the ring's lock (after it found
 * the ring empty), the wrapper commits two floats and only then takes the lock. The test is single-threaded, so nothing
 * else can signal: a reader that waits anyway sleeps out its 10 ms and counts a read timeout.
 */

#include <atomic>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/input_ring.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/safe_api.h"

extern "C" int
dsd_rtl_stream_should_exit(void) {
    return 0;
}

// GNU ld --wrap entry points must keep the reserved __wrap_*/__real_* symbol names.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
extern "C" int __real_dsd_mutex_lock(dsd_mutex_t* mutex);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
extern "C" int __wrap_dsd_mutex_lock(dsd_mutex_t* mutex);

/* The ring under test, in static storage: the wrapper reaches it from the reader's dsd_mutex_lock() call. */
static struct input_ring_state g_ring;
static int g_armed = 0;
static int g_committed = 0;

static void
commit_two_floats(struct input_ring_state* ring) {
    float* p1 = nullptr;
    float* p2 = nullptr;
    size_t n1 = 0U;
    size_t n2 = 0U;
    if (input_ring_reserve(ring, 2U, &p1, &n1, &p2, &n2) != 2 || !p1 || n1 != 2U) {
        return;
    }
    p1[0] = 0.25f;
    p1[1] = -0.5f;
    input_ring_commit(ring, 2U);
    g_committed = 1;
}

extern "C" int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_mutex_lock(dsd_mutex_t* mutex) {
    if (g_armed && mutex == &g_ring.ready_m) {
        /* Disarmed first: input_ring_commit() takes this lock itself to signal. */
        g_armed = 0;
        commit_two_floats(&g_ring);
    }
    return __real_dsd_mutex_lock(mutex);
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
test_commit_before_the_lock_is_read_without_a_timeout(void) {
    int rc = 0;
    if (input_ring_init(&g_ring, 64U) != 0) {
        DSD_FPRINTF(stderr, "FAIL: input_ring_init\n");
        return 1;
    }
    g_committed = 0;
    g_armed = 1;
    float out[8] = {0.0f};
    int got = input_ring_read_block(&g_ring, out, 8U);
    g_armed = 0;
    rc |= expect_u64("the wrapper committed while the reader took the lock", (uint64_t)g_committed, 1U);
    rc |= expect_u64("floats read", (uint64_t)(got > 0 ? got : 0), 2U);
    rc |= expect_u64("read timeouts", g_ring.read_timeouts.load(), 0U);
    if (got == 2 && (fabsf(out[0] - 0.25f) > 1e-6f || fabsf(out[1] + 0.5f) > 1e-6f)) {
        DSD_FPRINTF(stderr, "FAIL: samples read: %f %f\n", (double)out[0], (double)out[1]);
        rc = 1;
    }
    input_ring_destroy(&g_ring);
    return rc;
}

int
main(void) {
    int rc = test_commit_before_the_lock_is_read_without_a_timeout();
    if (rc == 0) {
        DSD_FPRINTF(stdout, "RUNTIME_INPUT_RING_WAKEUP: OK\n");
    }
    return rc;
}
