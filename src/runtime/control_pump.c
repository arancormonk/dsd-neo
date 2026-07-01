// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/control_pump.h>
#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_control_pump_fn g_control_pump_fn = NULL;
static dsd_mutex_t g_control_pump_mu;
static atomic_int g_control_pump_mu_state = 0; // 0=uninit, 1=initing, 2=init

static void
ensure_control_pump_mu_init(void) {
    if (atomic_load(&g_control_pump_mu_state) == 2) {
        return;
    }

    int expected = 0;
    if (atomic_compare_exchange_strong(&g_control_pump_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_control_pump_mu);
        atomic_store(&g_control_pump_mu_state, 2);
        return;
    }

    while (atomic_load(&g_control_pump_mu_state) != 2) {}
}

void
dsd_runtime_set_control_pump(dsd_control_pump_fn fn) {
    ensure_control_pump_mu_init();
    dsd_mutex_lock(&g_control_pump_mu);
    g_control_pump_fn = fn;
    dsd_mutex_unlock(&g_control_pump_mu);
}

void
dsd_runtime_pump_controls(dsd_opts* opts, dsd_state* state) {
    ensure_control_pump_mu_init();
    dsd_mutex_lock(&g_control_pump_mu);
    dsd_control_pump_fn fn = g_control_pump_fn;
    dsd_mutex_unlock(&g_control_pump_mu);
    if (!fn) {
        return;
    }
    fn(opts, state);
}
