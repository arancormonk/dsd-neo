// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/telemetry_observers.h>

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stddef.h>

#include "snapshot_internal.h"

enum { DSD_APP_TELEMETRY_MAX_OBSERVERS = 8 };

static const dsd_app_telemetry_observer* g_observers[DSD_APP_TELEMETRY_MAX_OBSERVERS];
static atomic_int g_observer_count = 0;
/* Guards the table and every dispatch: callbacks run under it, so a removal returns only once no callback of the
   removed observer is still running and the observer's storage may go. */
static dsd_mutex_t g_observer_mu;
static atomic_int g_observer_mu_state = 0; /* 0=uninit, 1=initing, 2=init */

static void
ensure_mu_init(void) {
    if (atomic_load(&g_observer_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_observer_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_observer_mu);
        atomic_store(&g_observer_mu_state, 2);
        return;
    }
    while (atomic_load(&g_observer_mu_state) != 2) {
        dsd_thread_yield();
    }
}

int
dsd_app_telemetry_observer_add(const dsd_app_telemetry_observer* observer) {
    if (observer == NULL || (observer->state == NULL && observer->opts == NULL)) {
        return -1;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_observer_mu);
    int rc = -1;
    int free_slot = -1;
    for (int i = 0; i < DSD_APP_TELEMETRY_MAX_OBSERVERS; i++) {
        if (g_observers[i] == observer) {
            free_slot = -1;
            break;
        }
        if (g_observers[i] == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        g_observers[free_slot] = observer;
        atomic_fetch_add(&g_observer_count, 1);
        rc = 0;
    }
    dsd_mutex_unlock(&g_observer_mu);
    return rc;
}

int
dsd_app_telemetry_observer_remove(const dsd_app_telemetry_observer* observer) {
    if (observer == NULL) {
        return -1;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_observer_mu);
    int rc = -1;
    for (int i = 0; i < DSD_APP_TELEMETRY_MAX_OBSERVERS; i++) {
        if (g_observers[i] == observer) {
            g_observers[i] = NULL;
            atomic_fetch_add(&g_observer_count, -1);
            rc = 0;
            break;
        }
    }
    dsd_mutex_unlock(&g_observer_mu);
    return rc;
}

void
dsd_app_telemetry_notify_state(const dsd_state* state) {
    if (atomic_load(&g_observer_count) <= 0) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_observer_mu);
    for (int i = 0; i < DSD_APP_TELEMETRY_MAX_OBSERVERS; i++) {
        const dsd_app_telemetry_observer* o = g_observers[i];
        if (o != NULL && o->state != NULL) {
            o->state(state, o->user);
        }
    }
    dsd_mutex_unlock(&g_observer_mu);
}

void
dsd_app_telemetry_notify_opts(const dsd_opts* opts) {
    if (atomic_load(&g_observer_count) <= 0) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_observer_mu);
    for (int i = 0; i < DSD_APP_TELEMETRY_MAX_OBSERVERS; i++) {
        const dsd_app_telemetry_observer* o = g_observers[i];
        if (o != NULL && o->opts != NULL) {
            o->opts(opts, o->user);
        }
    }
    dsd_mutex_unlock(&g_observer_mu);
}
