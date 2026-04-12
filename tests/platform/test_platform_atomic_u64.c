// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

#include <stdint.h>
#include <stdio.h>

struct inc_args {
    dsd_atomic_u64* counter;
    int loops;
};

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    inc_thread_fn(void* arg) {
    struct inc_args* args = (struct inc_args*)arg;
    for (int i = 0; i < args->loops; i++) {
        (void)dsd_atomic_u64_fetch_add_relaxed(args->counter, 1U);
    }
    DSD_THREAD_RETURN;
}

struct publish_args {
    dsd_atomic_u64* value;
    atomic_int* ready;
    uint64_t data;
};

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    publish_thread_fn(void* arg) {
    struct publish_args* args = (struct publish_args*)arg;
    dsd_atomic_u64_store_release(args->value, args->data);
    atomic_store(args->ready, 1);
    DSD_THREAD_RETURN;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
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
test_init_load_store(void) {
    int rc = 0;
    dsd_atomic_u64 a;
    dsd_atomic_u64_init(&a, 7U);
    rc |= expect_u64("load_relaxed after init", dsd_atomic_u64_load_relaxed(&a), 7U);
    dsd_atomic_u64_store_relaxed(&a, 11U);
    rc |= expect_u64("load_relaxed after store_relaxed", dsd_atomic_u64_load_relaxed(&a), 11U);
    dsd_atomic_u64_store_release(&a, 13U);
    rc |= expect_u64("load_acquire after store_release", dsd_atomic_u64_load_acquire(&a), 13U);
    return rc;
}

static int
test_fetch_add_single_thread(void) {
    int rc = 0;
    dsd_atomic_u64 a;
    dsd_atomic_u64_init(&a, 10U);
    rc |= expect_u64("fetch_add_relaxed returns old value", dsd_atomic_u64_fetch_add_relaxed(&a, 5U), 10U);
    rc |= expect_u64("value after fetch_add_relaxed", dsd_atomic_u64_load_relaxed(&a), 15U);
    rc |= expect_u64("fetch_add_release returns old value", dsd_atomic_u64_fetch_add_release(&a, 2U), 15U);
    rc |= expect_u64("value after fetch_add_release", dsd_atomic_u64_load_acquire(&a), 17U);
    return rc;
}

static int
test_multithread_increment(void) {
    int rc = 0;
    dsd_atomic_u64 counter;
    dsd_atomic_u64_init(&counter, 0U);

    enum { k_threads = 4, k_loops = 100000 };

    dsd_thread_t threads[k_threads];
    struct inc_args args[k_threads];

    for (int i = 0; i < k_threads; i++) {
        args[i].counter = &counter;
        args[i].loops = k_loops;
        int trc = dsd_thread_create(&threads[i], (dsd_thread_fn)inc_thread_fn, &args[i]);
        rc |= expect_int("thread_create", trc, 0);
    }
    for (int i = 0; i < k_threads; i++) {
        rc |= expect_int("thread_join", dsd_thread_join(threads[i]), 0);
    }

    rc |= expect_u64("multithread final value", dsd_atomic_u64_load_relaxed(&counter),
                     (uint64_t)k_threads * (uint64_t)k_loops);
    return rc;
}

static int
test_release_acquire_smoke(void) {
    int rc = 0;
    dsd_atomic_u64 value;
    dsd_atomic_u64_init(&value, 0U);
    atomic_int ready = 0;
    struct publish_args args = {&value, &ready, 0x1122334455667788ULL};
    dsd_thread_t t;
    rc |= expect_int("publish thread create", dsd_thread_create(&t, (dsd_thread_fn)publish_thread_fn, &args), 0);
    while (!atomic_load(&ready)) {
        dsd_sleep_ms(1);
    }
    rc |= expect_u64("acquire sees published value", dsd_atomic_u64_load_acquire(&value), 0x1122334455667788ULL);
    rc |= expect_int("publish thread join", dsd_thread_join(t), 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_init_load_store();
    rc |= test_fetch_add_single_thread();
    rc |= test_multithread_increment();
    rc |= test_release_acquire_smoke();
    return rc ? 1 : 0;
}
