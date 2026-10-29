// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The environment override store under concurrent readers and writers (#782).
 *
 * Readers loop dsd_env_read() while a writer sets, hides and clears overrides (each replaced value is zeroed and freed)
 * and adds and frees names. Every value a reader copies must be NULL or one of the strings that could be in force.
 * The writer waits for a read by every reader before its first write and after every CHECKPOINT writes, so the test
 * proves overlap however the threads are scheduled. The first calls of every thread race the store's lazy lock
 * initialisation. Run under tsan-debug too.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/threading.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/platform/platform.h>
#include "test_env.h"

#define READERS     4
#define ITERATIONS  20000
#define EXTRA_NAMES 48
#define CHECKPOINT  250

static const char* const kName = "DSD_NEO_TEST_ENV_THREADS";
static const char* const kValues[] = {"process", "host", "a", "bb", "ccc", "ini"};

static atomic_int g_start = 0;
static atomic_int g_stop = 0;
static atomic_int g_bad = 0;
/* Reads each reader finished; the writer waits on them so every stretch of writes overlaps reads by every reader. */
static atomic_int g_reads[READERS];

static int
value_allowed(const char* v) {
    if (v == NULL) {
        return 1;
    }
    for (size_t i = 0; i < sizeof kValues / sizeof kValues[0]; i++) {
        if (strcmp(v, kValues[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    reader_main(void* arg) {
    atomic_int* reads = (atomic_int*)arg;
    while (atomic_load(&g_start) == 0) {
        dsd_thread_yield();
    }
    unsigned n = 0;
    while (atomic_load(&g_stop) == 0) {
        char buf[16];
        const char* v = dsd_env_read(kName, buf, sizeof buf);
        if (!value_allowed(v)) {
            atomic_store(&g_bad, 1);
        }
        char extra[48];
        DSD_SNPRINTF(extra, sizeof extra, "DSD_NEO_TEST_ENV_THREADS_%u", n % EXTRA_NAMES);
        const char* e = dsd_env_read(extra, buf, sizeof buf);
        if (e != NULL && strcmp(e, "x") != 0) {
            atomic_store(&g_bad, 3);
        }
        n++;
        (void)atomic_fetch_add(reads, 1);
    }
    DSD_THREAD_RETURN;
}

/* Wait until every reader has finished at least one more read than `seen` records, then record the new counts. */
static void
wait_for_every_reader(int* seen) {
    for (int r = 0; r < READERS; r++) {
        while (atomic_load(&g_reads[r]) <= seen[r]) {
            dsd_thread_yield();
        }
        seen[r] = atomic_load(&g_reads[r]);
    }
}

static void
writer_step(int i) {
    switch (i % 7) {
        case 0: (void)dsd_env_set(DSD_ENV_LAYER_SESSION, kName, kValues[2 + (i / 7) % 3]); break;
        case 1: (void)dsd_env_set(DSD_ENV_LAYER_SESSION, kName, NULL); break;
        case 2: dsd_env_clear_layer(DSD_ENV_LAYER_SESSION); break;
        case 3: (void)dsd_env_set(DSD_ENV_LAYER_HOST, kName, "host"); break;
        case 4: (void)dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, kName, "ini"); break;
        case 5: dsd_env_clear_layer(DSD_ENV_LAYER_HOST); break;
        default: {
            char extra[48];
            DSD_SNPRINTF(extra, sizeof extra, "DSD_NEO_TEST_ENV_THREADS_%d", (i / 7) % EXTRA_NAMES);
            (void)dsd_env_set(DSD_ENV_LAYER_SESSION, extra, "x");
            break;
        }
    }
}

int
main(void) {
    assert(dsd_test_setenv(kName, "process", 1) == 0);
    dsd_thread_t readers[READERS];
    for (int i = 0; i < READERS; i++) {
        assert(dsd_thread_create(&readers[i], reader_main, &g_reads[i]) == 0);
    }
    atomic_store(&g_start, 1);
    /* Every reader reads before the first write and between every CHECKPOINT writes, so reads and writes overlap
       throughout, however the threads are scheduled. */
    int seen[READERS] = {0};
    int checkpoints = 0;
    wait_for_every_reader(seen);
    for (int i = 0; i < ITERATIONS; i++) {
        writer_step(i);
        if (i % CHECKPOINT == CHECKPOINT - 1) {
            wait_for_every_reader(seen);
            checkpoints++;
        }
    }
    atomic_store(&g_stop, 1);
    for (int i = 0; i < READERS; i++) {
        assert(dsd_thread_join(readers[i]) == 0);
    }
    assert(checkpoints == ITERATIONS / CHECKPOINT);
    if (atomic_load(&g_bad) != 0) {
        DSD_FPRINTF(stderr, "FAIL: a reader saw a value no writer set (%d)\n", atomic_load(&g_bad));
        return 1;
    }
    printf("PLATFORM_ENV_STORE_THREADS: OK\n");
    return 0;
}
