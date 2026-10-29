// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A -Y pause hook table whose lock cannot be initialised (issue #671): nothing is installed or read through a lock
   never initialised, and the engine finds no pause to call, as when none is installed. */
#include <assert.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/channel_scan_hooks.h>
#include <stddef.h>

static int g_initializations;
static int g_locks;

int
dsd_test_channel_scan_mutex_init(dsd_mutex_t* mutex) {
    (void)mutex;
    g_initializations++;
    return -1;
}

int
dsd_test_channel_scan_mutex_lock(dsd_mutex_t* mutex) {
    (void)mutex;
    g_locks++;
    return -1;
}

int
dsd_test_channel_scan_mutex_unlock(dsd_mutex_t* mutex) {
    (void)mutex;
    return -1;
}

static int
pause_hook(dsd_opts* opts, dsd_state* state, int in_place) {
    (void)opts;
    (void)state;
    (void)in_place;
    return 0;
}

int
main(void) {
    dsd_channel_scan_pause_hook_set(pause_hook);
    assert(dsd_channel_scan_pause_hook_get() == NULL);
    dsd_channel_scan_pause_hook_set(NULL);
    assert(dsd_channel_scan_pause_hook_get() == NULL);
    assert(g_initializations == 1);
    assert(g_locks == 0);
    return 0;
}
