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
static atomic_int g_channel_scan_hooks_mu_state = 0; // 0=uninit, 1=initing, 2=init

static void
ensure_channel_scan_hooks_mu_init(void) {
    if (atomic_load(&g_channel_scan_hooks_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_channel_scan_hooks_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_channel_scan_hooks_mu);
        atomic_store(&g_channel_scan_hooks_mu_state, 2);
        return;
    }
    while (atomic_load(&g_channel_scan_hooks_mu_state) != 2) {
        dsd_thread_yield();
    }
}

void
dsd_channel_scan_pause_hook_set(dsd_channel_scan_pause_fn fn) {
    ensure_channel_scan_hooks_mu_init();
    dsd_mutex_lock(&g_channel_scan_hooks_mu);
    g_channel_scan_pause_fn = fn;
    dsd_mutex_unlock(&g_channel_scan_hooks_mu);
}

dsd_channel_scan_pause_fn
dsd_channel_scan_pause_hook_get(void) {
    ensure_channel_scan_hooks_mu_init();
    dsd_mutex_lock(&g_channel_scan_hooks_mu);
    const dsd_channel_scan_pause_fn fn = g_channel_scan_pause_fn;
    dsd_mutex_unlock(&g_channel_scan_hooks_mu);
    return fn;
}
