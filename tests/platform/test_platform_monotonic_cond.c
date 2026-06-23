// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"

struct cond_signal_ctx {
    dsd_cond_t* cond;
    dsd_mutex_t* mutex;
    int* fired;
};

#if !DSD_PLATFORM_WIN_NATIVE
int dsd_thread_create_impl(dsd_thread_t* thread, void* arg, dsd_thread_fn func);
#endif

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    signal_thread_fn(void* arg) {
    struct cond_signal_ctx* ctx = (struct cond_signal_ctx*)arg;
    dsd_sleep_ms(50);
    dsd_mutex_lock(ctx->mutex);
    *ctx->fired = 1;
    dsd_cond_signal(ctx->cond);
    dsd_mutex_unlock(ctx->mutex);
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    immediate_signal_thread_fn(void* arg) {
    struct cond_signal_ctx* ctx = (struct cond_signal_ctx*)arg;
    dsd_mutex_lock(ctx->mutex);
    *ctx->fired = 1;
    dsd_cond_broadcast(ctx->cond);
    dsd_mutex_unlock(ctx->mutex);
    DSD_THREAD_RETURN;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
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

static int
test_timing_wrappers(void) {
    int rc = 0;
    struct tm tm_out;
    time_t fixed = 0;

    errno = 0;
    rc |= expect_int("localtime rejects null time", dsd_localtime(NULL, &tm_out), -1);
    rc |= expect_int("localtime null time errno", errno, EINVAL);

    errno = 0;
    rc |= expect_int("localtime rejects null out", dsd_localtime(&fixed, NULL), -1);
    rc |= expect_int("localtime null out errno", errno, EINVAL);

    errno = 0;
    rc |= expect_int("gmtime rejects null time", dsd_gmtime(NULL, &tm_out), -1);
    rc |= expect_int("gmtime null time errno", errno, EINVAL);

    rc |= expect_int("gmtime epoch", dsd_gmtime(&fixed, &tm_out), 0);
    rc |= expect_int("gmtime epoch year", tm_out.tm_year, 70);
    rc |= expect_int("gmtime epoch month", tm_out.tm_mon, 0);
    rc |= expect_int("gmtime epoch day", tm_out.tm_mday, 1);
    rc |= expect_int("localtime epoch", dsd_localtime(&fixed, &tm_out), 0);

    uint64_t monotonic_ns = dsd_time_monotonic_ns();
    uint64_t monotonic_ms = dsd_time_monotonic_ms();
    uint64_t realtime_before = dsd_time_realtime_ns();
    uint64_t deadline = dsd_time_deadline_ns(25);
    uint64_t realtime_after = dsd_time_realtime_ns();

    rc |= expect_true("monotonic ns initialized", monotonic_ns > 0U);
    rc |= expect_true("monotonic ms initialized", monotonic_ms > 0U);
    rc |= expect_true("realtime clock initialized", realtime_before > 1000000000000000000ULL);
    rc |= expect_true("deadline is not before realtime", deadline >= realtime_before);
    rc |= expect_true("deadline includes requested interval", deadline >= realtime_after);
    rc |= expect_true("deadline remains close", deadline <= realtime_after + 2000000000ULL);

    dsd_sleep_ns(0);
    dsd_sleep_us(0);
    dsd_sleep_ns(1);

    return rc;
}

static int
test_threading_wrapper_contracts(void) {
    int rc = 0;

    rc |= expect_int("mutex_init rejects null", dsd_mutex_init(NULL), EINVAL);
    rc |= expect_int("mutex_destroy rejects null", dsd_mutex_destroy(NULL), EINVAL);
    rc |= expect_int("mutex_lock rejects null", dsd_mutex_lock(NULL), EINVAL);
    rc |= expect_int("mutex_unlock rejects null", dsd_mutex_unlock(NULL), EINVAL);
    rc |= expect_int("cond_init rejects null", dsd_cond_init(NULL), EINVAL);
    rc |= expect_int("cond_init_monotonic rejects null", dsd_cond_init_monotonic(NULL), EINVAL);
    rc |= expect_int("cond_destroy rejects null", dsd_cond_destroy(NULL), EINVAL);
    rc |= expect_int("cond_wait rejects null cond", dsd_cond_wait(NULL, (dsd_mutex_t*)1), EINVAL);
    rc |= expect_int("cond_wait rejects null mutex", dsd_cond_wait((dsd_cond_t*)1, NULL), EINVAL);
    rc |= expect_int("cond_timedwait rejects null cond", dsd_cond_timedwait(NULL, (dsd_mutex_t*)1, 1), EINVAL);
    rc |= expect_int("cond_timedwait rejects null mutex", dsd_cond_timedwait((dsd_cond_t*)1, NULL, 1), EINVAL);
    rc |= expect_int("cond_timedwait_monotonic rejects null cond",
                     dsd_cond_timedwait_monotonic(NULL, (dsd_mutex_t*)1, 1), EINVAL);
    rc |= expect_int("cond_timedwait_monotonic rejects null mutex",
                     dsd_cond_timedwait_monotonic((dsd_cond_t*)1, NULL, 1), EINVAL);
    rc |= expect_int("cond_signal rejects null", dsd_cond_signal(NULL), EINVAL);
    rc |= expect_int("cond_broadcast rejects null", dsd_cond_broadcast(NULL), EINVAL);

    dsd_thread_t self = dsd_thread_self();
    rc |= expect_true("thread self is nonzero", (uintptr_t)self != 0U);

#if !DSD_PLATFORM_WIN_NATIVE
    dsd_thread_t thread;
    dsd_thread_t* null_thread = NULL;
    dsd_thread_fn null_fn = NULL;
    rc |= expect_int("thread_create_impl rejects null thread",
                     dsd_thread_create_impl(NULL, NULL, immediate_signal_thread_fn), EINVAL);
    rc |= expect_int("thread_create_impl rejects null function", dsd_thread_create_impl(&thread, NULL, NULL), EINVAL);
    rc |= expect_int("thread_create macro rejects null thread",
                     dsd_thread_create(null_thread, immediate_signal_thread_fn, NULL), EINVAL);
    rc |= expect_int("thread_create macro rejects null function", dsd_thread_create(&thread, null_fn, NULL), EINVAL);
#endif

    int prio_rc = dsd_thread_set_realtime_priority(-1000);
    rc |=
        expect_true("realtime priority wrapper returns status", prio_rc == 0 || prio_rc == EPERM || prio_rc == EINVAL);

    int affinity_rc = dsd_thread_set_affinity(0);
    rc |= expect_true("affinity wrapper returns status",
                      affinity_rc == 0 || affinity_rc == EINVAL || affinity_rc == EPERM || affinity_rc == ESRCH);

    return rc;
}

int
main(void) {
    int rc = 0;
    dsd_mutex_t mutex;
    dsd_cond_t cond;
    int fired = 0;
    dsd_thread_t thread;

    rc |= test_timing_wrappers();
    rc |= test_threading_wrapper_contracts();

    rc |= expect_int("mutex_init", dsd_mutex_init(&mutex), 0);
    rc |= expect_int("cond_init_monotonic", dsd_cond_init_monotonic(&cond), 0);

    struct cond_signal_ctx ctx = {&cond, &mutex, &fired};
    rc |= expect_int("thread_create", dsd_thread_create(&thread, signal_thread_fn, &ctx), 0);

    uint64_t start_ns = dsd_time_monotonic_ns();
    uint64_t deadline_ns = start_ns + 2000000000ULL;
    rc |= expect_int("mutex_lock", dsd_mutex_lock(&mutex), 0);
    while (!fired) {
        int wrc = dsd_cond_timedwait_monotonic(&cond, &mutex, deadline_ns);
        if (wrc == ETIMEDOUT) {
            rc |= expect_true("monotonic wait should have been signaled", 0);
            break;
        }
        if (wrc != 0) {
            DSD_FPRINTF(stderr, "FAIL: monotonic wait error rc=%d\n", wrc);
            rc |= 1;
            break;
        }
    }
    rc |= expect_int("mutex_unlock", dsd_mutex_unlock(&mutex), 0);
    uint64_t elapsed_ns = dsd_time_monotonic_ns() - start_ns;
    rc |= expect_true("signal wait elapsed < 2s", elapsed_ns < 2000000000ULL);
    rc |= expect_int("thread_join", dsd_thread_join(thread), 0);

    rc |= expect_int("mutex_lock (past deadline test)", dsd_mutex_lock(&mutex), 0);
    uint64_t before_timeout_ns = dsd_time_monotonic_ns();
    int timeout_rc = dsd_cond_timedwait_monotonic(&cond, &mutex, before_timeout_ns - 1U);
    uint64_t timeout_elapsed_ns = dsd_time_monotonic_ns() - before_timeout_ns;
    rc |= expect_int("past deadline timeout rc", timeout_rc, ETIMEDOUT);
    rc |= expect_true("past deadline returns quickly", timeout_elapsed_ns < 200000000ULL);
    rc |= expect_int("mutex_unlock (past deadline test)", dsd_mutex_unlock(&mutex), 0);

    rc |= expect_int("cond_destroy", dsd_cond_destroy(&cond), 0);
    rc |= expect_int("mutex_destroy", dsd_mutex_destroy(&mutex), 0);

    dsd_cond_t regular_cond;
    fired = 0;
    rc |= expect_int("regular mutex_init", dsd_mutex_init(&mutex), 0);
    rc |= expect_int("regular cond_init", dsd_cond_init(&regular_cond), 0);
    struct cond_signal_ctx regular_ctx = {&regular_cond, &mutex, &fired};
    rc |= expect_int("regular thread_create", dsd_thread_create(&thread, immediate_signal_thread_fn, &regular_ctx), 0);
    rc |= expect_int("regular mutex_lock", dsd_mutex_lock(&mutex), 0);
    while (!fired) {
        rc |= expect_int("regular cond_wait", dsd_cond_wait(&regular_cond, &mutex), 0);
    }
    rc |= expect_int("regular mutex_unlock", dsd_mutex_unlock(&mutex), 0);
    rc |= expect_int("regular thread_join", dsd_thread_join(thread), 0);
    rc |= expect_int("cond_signal success", dsd_cond_signal(&regular_cond), 0);
    rc |= expect_int("cond_broadcast success", dsd_cond_broadcast(&regular_cond), 0);
    rc |= expect_int("regular cond_destroy", dsd_cond_destroy(&regular_cond), 0);
    rc |= expect_int("regular mutex_destroy", dsd_mutex_destroy(&mutex), 0);

    return rc ? 1 : 0;
}
