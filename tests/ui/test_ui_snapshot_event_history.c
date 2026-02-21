// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/ui/ui_snapshot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

void ui_terminal_telemetry_publish_snapshot(const dsd_state* state);

static void
assert_slot_tail(const dsd_state* snap, uint32_t slot0_src, uint32_t slot1_src) {
    assert(snap != NULL);
    assert(snap->event_history_s != NULL);
    assert(snap->event_history_s[0].Event_History_Items[1].source_id == slot0_src);
    assert(snap->event_history_s[1].Event_History_Items[1].source_id == slot1_src);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    Event_History_I* history = (Event_History_I*)calloc(2u, sizeof(*history));
    if (!state || !history) {
        fprintf(stderr, "allocation failed\n");
        free(history);
        free(state);
        return 1;
    }
    state->event_history_s = history;

    history[0].Event_History_Items[1].source_id = 123U;
    history[1].Event_History_Items[1].source_id = 456U;
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(ui_get_latest_snapshot(), 123U, 456U);

    // Update only non-head rows; this must still refresh the snapshot copy.
    history[0].Event_History_Items[1].source_id = 789U;
    history[1].Event_History_Items[1].source_id = 987U;
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(ui_get_latest_snapshot(), 789U, 987U);

    // Reset-like clear with unchanged head rows must also be reflected.
    memset(history, 0, 2u * sizeof(*history));
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(ui_get_latest_snapshot(), 0U, 0U);

    printf("UI_SNAPSHOT_EVENT_HISTORY: OK\n");
    free(history);
    free(state);
    return 0;
}
