// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdlib.h>
#include "../../src/app_control/snapshot_internal.h"

/* Exercise the engine's observation -> event sync -> telemetry publication path
 * with real event and snapshot implementations, without a UI or an RF fixture. */
int
main(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_opts* opts = calloc(1, sizeof(*opts));
    Event_History_I* history = calloc(2, sizeof(*history));
    assert(state && opts && history);
    state->event_history_s = history;
    init_event_history(&history[0], 0, 255);
    init_event_history(&history[1], 0, 255);
    state->synctype = state->lastsynctype = DSD_SYNC_P25P1_POS;
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0, 123, 456);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = 10;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0);
    assert(history[0].Event_History_Items[0].target_id == 456);
    assert(history[0].Event_History_Items[0].emergency == 0);

    observation.has_service_metadata = 1;
    observation.emergency = 1;
    observation.priority = 3;
    observation.observed_m = 11;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) >= 0);
    dsd_event_sync_slot(opts, state, 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 0, &call) > 0);
    assert(call.emergency == 1 && call.priority == 3);
    assert(history[0].Event_History_Items[0].emergency == 1);
    assert(history[0].Event_History_Items[0].priority == 3);
    dsd_app_telemetry_publish_snapshot(state);
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();
    assert(snapshot && snapshot->event_history_s);
    assert(dsd_call_state_get(snapshot, 0, &call) > 0);
    assert(call.emergency == 1 && call.priority == 3);
    assert(snapshot->event_history_s[0].Event_History_Items[0].emergency == 1);
    assert(snapshot->event_history_s[0].Event_History_Items[0].priority == 3);

    assert(dsd_call_state_end_ex(state, 0, 12, DSD_CALL_END_SYNC_LOSS) > 0);
    dsd_event_sync_slot(opts, state, 0);
    assert(history[0].Event_History_Items[1].target_id == 456);
    assert(history[0].Event_History_Items[1].emergency == 1);
    /* A reacquired fragment lacking emergency must not clear the committed row. */
    observation.emergency = 0;
    observation.priority = 0;
    observation.observed_m = 12.5;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) >= 0);
    dsd_event_sync_slot(opts, state, 0);
    assert(dsd_call_state_end(state, 0, 13) > 0);
    dsd_event_sync_slot(opts, state, 0);
    assert(history[0].push_seq == 1);
    assert(history[0].Event_History_Items[1].emergency == 1);
    assert(history[0].Event_History_Items[1].priority == 3);

    observation = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0, 789, 999);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = 20;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0);
    dsd_app_telemetry_publish_snapshot(state);
    snapshot = dsd_app_get_latest_snapshot();
    assert(dsd_call_state_get(snapshot, 0, &call) > 0);
    assert(call.ota_target_id == 999 && call.emergency == 0 && call.priority == 0);
    assert(snapshot->event_history_s[0].Event_History_Items[0].target_id == 999);
    assert(snapshot->event_history_s[0].Event_History_Items[0].emergency == 0);
    assert(snapshot->event_history_s[0].Event_History_Items[0].priority == 0);
    assert(snapshot->event_history_s[0].Event_History_Items[1].emergency == 1);
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    free(opts);
    return 0;
}
