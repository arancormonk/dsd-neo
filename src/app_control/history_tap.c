// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include "snapshot_internal.h"

int
dsd_app_history_tap_copy(int slot, Event_History_I* out, dsd_app_history_tap_cursor* cursor,
                         dsd_app_history_tap_meta* meta) {
    if (out == NULL || cursor == NULL || slot < 0 || slot > 1) {
        return -1;
    }
    /* Options first, then the ring, each lock on its own (never nested), the order the frontend reads the two snapshots
       in (UiController::tick()). */
    if (meta != NULL) {
        dsd_app_opts_snapshot_history_meta(meta);
    }
    return dsd_app_snapshot_history_ring_copy(slot, out, cursor);
}
