// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The engine's side of TETRA trunking, with the real state machine and a stand-in tuner: the tick the decode loop runs
 * (dsd_engine_tetra_trunk_tick_locked()) settles a tune the backend reports in flight -- committing a completion,
 * putting a failure back and opening the frame gate either way -- and runs the hangtime; a carrier boundary drops the
 * followed call without tuning and forgets the carrier while the TETRA state stays allocated; the no-carrier pass
 * forgets the carrier's evidence.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "tetra_confirm.h"
#include "tetra_state.h"

enum {
    CC_HZ = 380012500,
    VC_HZ = 390012500,
};

static int g_failures;
static int g_tunes;
static int g_returns;
static int g_ted_sps;
static uint64_t g_request;
static dsd_trunk_tune_result g_tune_result;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    g_tunes++;
    g_ted_sps = ted_sps;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_tune_result)) {
        opts->trunk_is_tuned = 1;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
    return g_tune_result;
}

static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)state;
    g_returns++;
    g_request = request_id;
    opts->trunk_is_tuned = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
at_seconds(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * 1e9));
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} session;

/* A TETRA trunking session on a control channel a CRC-passed block proved. */
static session
session_open(void) {
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    if (!s.opts || !s.state) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        exit(1);
    }
    initOpts(s.opts);
    initState(s.state);
    s.opts->frame_tetra = 1;
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 1.0F;
    s.opts->audio_in_type = AUDIO_IN_PULSE;
    s.state->trunk_cc_freq = CC_HZ;
    s.state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    g_tunes = 0;
    g_returns = 0;
    g_ted_sps = -1;
    g_request = 0U;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    at_seconds(2000.0);
    tetra_sm_init(s.opts, s.state);
    tetra_sm_on_cc_sync(s.opts, s.state);
    tetra_confirm_note_signalling(s.state);
    return s;
}

static void
session_close(session* s) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    freeState(s->state);
    free(s->state);
    free(s->opts);
}

static int
follow_grant(session* s) {
    const tetra_sm_grant grant = {VC_HZ, 40U, 0x4U, 0x123U};
    return tetra_sm_on_grant(s->opts, s->state, &grant);
}

static void
engine_tick(session* s) {
    p25_sm_tick_guard_enter();
    dsd_engine_tetra_trunk_tick_locked(s->opts, s->state);
    p25_sm_tick_guard_leave();
}

/* A tune in flight holds the frame gate until the engine's tick settles it: a completion commits TUNED, a failure puts
 * the control channel back first. Either way the gate opens. */
static void
test_tick_settles_tunes(void) {
    session s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    expect(follow_grant(&s) == 1, "grant accepted in flight");
    expect(g_ted_sps == 0, "a TETRA grant tunes without a symbol-timing override");
    const uint64_t request = g_request;
    expect(!dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1), "tune in flight gates frames");
    engine_tick(&s);
    expect(dsd_trunk_tuning_pending_request() == request, "an unanswered tune stays in flight");

    /* Not a TETRA session, or trunk scan owns the tuner: the engine leaves the state machine alone. */
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    const tetra_state_t* const ts = tetra_st_existing(s.state);
    s.opts->frame_tetra = 0;
    engine_tick(&s);
    expect(ts->sm.pending_request_id == request, "no tick outside a TETRA session");
    s.opts->frame_tetra = 1;
    s.opts->trunk_scan_enabled = 1;
    engine_tick(&s);
    expect(ts->sm.pending_request_id == request, "no tick under trunk scan");
    s.opts->trunk_scan_enabled = 0;

    engine_tick(&s);
    expect(ts->sm.pending_request_id == 0U && dsd_trunk_tuning_pending_request() == 0U, "completion retired");
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.opts->trunk_is_tuned == 1, "completion commits TUNED");
    expect(dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1), "frames dispatch after it");
    session_close(&s);

    s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    expect(follow_grant(&s) == 1, "second grant accepted in flight");
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    expect(!dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1), "a failed tune keeps the gate");
    engine_tick(&s);
    expect(dsd_trunk_tuning_pending_request() == 0U, "failure retired");
    expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, NULL), "failure restores");
    expect(s.opts->trunk_is_tuned == 0 && s.state->trunk_vc_freq[0] == 0, "failure restores the shared tuning state");
    expect(dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1), "frames dispatch after it too");

    /* A tune the backend refuses outright changes nothing. */
    g_tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    expect(follow_grant(&s) == 0 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC, "refused tune changes nothing");
    expect(dsd_trunk_tuning_pending_request() == 0U, "refused tune leaves no request");
    session_close(&s);
}

/* The engine's tick runs the grant timeout and the hangtime after speech. */
static void
test_tick_runs_timers(void) {
    session s = session_open();
    expect(follow_grant(&s) == 1 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED, "grant followed");
    at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S - 0.5);
    tetra_sm_note_voice(s.state, 2U);
    at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 0.5);
    engine_tick(&s);
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0, "speech keeps the channel");
    at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S - 0.5 + TETRA_SM_SPEECH_GAP_S + 1.0);
    engine_tick(&s);
    expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1, "hangtime returns to the control channel");
    session_close(&s);
}

/* A carrier boundary (a manual tune) drops the followed call without tuning and forgets the carrier; the TETRA state
 * stays allocated for the decoder holding it. */
static void
test_carrier_boundary_abandons(void) {
    session s = session_open();
    expect(follow_grant(&s) == 1, "grant followed before the boundary");
    tetra_state_t* const ts = tetra_st_existing(s.state);
    tetra_confirm_note_signalling(s.state);
    ts->tdma_valid = 1;
    ts->mac_frag[1].active = 1;
    dsd_engine_carrier_boundary(s.opts, s.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    expect(tetra_st_existing(s.state) == ts, "the TETRA state stays allocated");
    expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, NULL), "call abandoned");
    expect(g_returns == 0 && g_tunes == 1, "abandoned without tuning");
    expect(s.opts->trunk_is_tuned == 0, "shared tuning state released");
    expect(ts->tdma_valid == 0 && !ts->mac_frag[1].active && !tetra_confirm_carrier_proven(s.state),
           "the carrier's clock, fragments and evidence are forgotten");
    session_close(&s);
}

/* The no-carrier pass forgets what proved the carrier, as for every other confirmation gate. */
static void
test_no_carrier_forgets_evidence(void) {
    session s = session_open();
    tetra_state_t* const ts = tetra_st_existing(s.state);
    ts->tdma_valid = 1;
    ts->mac_frag[0].active = 1;
    expect(tetra_confirm_carrier_proven(s.state), "evidence before the pass");
    dsd_engine_reset_no_carrier_state(s.opts, s.state);
    expect(tetra_st_existing(s.state) == ts, "the TETRA state stays allocated");
    expect(!tetra_confirm_carrier_proven(s.state) && ts->tdma_valid == 0 && !ts->mac_frag[0].active,
           "no-carrier pass forgets the carrier");
    expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC, "the control channel is still the one to return to");
    session_close(&s);
}

int
main(void) {
    dsd_decode_clock_use_test(2000ULL * 1000000000ULL);
    test_tick_settles_tunes();
    test_tick_runs_timers();
    test_carrier_boundary_abandons();
    test_no_carrier_forgets_evidence();
    dsd_decode_clock_use_system();
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "ENGINE_TETRA_TRUNK: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
