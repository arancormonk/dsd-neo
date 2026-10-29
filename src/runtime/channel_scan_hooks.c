// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/channel_scan_hooks.h>
#include <stddef.h>

/* Installed and cleared with the frontend session, on whichever thread starts or stops it, while the decoder thread
   reads it: under a lock, as the control pump is (control_pump.c). */
static dsd_channel_scan_pause_fn g_channel_scan_pause_fn = NULL;
static dsd_mutex_t g_channel_scan_hooks_mu;
static atomic_int g_channel_scan_hooks_mu_state = 0; // 0=uninit, 1=initing, 2=init, -1=failed

/* Whether the lock is usable. A lock that failed to initialise stays unusable: nothing is installed through it, and
   nothing is read through it (the engine then pauses on its own, as without an installed pause). */
static int
ensure_channel_scan_hooks_mu_init(void) {
    int state = atomic_load(&g_channel_scan_hooks_mu_state);
    if (state == 2 || state == -1) {
        return state == 2;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_channel_scan_hooks_mu_state, &expected, 1)) {
        const int ready = dsd_mutex_init(&g_channel_scan_hooks_mu) == 0;
        atomic_store(&g_channel_scan_hooks_mu_state, ready ? 2 : -1);
        return ready;
    }
    while ((state = atomic_load(&g_channel_scan_hooks_mu_state)) == 1) {
        dsd_thread_yield();
    }
    return state == 2;
}

void
dsd_channel_scan_pause_hook_set(dsd_channel_scan_pause_fn fn) {
    if (!ensure_channel_scan_hooks_mu_init() || dsd_mutex_lock(&g_channel_scan_hooks_mu) != 0) {
        return;
    }
    g_channel_scan_pause_fn = fn;
    dsd_mutex_unlock(&g_channel_scan_hooks_mu);
}

dsd_channel_scan_pause_fn
dsd_channel_scan_pause_hook_get(void) {
    if (!ensure_channel_scan_hooks_mu_init() || dsd_mutex_lock(&g_channel_scan_hooks_mu) != 0) {
        return NULL;
    }
    const dsd_channel_scan_pause_fn fn = g_channel_scan_pause_fn;
    dsd_mutex_unlock(&g_channel_scan_hooks_mu);
    return fn;
}
