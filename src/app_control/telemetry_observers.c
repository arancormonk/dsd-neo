// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/telemetry_observers.h>

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <string.h>

enum { DSD_APP_TELEMETRY_MAX_OBSERVERS = 8 };

static const dsd_app_telemetry_observer* g_observers[DSD_APP_TELEMETRY_MAX_OBSERVERS];
static atomic_int g_observer_count = 0;
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
    for (int i = 0; i < DSD_APP_TELEMETRY_MAX_OBSERVERS; i++) {
        if (g_observers[i] == NULL) {
            g_observers[i] = observer;
            atomic_store(&g_observer_count, atomic_load(&g_observer_count) + 1);
            rc = 0;
            break;
        }
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
            atomic_store(&g_observer_count, atomic_load(&g_observer_count) - 1);
            rc = 0;
            break;
        }
    }
    dsd_mutex_unlock(&g_observer_mu);
    return rc;
}

static int
snapshot_observers(const dsd_app_telemetry_observer* out[DSD_APP_TELEMETRY_MAX_OBSERVERS]) {
    if (atomic_load(&g_observer_count) <= 0) {
        return 0;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_observer_mu);
    int n = 0;
    for (int i = 0; i < DSD_APP_TELEMETRY_MAX_OBSERVERS; i++) {
        if (g_observers[i] != NULL) {
            out[n++] = g_observers[i];
        }
    }
    dsd_mutex_unlock(&g_observer_mu);
    return n;
}

void
dsd_app_telemetry_notify_state(const dsd_state* state) {
    const dsd_app_telemetry_observer* local[DSD_APP_TELEMETRY_MAX_OBSERVERS];
    const int n = snapshot_observers(local);
    for (int i = 0; i < n; i++) {
        if (local[i]->state != NULL) {
            local[i]->state(state, local[i]->user);
        }
    }
}

void
dsd_app_telemetry_notify_opts(const dsd_opts* opts) {
    const dsd_app_telemetry_observer* local[DSD_APP_TELEMETRY_MAX_OBSERVERS];
    const int n = snapshot_observers(local);
    for (int i = 0; i < n; i++) {
        if (local[i]->opts != NULL) {
            local[i]->opts(opts, local[i]->user);
        }
    }
}
