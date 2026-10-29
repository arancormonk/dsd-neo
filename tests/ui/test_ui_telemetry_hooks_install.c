// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for app-control telemetry hook installation.
 */

#include <assert.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/channel_scan_hooks.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stddef.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"
#include "snapshot_internal.h"

static dsd_telemetry_hooks g_hooks;
static int g_snapshot_calls;
static int g_opts_snapshot_calls;
static int g_notification_resets;
static int g_session_open;
static int g_search_session_ends;

void
dsd_app_command_session_set_open(int open) { // NOLINT(misc-use-internal-linkage)
    g_session_open = open;
}

void
dsd_app_notification_reset(void) { // NOLINT(misc-use-internal-linkage)
    g_notification_resets++;
}

void
dsd_app_signal_search_session_ended(void) { // NOLINT(misc-use-internal-linkage)
    g_search_session_ends++;
}

void
dsd_telemetry_hooks_set(dsd_telemetry_hooks hooks) { // NOLINT(misc-use-internal-linkage)
    g_hooks = hooks;
}

static dsd_controls_pending_fn g_controls_pending;
static dsd_controls_pending_fn g_controls_need_frame_loop;
static dsd_controls_pending_fn g_publish_pending;
static int g_snapshot_pending;
static int g_opts_snapshot_pending;
static int g_guard_busy;

void
dsd_runtime_set_publish_pending(dsd_controls_pending_fn fn) { // NOLINT(misc-use-internal-linkage)
    g_publish_pending = fn;
}

int
dsd_app_telemetry_snapshot_pending(void) { // NOLINT(misc-use-internal-linkage)
    return g_snapshot_pending;
}

int
dsd_app_telemetry_opts_snapshot_pending(void) { // NOLINT(misc-use-internal-linkage)
    return g_opts_snapshot_pending;
}

int
p25_sm_tick_guard_busy(void) { // NOLINT(misc-use-internal-linkage)
    return g_guard_busy;
}

void
dsd_runtime_set_control_pump(dsd_control_pump_fn fn) { // NOLINT(misc-use-internal-linkage)
    (void)fn;
}

void
dsd_runtime_set_controls_pending(dsd_controls_pending_fn fn) { // NOLINT(misc-use-internal-linkage)
    g_controls_pending = fn;
}

void
dsd_runtime_set_controls_need_frame_loop(dsd_controls_pending_fn fn) { // NOLINT(misc-use-internal-linkage)
    g_controls_need_frame_loop = fn;
}

/* The engine's own re-pause of a -Y scan goes through app-control's recording pause while a session runs (issue
   #671). */
static dsd_channel_scan_pause_fn g_channel_scan_pause;
static int g_channel_scan_pause_sets;

void
dsd_channel_scan_pause_hook_set(dsd_channel_scan_pause_fn fn) { // NOLINT(misc-use-internal-linkage)
    g_channel_scan_pause = fn;
    g_channel_scan_pause_sets++;
}

int
svc_pause_channel_scan(dsd_opts* opts, dsd_state* state, int in_place) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)in_place;
    return 0;
}

static dsd_stream_notice_fn g_stream_notice_sink;
static int g_stream_notice_clears;

void
dsd_runtime_stream_notice_set(dsd_stream_notice_fn fn) { // NOLINT(misc-use-internal-linkage)
    g_stream_notice_sink = fn;
}

void
dsd_app_stream_notice_post(int kind) { // NOLINT(misc-use-internal-linkage)
    (void)kind;
}

void
dsd_app_stream_notice_clear(void) { // NOLINT(misc-use-internal-linkage)
    g_stream_notice_clears++;
}

int
dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    return 0;
}

int
dsd_app_commands_need_frame_loop(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

int
dsd_app_commands_pending(void) { // NOLINT(misc-use-internal-linkage)
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

    /* Stopping must drop the published status with the hooks. It is a module static
       that outlives the session, and the Android service polls it again before the next
       session has published anything -- a stale record would caption the new run with
       the previous one's last call. */
    dsd_app_frontend_runtime_start(NULL, NULL);
    assert(g_session_open == 1);
    assert(g_notification_resets == 0 && g_search_session_ends == 0);
    /* A silent input's wait asks the queue whether a command waits (issue #634). */
    assert(g_controls_pending == dsd_app_commands_pending);
    assert(g_controls_need_frame_loop == dsd_app_commands_need_frame_loop);
    /* ... and whether a publication a watchdog tick put off waits for the decoder's next pass: once the tick has left
       the guard, when the pump's retry can take it. */
    assert(g_publish_pending != NULL && g_publish_pending() == 0);
    g_snapshot_pending = 1;
    assert(g_publish_pending() == 1);
    g_guard_busy = 1;
    assert(g_publish_pending() == 0);
    g_snapshot_pending = 0;
    g_opts_snapshot_pending = 1;
    assert(g_publish_pending() == 0);
    g_guard_busy = 0;
    assert(g_publish_pending() == 1);
    g_opts_snapshot_pending = 0;
    /* The stream's notices (a stalled RTL-SDR recovered) reach the decoder's drain while the session runs, from an
       empty slot: whatever an earlier session left there was cleared first. */
    assert(g_stream_notice_sink == dsd_app_stream_notice_post);
    assert(g_stream_notice_clears == 1);
    assert(g_channel_scan_pause == svc_pause_channel_scan && g_channel_scan_pause_sets == 1);
    dsd_app_frontend_runtime_stop();
    assert(g_channel_scan_pause == NULL && g_channel_scan_pause_sets == 2);
    /* ... and none is left for the next session. */
    assert(g_stream_notice_sink == NULL);
    assert(g_stream_notice_clears == 2);
    assert(g_notification_resets == 1);
    /* So does a Signal search status a rejected start left with no coordinator to stop it. */
    assert(g_search_session_ends == 1);
    assert(g_session_open == 0);
    assert(g_controls_pending == NULL);
    assert(g_controls_need_frame_loop == NULL);
    assert(g_publish_pending == NULL);
    assert(g_hooks.publish_snapshot == NULL);
    assert(g_hooks.publish_opts_snapshot == NULL);
    assert(g_hooks.request_redraw == NULL);
    return 0;
}
