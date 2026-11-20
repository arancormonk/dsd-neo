// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The engine commits the session's last call and publishes it before it calls the frontends' stop hooks, so a
 * frontend that records calls (the control API's event subscribers) gets the final transmission of an orderly
 * shutdown instead of losing it to the cleanup that runs after the hooks.
 *
 * A Signal search visit tag (issue #809) lasts through those final closes and is cleared after the last one, and a
 * session starts with none, whatever an earlier session left.
 *
 * A frontend with a start hook and no stop hook (Android) still gets the final commit published before the cleanup
 * (bug B1, issue #800); a session whose start hook fails gets no lifecycle publish and no stop callback, and the
 * cleanup still commits its last call. No session commits its last call twice.
 *
 * The final publish happens with the exit flag already set: the published position feed's clock sample reads the run
 * as stopped from that publish on (issue #800), even for a run that ended on its own with the flag clear, and also for
 * a run with no lifecycle hooks whose embedder attached the telemetry itself.
 */

#include <assert.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static uint64_t g_push_at_start = 0U;
static int g_started = 0;
static int g_published_after_commit = 0;
static int g_stop_calls = 0;

static void
count_publish(const dsd_state* state) {
    if (g_started && state != NULL && state->event_history_s != NULL
        && state->event_history_s[0].push_seq > g_push_at_start) {
        g_published_after_commit = 1;
    }
}

static int
visit_tagged(uint64_t session, uint64_t hit) {
    uint64_t tag_session = 0, tag_hit = 0;
    return dsd_signal_search_visit_get(&tag_session, &tag_hit) == 1 && tag_session == session && tag_hit == hit;
}

static int
start_with_active_call(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    /* The tag an earlier session left is gone; this session's search visit is on the air when the session ends. */
    assert(dsd_signal_search_visit_get(NULL, NULL) == 0);
    dsd_signal_search_visit_set(7, 8);
    dsd_call_observation observation = dsd_call_observation_data(DSD_SYNC_P25P1_POS, 0, 123, 456);
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.observed_m = 10;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0);
    g_push_at_start = state->event_history_s[0].push_seq;
    g_started = 1;
    /* Leave the decode loop at once: the call is still on air when the session ends. */
    dsd_exitflag_store(1);
    return 0;
}

/* A frontend that fails to start after the call went on air: the session never reaches its decode loop. */
static int
start_fails_with_active_call(dsd_opts* opts, dsd_state* state, void* context) {
    (void)start_with_active_call(opts, state, context);
    return -1;
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
    /* Nothing cleared the tag before the final closes. */
    assert(visit_tagged(7, 8));
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} session;

static session
session_open(void) {
    session s;
    s.opts = (dsd_opts*)calloc(1, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    initOpts(s.opts);
    initState(s.state);
    DSD_SNPRINTF(s.opts->audio_in_dev, sizeof(s.opts->audio_in_dev), "%s", "m17udp");
    DSD_SNPRINTF(s.opts->audio_out_dev, sizeof(s.opts->audio_out_dev), "%s", "null");
    s.opts->audio_in_type = AUDIO_IN_NULL;
    s.opts->audio_out_type = 9;
    g_push_at_start = 0U;
    g_started = 0;
    g_published_after_commit = 0;
    g_stop_calls = 0;
    return s;
}

static void
session_close(session* s) {
    freeState(s->state);
    free(s->state);
    free(s->opts);
}

/* The session's last call was committed exactly once: one push since the start hook, and the row behind it is another
 * call's. */
static void
assert_committed_once(const dsd_state* state) {
    assert(state->event_history_s[0].push_seq == g_push_at_start + 1U);
    assert(state->event_history_s[0].Event_History_Items[1].target_id == 456U);
    assert(state->event_history_s[0].Event_History_Items[2].target_id != 456U);
}

static void
test_start_and_stop_hooks(void) {
    session s = session_open();
    const dsd_engine_lifecycle_hooks hooks = {.start = start_with_active_call, .stop = stop_sees_committed_call};
    dsd_signal_search_visit_set(5, 6);
    (void)dsd_engine_run_with_lifecycle(s.opts, s.state, &hooks);
    assert(g_stop_calls == 1);
    assert_committed_once(s.state);
    /* The teardown cleared the tag after the session's last recording close. */
    assert(dsd_signal_search_visit_get(NULL, NULL) == 0);
    session_close(&s);
}

static void
test_start_hook_only(void) {
    session s = session_open();
    const dsd_engine_lifecycle_hooks hooks = {.start = start_with_active_call};
    (void)dsd_engine_run_with_lifecycle(s.opts, s.state, &hooks);
    /* No stop hook to call, yet the final commit was published before the cleanup. */
    assert(g_stop_calls == 0);
    assert(g_published_after_commit == 1);
    assert_committed_once(s.state);
    assert(dsd_signal_search_visit_get(NULL, NULL) == 0);
    session_close(&s);
}

static void
test_start_failure(void) {
    session s = session_open();
    const dsd_engine_lifecycle_hooks hooks = {.start = start_fails_with_active_call, .stop = stop_sees_committed_call};
    assert(dsd_engine_run_with_lifecycle(s.opts, s.state, &hooks) != 0);
    /* The lifecycle never started: no stop callback and no lifecycle publish; the cleanup still committed the call. */
    assert(g_started == 1);
    assert(g_stop_calls == 0);
    assert(g_published_after_commit == 0);
    assert_committed_once(s.state);
    session_close(&s);
}

/* A frontend attaching as the CLI and Android do: the app-control runtime's first publish starts the run's sample. */
static int
start_app_control(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    g_started = 1;
    dsd_app_frontend_runtime_start(opts, state);
    dsd_app_position_cursor cursor;
    assert(dsd_app_position_copy_since(0U, 0U, NULL, 0U, &cursor) == 0);
    assert(cursor.stream_id != 0U && cursor.stream_id == dsd_position_feed_stream_id(state));
    assert(cursor.sample.stream_id == cursor.stream_id && cursor.sample.running == 1U);
    return 0;
}

/* A run that ends on its own (--playfiles with nothing to play), with the exit flag never set before the engine's own
   end: the final publish still reads the run as stopped, because the engine sets the flag before it publishes. */
static void
test_final_publish_stops_position_sample(void) {
    session s = session_open();
    s.opts->playfiles = 1;
    s.state->cli_argc_effective = 0;
    s.state->cli_argv = NULL;
    const dsd_engine_lifecycle_hooks hooks = {.start = start_app_control};
    (void)dsd_engine_run_with_lifecycle(s.opts, s.state, &hooks);
    assert(g_started == 1);
    dsd_app_position_cursor cursor;
    assert(dsd_app_position_copy_since(0U, 0U, NULL, 0U, &cursor) == 0);
    assert(cursor.stream_id != 0U && cursor.sample.stream_id == cursor.stream_id);
    assert(cursor.sample.running == 0U);
    dsd_app_frontend_runtime_stop();
    dsd_exitflag_store(0);
    session_close(&s);
}

/* An embedder that attaches app-control telemetry itself and runs with no lifecycle hooks: the run still publishes its
   final state, the last records and the stopped sample, before the cleanup frees the feed (issue #800). */
static void
test_final_publish_without_start_hook(void) {
    session s = session_open();
    s.opts->playfiles = 1;
    s.state->cli_argc_effective = 0;
    s.state->cli_argv = NULL;
    dsd_app_frontend_runtime_start(s.opts, s.state);
    (void)dsd_engine_run_with_lifecycle(s.opts, s.state, NULL);
    /* The last publish is the run's own: its feed stream, read as stopped. */
    dsd_app_position_cursor cursor;
    assert(dsd_app_position_copy_since(0U, 0U, NULL, 0U, &cursor) == 0);
    assert(cursor.stream_id != 0U && cursor.sample.stream_id == cursor.stream_id);
    assert(cursor.sample.running == 0U);
    dsd_app_frontend_runtime_stop();
    dsd_exitflag_store(0);
    session_close(&s);
}

int
main(void) {
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){.publish_snapshot = count_publish});
    test_start_and_stop_hooks();
    test_start_hook_only();
    test_start_failure();
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){0});
    test_final_publish_stops_position_sample();
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){0});
    test_final_publish_without_start_hook();
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){0});
    printf("engine lifecycle finalize test passed\n");
    return 0;
}
