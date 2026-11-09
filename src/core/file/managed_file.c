// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/managed_file.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include "managed_file_test_support.h"

static dsd_mutex_t s_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. */
static atomic_int s_mu_state;
static atomic_int s_waiters;

/* Read and written only with the lock held. */
static dsd_managed_file_test_hook s_test_hook;

void
dsd_managed_file_lock(void) {
    if (atomic_load(&s_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&s_mu);
            atomic_store(&s_mu_state, 2);
        } else {
            while (atomic_load(&s_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    (void)atomic_fetch_add(&s_waiters, 1);
    dsd_mutex_lock(&s_mu);
    (void)atomic_fetch_add(&s_waiters, -1);
}

void
dsd_managed_file_unlock(void) {
    dsd_mutex_unlock(&s_mu);
}

void
dsd_managed_file_step_reached(dsd_managed_file_step step) {
    if (s_test_hook) {
        s_test_hook(step);
    }
}

void
dsd_managed_file_test_set_hook(dsd_managed_file_test_hook hook) {
    dsd_managed_file_lock();
    s_test_hook = hook;
    dsd_managed_file_unlock();
}

int
dsd_managed_file_test_waiters(void) {
    return atomic_load(&s_waiters);
}
