// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for app-control telemetry hook installation.
 */

#include <assert.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stddef.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "snapshot_internal.h"

static dsd_telemetry_hooks g_hooks;
static int g_snapshot_calls;
static int g_opts_snapshot_calls;

void
dsd_telemetry_hooks_set(dsd_telemetry_hooks hooks) { // NOLINT(misc-use-internal-linkage)
    g_hooks = hooks;
}

void
dsd_runtime_set_control_pump(dsd_control_pump_fn fn) { // NOLINT(misc-use-internal-linkage)
    (void)fn;
}

int
dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    return 0;
}

void
dsd_app_telemetry_publish_snapshot(const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    g_snapshot_calls++;
}

void
dsd_app_telemetry_publish_opts_snapshot(const dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    g_opts_snapshot_calls++;
}

int
main(void) {
    dsd_app_install_telemetry_hooks();

    assert(g_hooks.publish_snapshot == dsd_app_telemetry_publish_snapshot);
    assert(g_hooks.publish_opts_snapshot == dsd_app_telemetry_publish_opts_snapshot);
    assert(g_hooks.request_redraw == dsd_app_request_redraw);

    g_hooks.publish_snapshot(NULL);
    g_hooks.publish_opts_snapshot(NULL);
    g_hooks.request_redraw();

    assert(g_snapshot_calls == 1);
    assert(g_opts_snapshot_calls == 1);
    assert(dsd_app_frontend_redraw_consume() == 1);
    assert(dsd_app_frontend_redraw_consume() == 0);

    dsd_app_request_redraw();
    assert(dsd_app_frontend_redraw_consume() == 1);

    dsd_app_install_telemetry_hooks();
    assert(g_hooks.request_redraw == dsd_app_request_redraw);
    return 0;
}
