// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The environment override store under concurrent readers and writers (#782).
 *
 * Readers loop dsd_env_get() while a writer sets, hides and clears overrides and adds new names. Every value a reader
 * gets must be NULL or one of the strings that could be in force, and must still read the same after the writer has
 * moved on. The first calls of every thread race the store's lazy lock initialisation. Run under tsan-debug too.
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

static const char* const kName = "DSD_NEO_TEST_ENV_THREADS";
static const char* const kValues[] = {"process", "host", "a", "bb", "ccc", "ini"};

static atomic_int g_start = 0;
static atomic_int g_stop = 0;
static atomic_int g_bad = 0;

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
    (void)arg;
    while (atomic_load(&g_start) == 0) {
        dsd_thread_yield();
    }
    const char* held[8] = {0};
    char held_text[8][8];
    unsigned n = 0;
    while (atomic_load(&g_stop) == 0) {
        const char* v = dsd_env_get(kName);
        if (!value_allowed(v)) {
            atomic_store(&g_bad, 1);
        }
        /* A pointer read earlier still holds the text it had. */
        const unsigned slot = n % 8U;
        if (held[slot] != NULL && strcmp(held[slot], held_text[slot]) != 0) {
            atomic_store(&g_bad, 2);
        }
        held[slot] = v;
        if (v != NULL) {
            DSD_SNPRINTF(held_text[slot], sizeof held_text[slot], "%s", v);
        }
        char extra[48];
        DSD_SNPRINTF(extra, sizeof extra, "DSD_NEO_TEST_ENV_THREADS_%u", n % EXTRA_NAMES);
        const char* e = dsd_env_get(extra);
        if (e != NULL && strcmp(e, "x") != 0) {
            atomic_store(&g_bad, 3);
        }
        n++;
    }
    DSD_THREAD_RETURN;
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
        assert(dsd_thread_create(&readers[i], reader_main, NULL) == 0);
    }
    atomic_store(&g_start, 1);
    for (int i = 0; i < ITERATIONS; i++) {
        writer_step(i);
    }
    atomic_store(&g_stop, 1);
    for (int i = 0; i < READERS; i++) {
        assert(dsd_thread_join(readers[i]) == 0);
    }
    if (atomic_load(&g_bad) != 0) {
        DSD_FPRINTF(stderr, "FAIL: a reader saw a value no writer set (%d)\n", atomic_load(&g_bad));
        return 1;
    }
    printf("PLATFORM_ENV_STORE_THREADS: OK\n");
    return 0;
}
