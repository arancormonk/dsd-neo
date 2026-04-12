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

struct cond_signal_ctx {
    dsd_cond_t* cond;
    dsd_mutex_t* mutex;
    int* fired;
};

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

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    dsd_mutex_t mutex;
    dsd_cond_t cond;
    int fired = 0;
    dsd_thread_t thread;

    rc |= expect_int("mutex_init", dsd_mutex_init(&mutex), 0);
    rc |= expect_int("cond_init_monotonic", dsd_cond_init_monotonic(&cond), 0);

    struct cond_signal_ctx ctx = {&cond, &mutex, &fired};
    rc |= expect_int("thread_create", dsd_thread_create(&thread, (dsd_thread_fn)signal_thread_fn, &ctx), 0);

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
            fprintf(stderr, "FAIL: monotonic wait error rc=%d\n", wrc);
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
    return rc ? 1 : 0;
}
