// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The engine commits the session's last call and publishes it before it calls the frontends' stop hooks, so a
 * frontend that records calls (the control API's event subscribers) gets the final transmission of an orderly
 * shutdown instead of losing it to the cleanup that runs after the hooks.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static uint64_t g_push_at_start = 0U;
static int g_published_after_commit = 0;
static int g_stop_calls = 0;

static void
count_publish(const dsd_state* state) {
    if (state != NULL && state->event_history_s != NULL && state->event_history_s[0].push_seq > g_push_at_start) {
        g_published_after_commit = 1;
    }
}

static int
start_with_active_call(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0, 123, 456);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = 10;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0);
    g_push_at_start = state->event_history_s[0].push_seq;
    /* Leave the decode loop at once: the call is still on air when the session ends. */
    dsd_exitflag_store(1);
    return 0;
}

static void
stop_sees_committed_call(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)context;
    g_stop_calls++;
    /* The last call was committed (a push on its slot) and published before the frontends were stopped. */
    assert(state->event_history_s[0].push_seq > g_push_at_start);
    assert(state->event_history_s[0].Event_History_Items[1].target_id == 456U);
    assert(g_published_after_commit == 1);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof(opts->audio_out_dev), "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){.publish_snapshot = count_publish});

    const dsd_engine_lifecycle_hooks hooks = {.start = start_with_active_call, .stop = stop_sees_committed_call};
    (void)dsd_engine_run_with_lifecycle(opts, state, &hooks);
    assert(g_stop_calls == 1);

    dsd_telemetry_hooks_set((dsd_telemetry_hooks){0});
    freeState(state);
    free(state);
    free(opts);
    printf("engine lifecycle finalize test passed\n");
    return 0;
}
