// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A Signal search visit tag whose lock cannot be initialised (issue #809): nothing is set or read through a lock never
   initialised, the init is tried once, and the tag reads as clear, so recordings are published untagged. */
#include <assert.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>

static int g_initializations;
static int g_locks;

int
dsd_test_visit_mutex_init(dsd_mutex_t* mutex) {
    (void)mutex;
    g_initializations++;
    return -1;
}

int
dsd_test_visit_mutex_lock(dsd_mutex_t* mutex) {
    (void)mutex;
    g_locks++;
    return -1;
}

int
dsd_test_visit_mutex_unlock(dsd_mutex_t* mutex) {
    (void)mutex;
    return -1;
}

int
main(void) {
    dsd_signal_search_visit_set(42, 3);
    uint64_t session = 7, hit = 7;
    assert(dsd_signal_search_visit_get(&session, &hit) == 0 && session == 0 && hit == 0);
    dsd_signal_search_visit_clear();
    assert(dsd_signal_search_visit_get(NULL, NULL) == 0);
    assert(g_initializations == 1);
    assert(g_locks == 0);
    return 0;
}
