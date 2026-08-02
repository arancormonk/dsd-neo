// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_thread_yield() exists for the one-time-init spin loops in app_control and
 * runtime, so what matters is that it always returns and never wedges a spinner:
 * a yield that blocked, or that a second thread could not make progress across,
 * would turn those loops into the deadlock they are written to avoid.
 */

#include <assert.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stddef.h>

static atomic_int g_flag = 0;
static atomic_int g_spins = 0;

static void*
setter_thread(void* arg) {
    (void)arg;
    atomic_store(&g_flag, 1);
    return NULL;
}

/* Calling it with nothing else runnable must simply return. */
static void
test_yield_returns_when_alone(void) {
    for (int i = 0; i < 1000; i++) {
        dsd_thread_yield();
    }
}

/* The shape the init spin loops use: another thread's store has to become visible
 * across a yielding spin. If this hangs, the CTest timeout is the failure. */
static void
test_spin_observes_other_thread(void) {
    dsd_thread_t thread;

    atomic_store(&g_flag, 0);
    atomic_store(&g_spins, 0);

    assert(dsd_thread_create(&thread, setter_thread, NULL) == 0);

    while (atomic_load(&g_flag) != 1) {
        atomic_fetch_add(&g_spins, 1);
        dsd_thread_yield();
    }

    assert(dsd_thread_join(thread) == 0);
    assert(atomic_load(&g_flag) == 1);
}

int
main(void) {
    test_yield_returns_when_alone();
    test_spin_observes_other_thread();
    return 0;
}
