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
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
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
static dsd_trunk_tune_result g_return_result;

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

/* As the engine's return (dsd_engine_return_to_cc_request()): a return accepted, completed or in flight, releases the
 * shared tuning state at once. */
static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_returns++;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_return_result)) {
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return g_return_result;
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
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
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

/* A return of the machine's own still in flight when the operator moves the receiver (a tune, a source change, an input
 * switch) goes with the call it left: the machine already rests on the control channel and the return released the
 * shared tuning state, so only its request in flight says the machine owns the return (tetra_sm_owns_return()). Once
 * the boundary let go of it, the request's later failure puts nothing back on the operator's carrier: no follow of the
 * voice channel left, no claim on the shared tuning state, and no return to the old control channel at its hangtime. */
static void
test_carrier_boundary_abandons_a_return_in_flight(void) {
    static const dsd_carrier_boundary_kind kinds[] = {DSD_CARRIER_BOUNDARY_TUNE, DSD_CARRIER_BOUNDARY_SOURCE,
                                                      DSD_CARRIER_BOUNDARY_INPUT_SWITCH};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        session s = session_open();
        expect(follow_grant(&s) == 1 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED, "in flight: grant followed");
        g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 0.5);
        engine_tick(&s);
        const uint64_t request = g_request;
        expect(g_returns == 1 && request != 0U && tetra_sm_owns_return(s.state), "in flight: the return is in flight");
        expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0,
               "in flight: the machine rests on the control channel, the shared tuning state released");
        dsd_engine_carrier_boundary(s.opts, s.state, kinds[i], 0);
        expect(!tetra_sm_owns_return(s.state), "in flight: the boundary lets go of the return");
        dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
        engine_tick(&s);
        expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, NULL),
               "in flight: its failure brings no follow back");
        expect(s.opts->trunk_is_tuned == 0 && s.state->trunk_vc_freq[0] == 0 && s.state->p25_vc_freq[0] == 0,
               "in flight: nor the voice channel's shared tuning state");
        expect(g_returns == 1 && !tetra_sm_owns_return(s.state), "in flight: nor a return of its own");
        expect(dsd_trunk_tuning_pending_request() == 0U
                   && dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1),
               "in flight: and it holds the frame gate shut no longer");
        at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 30.0);
        engine_tick(&s);
        expect(g_returns == 1 && g_tunes == 1, "in flight: nor one later");
        session_close(&s);
    }
}

/* A grant's tune still in flight at a carrier boundary is let go of with the call (tetra_sm_abandon_carrier()): the
 * backend still owns it, so the frame gate waits for its answer. One that fails later has nothing to put back, and the
 * runtime retires it once the backend answers (dsd_trunk_tuning_request_disown()), so trunking goes on decoding rather
 * than wait for a tune nobody makes; one that succeeds later published its own tuner boundary, and the machine follows
 * nothing for it. */
static void
test_carrier_boundary_retires_an_abandoned_tune_once_it_fails(void) {
    for (int fails = 1; fails >= 0; fails--) {
        session s = session_open();
        g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        expect(follow_grant(&s) == 1 && tetra_sm_owns_return(s.state), "abandoned: the grant's tune in flight");
        const uint64_t request = g_request;
        dsd_engine_carrier_boundary(s.opts, s.state, DSD_CARRIER_BOUNDARY_TUNE, 0);
        expect(!tetra_sm_owns_return(s.state) && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC,
               "abandoned: the boundary lets go of the call and its tune");
        expect(dsd_trunk_tuning_pending_request() == request, "abandoned: the backend's answer is still awaited");
        engine_tick(&s);
        expect(dsd_trunk_tuning_pending_request() == request, "abandoned: an unanswered tune stays the backend's");
        dsd_trunk_tuning_request_publish(request, fails ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_OK);
        engine_tick(&s);
        expect(dsd_trunk_tuning_pending_request() == 0U
                   && dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1),
               fails ? "abandoned: a failure is retired" : "abandoned: a success is its own tuner boundary");
        expect(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, NULL)
                   && s.opts->trunk_is_tuned == 0,
               "abandoned: either way the machine follows nothing for it");
        session_close(&s);
    }
}

