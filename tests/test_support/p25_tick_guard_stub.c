// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * The P25 SM tick guard (src/protocol/p25/p25_sm_watchdog.c) for tests that build an app-control publisher without the
 * P25 protocol library: the same one flag, owned per thread and not re-entrant. A re-entry is counted
 * (dsd_test_p25_tick_guard_reentries()) and returns, where the real guard would hang. A hold on another thread stands
 * in for a watchdog tick in progress (dsd_test_p25_tick_guard_hold_elsewhere()).
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <stddef.h>
#include "p25_tick_guard_stub.h"

#ifdef _MSC_VER
#define P25_TICK_GUARD_STUB_THREAD_LOCAL __declspec(thread)
#else
#define P25_TICK_GUARD_STUB_THREAD_LOCAL _Thread_local
#endif

static atomic_int g_lock = 0;
static atomic_int g_reentries = 0;
static P25_TICK_GUARD_STUB_THREAD_LOCAL int s_held_here = 0;

int
p25_sm_tick_guard_try_enter(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_lock, &expected, 1)) {
        return 0;
    }
    s_held_here = 1;
    return 1;
}

void
p25_sm_tick_guard_enter(void) {
    if (s_held_here) {
        (void)atomic_fetch_add(&g_reentries, 1);
        return;
    }
    while (!p25_sm_tick_guard_try_enter()) {
        dsd_thread_yield();
    }
}

void
p25_sm_tick_guard_leave(void) {
    s_held_here = 0;
    atomic_store(&g_lock, 0);
}

int
p25_sm_tick_guard_held_by_caller(void) {
    return s_held_here;
}

int
dsd_test_p25_tick_guard_reentries(void) {
    return atomic_load(&g_reentries);
}

static struct {
    dsd_thread_t thread;
    dsd_mutex_t mutex;
    dsd_cond_t condition;
    int initialized;
    int held;
    int release;
    int ran_out;
} g_elsewhere;

static DSD_THREAD_RETURN_TYPE
hold_elsewhere(void* arg) {
    (void)arg;
    while (!p25_sm_tick_guard_try_enter()) {
        dsd_thread_yield();
    }
    dsd_mutex_lock(&g_elsewhere.mutex);
    g_elsewhere.held = 1;
    dsd_cond_broadcast(&g_elsewhere.condition);
    while (!g_elsewhere.release) {
        if (dsd_cond_timedwait(&g_elsewhere.condition, &g_elsewhere.mutex, 2000U) != 0) {
            g_elsewhere.ran_out = 1;
            break;
        }
    }
    dsd_mutex_unlock(&g_elsewhere.mutex);
    p25_sm_tick_guard_leave();
    DSD_THREAD_RETURN;
}

int
dsd_test_p25_tick_guard_hold_elsewhere(void) {
    if (!g_elsewhere.initialized) {
        (void)dsd_mutex_init(&g_elsewhere.mutex);
        (void)dsd_cond_init(&g_elsewhere.condition);
        g_elsewhere.initialized = 1;
    }
    g_elsewhere.held = g_elsewhere.release = g_elsewhere.ran_out = 0;
    if (dsd_thread_create(&g_elsewhere.thread, hold_elsewhere, NULL) != 0) {
        return 0;
    }
    dsd_mutex_lock(&g_elsewhere.mutex);
    while (!g_elsewhere.held && !g_elsewhere.ran_out) {
        if (dsd_cond_timedwait(&g_elsewhere.condition, &g_elsewhere.mutex, 2000U) != 0) {
            break;
        }
    }
    const int held = g_elsewhere.held;
    dsd_mutex_unlock(&g_elsewhere.mutex);
    return held;
}

int
dsd_test_p25_tick_guard_release_elsewhere(void) {
    dsd_mutex_lock(&g_elsewhere.mutex);
    g_elsewhere.release = 1;
    dsd_cond_broadcast(&g_elsewhere.condition);
    dsd_mutex_unlock(&g_elsewhere.mutex);
    dsd_thread_join(g_elsewhere.thread);
    return !g_elsewhere.ran_out;
}
