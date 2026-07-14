// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stddef.h>

#include "commands_internal.h"
#include "snapshot_internal.h"

static atomic_int g_redraw_requested = 0;

void
dsd_app_install_telemetry_hooks(void) {
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){
        .publish_snapshot = dsd_app_telemetry_publish_snapshot,
        .publish_opts_snapshot = dsd_app_telemetry_publish_opts_snapshot,
        .request_redraw = dsd_app_request_redraw,
    });
}

void
dsd_app_request_redraw(void) {
    atomic_store(&g_redraw_requested, 1);
}

int
dsd_app_frontend_redraw_consume(void) {
    return atomic_exchange(&g_redraw_requested, 0);
}

static void
dsd_app_frontend_control_pump(dsd_opts* opts, dsd_state* state) {
    (void)dsd_app_drain_cmds(opts, state);
}

void
dsd_app_frontend_runtime_start(const dsd_opts* initial_opts, const dsd_state* initial_state) {
    dsd_app_install_telemetry_hooks();
    if (initial_opts) {
        dsd_app_telemetry_publish_opts_snapshot(initial_opts);
    }
    if (initial_state) {
        dsd_app_telemetry_publish_snapshot(initial_state);
    }
    dsd_runtime_set_control_pump(dsd_app_frontend_control_pump);
}

void
dsd_app_frontend_runtime_stop(void) {
    dsd_runtime_set_control_pump(NULL);
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){0});
}