/* A request of the machine's in flight (its grant's tune, or its return) when the operator changes the decode mode:
 * the mode change drops the TETRA state, the machine with it, and the request stays the backend's, so the frame gate
 * waits for its answer. A failure answered after the change has no owner left to put anything back, and trunking stays
 * on, so nothing retires failed requests for an inactive tuning owner either: the runtime retires it as soon as the
 * backend answers, whether the session is still in the other mode or has come back to TETRA, whose new state machine
 * knows nothing of it. A success is its own tuner boundary. */
static void
test_mode_change_leaves_a_request_in_flight_to_the_runtime(void) {
    for (int returning = 0; returning <= 1; returning++) {
        for (int back = 0; back <= 1; back++) {
            for (int fails = 1; fails >= 0; fails--) {
                session s = session_open();
                g_tune_result = returning ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_PENDING;
                expect(follow_grant(&s) == 1, "mode change: grant followed");
                if (returning) {
                    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
                    at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 0.5);
                    engine_tick(&s);
                }
                const uint64_t request = g_request;
                expect(request != 0U && tetra_sm_owns_return(s.state) && dsd_trunk_tuning_pending_request() == request,
                       "mode change: the machine's request is in flight");
                expect(dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, s.opts, s.state)
                               == 0
                           && s.opts->frame_tetra == 0 && tetra_st_existing(s.state) == NULL,
                       "mode change: the TETRA state goes with the mode");
                expect(s.opts->trunk_enable == 1, "mode change: trunking stays on");
                expect(dsd_trunk_tuning_pending_request() == request
                           && !dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1),
                       "mode change: the backend's answer is still awaited");
                if (back) {
                    expect(
                        dsd_apply_decode_mode_preset(DSDCFG_MODE_TETRA, DSD_DECODE_PRESET_PROFILE_CLI, s.opts, s.state)
                                == 0
                            && s.opts->frame_tetra == 1,
                        "mode change: back to TETRA");
                    engine_tick(&s);
                    expect(dsd_trunk_tuning_pending_request() == request,
                           "mode change: back on TETRA the unanswered request is still the backend's");
                }
                const uint64_t generation = dsd_trunk_tuning_generation();
                dsd_trunk_tuning_request_publish(request,
                                                 fails ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_OK);
                engine_tick(&s);
                expect(dsd_trunk_tuning_pending_request() == 0U
                           && dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1),
                       fails ? "mode change: the failure no longer holds the frame gate shut"
                             : "mode change: the success opens the frame gate");
                expect(dsd_trunk_tuning_generation() == generation + (fails ? 0U : 1U),
                       fails ? "mode change: a failure is no tuner boundary"
                             : "mode change: a success is its own tuner boundary");
                expect(tetra_sm_get_state(s.state) != TETRA_SM_TUNED && !tetra_sm_followed(s.state, NULL),
                       "mode change: nothing is followed for it");
                session_close(&s);
            }
        }
    }
}

/* The engine's own return (dsd_engine_return_to_cc_request()), counted. */
static dsd_trunk_tune_result
engine_return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_returns++;
    return dsd_engine_return_to_cc_request(opts, state, request_id);
}

/* Trunking turned off while following (the trunk toggle clears trunk_enable and nothing else) leaves the receiver on
 * the traffic channel, as DMR's and P25's machines leave it, which do not tick while trunking is off. The engine's
 * return moves no tuner then and still answers OK, so the machine asks it for nothing: it keeps the channel, at its tick
 * and at a release of the followed call alike, reports no return, and keeps the evidence of the carrier the receiver
 * never left; it lets go of the call, whose speech and signalling are decoded there as with trunking off. With trunking
 * on again its grant timeout runs as before and asks the engine's return, which tunes through the backend: an audio
 * input has none here, so the return is refused and the channel kept for the next tick. */
