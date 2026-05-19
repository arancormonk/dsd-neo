// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
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

static void
assert_render_fields(const dsd_state* snap) {
    dsd_tg_policy_lookup lookup;
    assert(snap != NULL);
    assert(snap->lasttg == 321);
    assert(snap->trunk_chan_map[0x1234] == 769768750L);
    assert(snap->trunk_chan_map_used_count == 1U);
    assert(snap->trunk_chan_map_used[0] == 0x1234U);
    assert(dsd_tg_policy_lookup_id(snap, 321U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lookup.entry.name, "DISPATCH") == 0);
    assert(strcmp(snap->ui_msg, "snapshot ready") == 0);
    assert(snap->rkey_array[7] == 0ULL);
}

static void
assert_cc_candidates(const dsd_state* snap) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(snap);
    assert(cc != NULL);
    assert(cc->count == 2);
    assert(cc->idx == 1);
    assert(cc->candidates[0] == 851006250L);
    assert(cc->candidates[1] == 852006250L);
    assert(cc->cool_until[1] == 12.5);
    assert(cc->added == 2U);
    assert(cc->used == 3U);
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
    state->lasttg = 321;
    dsd_state_set_trunk_chan_freq(state, 0x1234U, 769768750L);
    dsd_tg_policy_entry entry;
    assert(dsd_tg_policy_make_exact_entry(321U, "A", "DISPATCH", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);
    snprintf(state->ui_msg, sizeof(state->ui_msg), "%s", "snapshot ready");
    state->rkey_array[7] = 0x12345678ULL;
    assert(dsd_trunk_cc_candidates_add(state, 851006250L, 1) == 1);
    assert(dsd_trunk_cc_candidates_add(state, 852006250L, 1) == 1);
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    assert(cc != NULL);
    cc->idx = 1;
    cc->cool_until[1] = 12.5;
    cc->used = 3U;

    history[0].Event_History_Items[1].source_id = 123U;
    history[1].Event_History_Items[1].source_id = 456U;
    ui_terminal_telemetry_publish_snapshot(state);
    cc->count = 1;
    cc->candidates[0] = 999999999L;
    cc->added = 99U;
    const dsd_state* snap = ui_get_latest_snapshot();
    assert_slot_tail(snap, 123U, 456U);
    assert_render_fields(snap);
    assert_cc_candidates(snap);

    // Update only non-head rows; this must still refresh the snapshot copy.
    history[0].Event_History_Items[1].source_id = 789U;
    history[1].Event_History_Items[1].source_id = 987U;
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(ui_get_latest_snapshot(), 789U, 987U);

    // Reset-like clear with unchanged head rows must also be reflected.
    memset(history, 0, 2u * sizeof(*history));
    ui_terminal_telemetry_publish_snapshot(state);
    assert_slot_tail(ui_get_latest_snapshot(), 0U, 0U);

    assert(dsd_tg_policy_make_exact_entry(7777U, "B", "POLICY-ONLY", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);
    ui_terminal_telemetry_publish_snapshot(state);
    snap = ui_get_latest_snapshot();
    dsd_tg_policy_lookup lookup;
    assert(dsd_tg_policy_lookup_id(snap, 7777U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lookup.entry.name, "POLICY-ONLY") == 0);

    printf("UI_SNAPSHOT_EVENT_HISTORY: OK\n");
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    return 0;
}
