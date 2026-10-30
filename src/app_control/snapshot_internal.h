// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_SNAPSHOT_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_SNAPSHOT_INTERNAL_H_

#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>

#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Public frontend surface, declared elsewhere: the snapshot read accessors live in
 * <dsd-neo/app_control/snapshot.h> and dsd_app_request_redraw() in
 * <dsd-neo/app_control/frontend_runtime.h>. */

void dsd_app_install_telemetry_hooks(void);
void dsd_app_telemetry_publish_snapshot(const dsd_state* state);
void dsd_app_telemetry_publish_opts_snapshot(const dsd_opts* opts);

/* Whether a publication found the P25 SM tick guard busy and was put off (1), until the next publish of the same
 * snapshot: the drain retries it at its start (dsd_app_drain_cmds()), and the frame-sync hunt publishes every 50 ms. */
int dsd_app_telemetry_snapshot_pending(void);
int dsd_app_telemetry_opts_snapshot_pending(void);

/* How a publication stands with the P25 SM tick guard: the watchdog's tick writes what it copies under that guard.
 * Publishing never waits for a tick: a publisher already under the guard (a frame dispatch) uses that hold, since the
 * guard is not re-entrant; otherwise it tries once, and a busy guard puts the publication off. */
typedef enum {
    DSD_APP_PUBLISH_GUARD_BUSY = 0,
    DSD_APP_PUBLISH_GUARD_HELD,
    DSD_APP_PUBLISH_GUARD_TAKEN,
} dsd_app_publish_guard;

static inline dsd_app_publish_guard
dsd_app_publish_guard_try(void) {
    if (p25_sm_tick_guard_held_by_caller()) {
        return DSD_APP_PUBLISH_GUARD_HELD;
    }
    return p25_sm_tick_guard_try_enter() ? DSD_APP_PUBLISH_GUARD_TAKEN : DSD_APP_PUBLISH_GUARD_BUSY;
}

static inline void
dsd_app_publish_guard_leave(dsd_app_publish_guard guard) {
    if (guard == DSD_APP_PUBLISH_GUARD_TAKEN) {
        p25_sm_tick_guard_leave();
    }
}

/* Telemetry observer dispatch (<dsd-neo/app_control/telemetry_observers.h>), called by the two publishers above on
 * the decode thread after they release their own locks. */
void dsd_app_telemetry_notify_state(const dsd_state* state);
void dsd_app_telemetry_notify_opts(const dsd_opts* opts);

/* The history tap's two halves (history_tap.c), each under its own publisher's lock and neither touching the
 * single-consumer buffers: the ring copy as dsd_app_history_tap_copy() returns it, and the options' attribution fields
 * (opts_published 0, the rest 0, before the first options snapshot). */
int dsd_app_snapshot_history_ring_copy(int slot, Event_History_I* out, dsd_app_history_tap_cursor* cursor);
void dsd_app_opts_snapshot_history_meta(dsd_app_history_tap_meta* meta);

#ifdef DSD_NEO_TEST_HOOKS
typedef struct {
    uint64_t source_to_published[2];
    uint64_t published_to_consumer[2];
} dsd_app_snapshot_event_history_copy_counts;

void dsd_app_snapshot_test_reset_event_history_copy_counts(void);
void dsd_app_snapshot_test_get_event_history_copy_counts(dsd_app_snapshot_event_history_copy_counts* counts);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_SNAPSHOT_INTERNAL_H_ */
