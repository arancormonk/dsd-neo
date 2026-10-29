// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * The P25 SM tick guard (src/protocol/p25/p25_sm_watchdog.c) for tests that build an app-control publisher without the
 * P25 protocol library: the same one flag, owned per thread and not re-entrant. A re-entry is counted
 * (dsd_test_p25_tick_guard_reentries()) and returns, where the real guard would hang.
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
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