static void
test_trunking_off_leaves_the_receiver(void) {
    session s = session_open();
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = engine_return_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    expect(follow_grant(&s) == 1 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED, "off: grant followed");
    tetra_confirm_note_signalling(s.state);
    tetra_confirm_note_followed_call(s.state);

    s.opts->trunk_enable = 0;
    engine_tick(&s);
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_sm_followed(s.state, NULL),
           "off: the machine keeps the channel");
    expect(!tetra_sm_followed_call_id(s.state, NULL) && !tetra_confirm_followed_call_proven(s.state),
           "off: and lets go of the call");
    expect(g_returns == 0 && s.opts->trunk_is_tuned == 1 && s.state->trunk_vc_freq[0] == VC_HZ,
           "off: no return is asked for, and the receiver is still the traffic channel's");
    expect(tetra_confirm_carrier_proven(s.state), "off: the evidence of the carrier the receiver is on stays");
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0 && s.opts->trunk_is_tuned == 1,
           "off: a release of the followed call asks for no return either");
    at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 30.0);
    engine_tick(&s);
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0, "off: nor does the grant timeout");

    s.opts->trunk_enable = 1;
    engine_tick(&s);
    expect(g_returns == 1, "on again: the grant timeout asks the engine's return");
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.opts->trunk_is_tuned == 1,
           "on again: the backend refused the tune, so the channel is kept");
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

/* With -t 1 a grant still waiting for its first speech is the state machine's to time out (TETRA_SM_GRANT_TIMEOUT_S):
 * a no-carrier pass before then, whose generic return reads last_vc_sync_time against the one-second hangtime, takes
 * no return of its own and leaves the follow; once the grant timeout passed, the pass ticks the machine, which returns
 * to the control channel itself, once. Without a control channel known the pass does not drop the follow either. */
static void
test_no_carrier_leaves_the_follow_to_the_machine(void) {
    for (int with_cc = 1; with_cc >= 0; with_cc--) {
        session s = session_open();
        expect(follow_grant(&s) == 1 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED, "owner: grant followed");
        if (!with_cc) {
            s.state->trunk_cc_freq = 0;
        }
        at_seconds(2002.0);
        engine_tick(&s);
        noCarrier(s.opts, s.state);
        expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_sm_followed(s.state, NULL),
               "owner: the pass keeps a grant its timeout still keeps");
        expect(g_returns == 0 && s.opts->trunk_is_tuned == 1, "owner: the pass takes no return of its own");
        at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 0.5);
        noCarrier(s.opts, s.state);
        expect(g_returns == with_cc && s.opts->trunk_is_tuned == 0 && !tetra_sm_followed(s.state, NULL),
               "owner: the machine ends the follow at its grant timeout");
        session_close(&s);
    }
}

/* A return of the machine's own in flight is the machine's as well: one the backend failed is put back by the pass's
 * tick of the machine, which keeps the call when its retry is deferred, rather than taken over by a generic return. */
static void
test_no_carrier_leaves_a_return_in_flight_to_the_machine(void) {
    session s = session_open();
    expect(follow_grant(&s) == 1, "owner: grant followed before the return");
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    at_seconds(2000.0 + TETRA_SM_GRANT_TIMEOUT_S + 0.5);
    engine_tick(&s);
    const uint64_t request = g_request;
    expect(g_returns == 1 && dsd_trunk_tuning_pending_request() == request && request != 0U,
           "owner: the grant timeout's return is in flight");
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    noCarrier(s.opts, s.state);
    expect(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_sm_followed(s.state, NULL),
           "owner: the failed return is put back and its deferred retry keeps the call");
    expect(g_returns == 2 && dsd_trunk_tuning_pending_request() == 0U, "owner: one retry, by the machine");
    session_close(&s);
}

int
main(void) {
    dsd_decode_clock_use_test(2000ULL * 1000000000ULL);
    test_tick_settles_tunes();
    test_tick_runs_timers();
    test_carrier_boundary_abandons();
    test_carrier_boundary_abandons_a_return_in_flight();
    test_carrier_boundary_retires_an_abandoned_tune_once_it_fails();
    test_mode_change_leaves_a_request_in_flight_to_the_runtime();
    test_trunking_off_leaves_the_receiver();
    test_no_carrier_forgets_evidence();
    test_no_carrier_leaves_the_follow_to_the_machine();
    test_no_carrier_leaves_a_return_in_flight_to_the_machine();
    dsd_decode_clock_use_system();
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "ENGINE_TETRA_TRUNK: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
