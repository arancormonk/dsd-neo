// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/channel_scan_hooks.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stddef.h>

#include "commands_internal.h"
#include "services.h"
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
    dsd_app_command_session_set_open(1);
    dsd_app_install_telemetry_hooks();
    if (initial_opts) {
        dsd_app_telemetry_publish_opts_snapshot(initial_opts);
    }
    if (initial_state) {
        dsd_app_telemetry_publish_snapshot(initial_state);
    }
    dsd_runtime_set_control_pump(dsd_app_frontend_control_pump);
    dsd_runtime_set_controls_pending(dsd_app_commands_pending);
    dsd_runtime_set_controls_need_frame_loop(dsd_app_commands_need_frame_loop);
    /* A -Y scan the engine pauses again on its own (a resume whose row never landed) records its return to the monitor
       as an interactive pause does (issue #671). */
    dsd_channel_scan_pause_hook_set(svc_pause_channel_scan);
    /* Whatever an earlier session left in the slot is not this session's; cleared before the sink can fill it. */
    dsd_app_stream_notice_clear();
    dsd_runtime_stream_notice_set(dsd_app_stream_notice_post);
}

void
dsd_app_frontend_runtime_stop(void) {
    dsd_app_command_session_set_open(0);
    dsd_runtime_set_control_pump(NULL);
    dsd_runtime_set_controls_pending(NULL);
    dsd_runtime_set_controls_need_frame_loop(NULL);
    dsd_channel_scan_pause_hook_set(NULL);
    /* A notice the session did not get to show is not the next session's. The sink is removed first: a post still
       delivering to it finishes before the removal returns, so the clear after it is the last word. */
    dsd_runtime_stream_notice_set(NULL);
    dsd_app_stream_notice_clear();
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){0});
    /* Clearing the hooks stops the feed but leaves the last record behind, and that
       record is a module static that outlives the session. On Android the service starts
       polling before the next session reaches dsd_app_frontend_runtime_start(), so a
       stale record would caption the new run with the old one's last call. */
    dsd_app_notification_reset();
    /* Likewise a Signal search status: a start the engine rejected has no coordinator to publish STOPPED, and a
       recreated frontend would otherwise restore the search screen over the next, ordinary session. */
    dsd_app_signal_search_session_ended();
}
