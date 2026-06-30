// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/telemetry.h>

#include <dsd-neo/app_control/snapshot.h>

#include "telemetry_hooks_impl.h"

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
dsd_app_consume_redraw_requested(void) {
    return atomic_exchange(&g_redraw_requested, 0);
}

void
ui_terminal_telemetry_request_redraw(void) {
    dsd_app_request_redraw();
}

void
ui_terminal_install_telemetry_hooks(void) {
    dsd_app_install_telemetry_hooks();
}
