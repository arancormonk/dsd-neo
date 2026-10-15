// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The TETRA trunking state machine (tetra_trunk_sm.h): per-session state, the CRC evidence a grant needs, tunes the
 * backend completes at once or later, the grant timeout and the hangtime after speech, releases by call identifier,
 * and the hand-overs the engine makes (external control-channel return, carrier abandon).
 *
 * The tuning backend is a pair of stand-in hooks that do to the shared tuning state what the engine's do. Time is the
 * decode clock's TEST source.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"

enum {
    CC_HZ = 380012500,
    VC_HZ = 390012500,
    VC2_HZ = 391012500,
};

#define SECOND_NS 1000000000ULL

static uint64_t g_now_ns;
static int g_tunes;
static int g_returns;
static long g_tuned_hz;
static int g_ted_sps;
static uint64_t g_request;
static dsd_trunk_tune_result g_tune_result;
static dsd_trunk_tune_result g_return_result;

/* What the engine's tune hook does to the shared tuning state and the tuned frequency when it accepts a voice-channel
 * tune (dsd_engine_tune_rtl()). */
static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    g_tunes++;
    g_tuned_hz = freq;
    g_ted_sps = ted_sps;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_tune_result)) {
        opts->rtlsdr_center_freq = (uint32_t)freq;
        opts->trunk_is_tuned = 1;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->last_vc_sync_time = dsd_decode_time();
        state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    }
    return g_tune_result;
}

/* And to it when it accepts a return to the control channel (dsd_engine_release_tuned_call_state()). */
static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_returns++;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_return_result)) {
        opts->rtlsdr_center_freq = (uint32_t)state->trunk_cc_freq;
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->last_vc_sync_time = 0;
        state->last_vc_sync_time_m = 0.0;
    }
    return g_return_result;
}

static void
at_seconds(double seconds) {
    g_now_ns = (uint64_t)(seconds * (double)SECOND_NS);
    dsd_decode_clock_test_set_ns(g_now_ns);
}

static void
reset_backend(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    g_tunes = 0;
    g_returns = 0;
    g_tuned_hz = 0;
    g_ted_sps = -1;
    g_request = 0U;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
    at_seconds(1000.0);
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} session;

/* A trunking session on a control channel that a CRC-passed block has proved, with a 2 s hangtime. */
static session
session_open(void) {
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 2.0F;
    s.opts->frame_tetra = 1;
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = CC_HZ;
    s.state->trunk_cc_freq = CC_HZ;
    assert(tetra_st(s.state) != NULL);
    tetra_sm_on_cc_sync(s.opts, s.state);
    tetra_confirm_note_signalling(s.state);
    return s;
}

static void
session_close(session* s) {
    dsd_state_ext_free_all(s->state);
    free(s->state);
    free(s->opts);
}

static tetra_sm_grant
grant_of(long freq_hz, uint8_t timeslots, uint16_t call_id) {
    const tetra_sm_grant g = {freq_hz, 40U, timeslots, call_id};
    return g;
}

static int
followed_call(const dsd_state* state) {
    tetra_sm_grant g;
    return tetra_sm_followed(state, &g) ? (int)g.call_id : -1;
}

/* The control channel comes from CRC-passed SYSINFO, and only while trunking. */
static void
test_control_channel(void) {
    reset_backend();
    session s = session_open();
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    assert(s.state->tetra_pub.trunk_state == DSD_TETRA_TRUNK_ON_CONTROL);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);

    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    tetra_sm_on_cc_sync(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    session_close(&s);

    assert(tetra_sm_get_state(NULL) == TETRA_SM_IDLE);
    assert(!tetra_sm_followed(NULL, NULL));
}

/* Two sessions never share state machine state. */
static void
test_sessions_are_separate(void) {
    reset_backend();
    session a = session_open();
    session b = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(a.opts, a.state, &g) == 1);
    assert(tetra_sm_get_state(a.state) == TETRA_SM_TUNED && followed_call(a.state) == 0x123);
    assert(tetra_sm_get_state(b.state) == TETRA_SM_ON_CC && followed_call(b.state) == -1);
    tetra_sm_on_release(b.opts, b.state, 0x123U);
    assert(tetra_sm_get_state(a.state) == TETRA_SM_TUNED && g_returns == 0);
    session_close(&a);
    session_close(&b);
}

/* No grant is followed before a CRC-passed BSCH or MAC block on the carrier; the tune has no timing override. */
static void
test_grant_needs_carrier_evidence(void) {
    reset_backend();
    session s = session_open();
    tetra_confirm_reset(s.state);
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0);
    assert(g_tunes == 0 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);

    tetra_confirm_note_signalling(s.state);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    assert(g_tunes == 1 && g_tuned_hz == VC_HZ && g_ted_sps == 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.opts->trunk_is_tuned == 1);
    assert(s.state->tetra_pub.trunk_state == DSD_TETRA_TRUNK_ON_TRAFFIC);
    /* The receiver is on another carrier now: the control channel's evidence and TDMA clock are gone. */
    assert(!tetra_confirm_carrier_proven(s.state) && !tetra_confirm_followed_call_proven(s.state));
    assert(tetra_st(s.state)->tdma_valid == 0);
    session_close(&s);
}

/* Grants that cannot be followed change nothing. */
static void
test_ignored_grants(void) {
    reset_backend();
    session s = session_open();
    tetra_sm_grant g = grant_of(0, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0);
    g = grant_of(VC_HZ, 0x4U, 0x123U);
    s.opts->trunk_enable = 0;
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0);
    s.opts->trunk_enable = 1;
    s.state->trunk_cc_freq = 0;
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0);
    s.state->trunk_cc_freq = CC_HZ;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(tetra_sm_on_grant(s.opts, s.state, NULL) == 0);
    session_close(&s);
}

/* No speech after the tune: the grant timeout returns to the control channel. */
static void
test_grant_timeout(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S - 0.1);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1 && s.opts->trunk_is_tuned == 0);
    assert(followed_call(s.state) == -1);
    session_close(&s);
}

/* Speech on a followed timeslot keeps the channel; the hangtime counts from the end of the last speech. */
static void
test_hangtime_after_speech(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U); /* timeslot 2 */
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    for (int i = 1; i <= 100; i++) {
        at_seconds(1000.0 + 0.0567 * i);
        tetra_sm_note_voice(s.state, 2U);
        tetra_sm_tick(s.opts, s.state);
    }
    const double last = 1000.0 + 0.0567 * 100;
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    assert(s.state->last_vc_sync_time_m > 1000.0);

    /* Speech on timeslot 3, which the call was not given, is not the call's. */
    at_seconds(last + TETRA_SM_SPEECH_GAP_S + 1.0);
    tetra_sm_note_voice(s.state, 3U);
    at_seconds(last + TETRA_SM_SPEECH_GAP_S + 1.99);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    at_seconds(last + TETRA_SM_SPEECH_GAP_S + 2.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1);
    session_close(&s);
}

/* Speech the decoder cannot place on a timeslot (no TDMA clock) may be any timeslot's, so it is not the followed
 * call's: it does not keep the channel, and the grant times out as though none had come. It is still reception. */
static void
test_unplaced_speech_does_not_count(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    at_seconds(1003.9);
    tetra_sm_note_voice(s.state, 0U);
    assert(s.state->last_vc_sync_time_m >= 1003.9 && tetra_st(s.state)->sm.t_voice_m <= 0.0);
    tetra_sm_note_voice(s.state, 5U);
    assert(tetra_st(s.state)->sm.t_voice_m <= 0.0);
    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1);
    session_close(&s);
}

/* Only the followed call releases. */
static void
test_release_by_call_id(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    tetra_sm_on_release(s.opts, s.state, 0x124U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1);
    /* Leaving the call leaves no followed call behind for the speech gates or the traffic row. */
    tetra_chan_alloc shown;
    assert(tetra_calls_followed(s.state) == NULL && !tetra_calls_traffic(s.state, &shown));
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    assert(g_returns == 1);
    session_close(&s);
}

/* A return the backend refuses keeps the call; the next tick tries again. */
static void
test_refused_return_keeps_call(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    s.state->p25_cc_freq = 0;
    g_return_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x123 && g_returns == 1);
    assert(s.opts->trunk_is_tuned == 1);
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 2);
    session_close(&s);
}

/* A grant on the carrier the receiver is on needs no retune: a traffic timeslot of the main carrier. */
static void
test_grant_on_this_carrier(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(CC_HZ, 0x2U, 0x77U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    assert(g_tunes == 0 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.opts->trunk_is_tuned == 1);
    assert(s.state->trunk_vc_freq[0] == CC_HZ);
    /* The grant was the call's CRC-passed signalling on this carrier, whose evidence stays. */
    assert(tetra_confirm_followed_call_proven(s.state) && tetra_confirm_carrier_proven(s.state));

    /* The same call again is nothing new; one without a call identifier names no other call. */
    const tetra_sm_grant same = grant_of(CC_HZ, 0x2U, 0U);
    assert(tetra_sm_on_grant(s.opts, s.state, &same) == 0 && followed_call(s.state) == 0x77);

    tetra_sm_on_release(s.opts, s.state, 0x77U);
    assert(g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);
    assert(s.state->trunk_vc_freq[0] == 0);
    session_close(&s);
}

/* SYSINFO names the main carrier on every carrier of the cell, so it does not say where the receiver is: started on a
 * secondary carrier, a grant for a traffic timeslot of the main carrier is a retune, and a grant on the carrier the
 * receiver is tuned to is not. With no tuned frequency known (no radio input) every grant is a retune. */
static void
test_grant_on_the_received_carrier(void) {
    reset_backend();
    session s = session_open();
    s.opts->rtlsdr_center_freq = VC2_HZ;
    tetra_sm_grant g = grant_of(CC_HZ, 0x2U, 0x77U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    assert(g_tunes == 1 && g_tuned_hz == CC_HZ && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    assert(!tetra_confirm_followed_call_proven(s.state) && !tetra_confirm_carrier_proven(s.state));
    session_close(&s);

    reset_backend();
    s = session_open();
    s.opts->rtlsdr_center_freq = VC2_HZ;
    g = grant_of(VC2_HZ, 0x2U, 0x78U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1 && g_tunes == 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_confirm_followed_call_proven(s.state));
    session_close(&s);

    reset_backend();
    s = session_open();
    s.opts->audio_in_type = AUDIO_IN_PULSE;
    g = grant_of(CC_HZ, 0x2U, 0x79U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1 && g_tunes == 1 && g_tuned_hz == CC_HZ);
    session_close(&s);
}

/* A call identifier arriving after a header-only allocation is adopted; another call on the followed carrier is taken
 * without a retune. */
static void
test_same_carrier_grants(void) {
    reset_backend();
    session s = session_open();
    tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1 && followed_call(s.state) == 0);
    g = grant_of(VC_HZ, 0x4U, 0x55U);
    tetra_confirm_note_signalling(s.state);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 0 && followed_call(s.state) == 0x55 && g_tunes == 1);
    g = grant_of(VC_HZ, 0x1U, 0x56U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1 && followed_call(s.state) == 0x56 && g_tunes == 1);
    assert(tetra_confirm_followed_call_proven(s.state));
    /* Preemption onto another carrier retunes. */
    g = grant_of(VC2_HZ, 0x8U, 0x57U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1 && g_tunes == 2 && g_tuned_hz == VC2_HZ);
    assert(followed_call(s.state) == 0x57);
    session_close(&s);
}

/* A tune the backend reports in flight: TUNED is staged, the tick settles the completion. */
static void
test_pending_grant_completes(void) {
    reset_backend();
    session s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const uint64_t request = g_request;
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && dsd_trunk_tuning_pending_request() == request);
    /* A second grant waits for the first tune. */
    const tetra_sm_grant other = grant_of(VC2_HZ, 0x8U, 0x124U);
    assert(tetra_sm_on_grant(s.opts, s.state, &other) == 0 && g_tunes == 1);
    tetra_sm_tick(s.opts, s.state);
    assert(dsd_trunk_tuning_pending_request() == request);

    at_seconds(1001.0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_tick(s.opts, s.state);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.opts->trunk_is_tuned == 1);
    /* The grant timeout runs from the completion. */
    at_seconds(1001.0 + TETRA_SM_GRANT_TIMEOUT_S - 0.1);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    at_seconds(1001.0 + TETRA_SM_GRANT_TIMEOUT_S);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    session_close(&s);
}

/* A rename of the followed call (D-INFO, D-CALL RESTORE) moves the identifier the state machine follows, whatever a
 * grant would need (a proved carrier, a control channel), and keeps its timers; it moves the identifier a tune in
 * flight would put back too, so a failure of that tune brings the call back under its new identifier. Another
 * identifier, and a retired followed call, are not renamed. */
static void
test_rename_moves_the_followed_identifier(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant a = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &a) == 1 && !tetra_confirm_carrier_proven(s.state));
    at_seconds(1001.0);
    tetra_sm_note_voice(s.state, 2U);
    s.state->trunk_cc_freq = 0; /* the control channel unknown for now: no grant could be followed */
    tetra_sm_on_call_renamed(s.state, 0x124U, 0x200U);
    assert(followed_call(s.state) == 0x123);
    at_seconds(1001.5);
    tetra_sm_on_call_renamed(s.state, 0x123U, 0x200U);
    assert(followed_call(s.state) == 0x200 && g_tunes == 1);
    assert(tetra_st(s.state)->sm.t_voice_m > 1000.9 && tetra_st(s.state)->sm.t_voice_m < 1001.1);
    s.state->trunk_cc_freq = CC_HZ;

    /* The rollback of a tune in flight to another call. */
    tetra_confirm_note_signalling(s.state);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant b = grant_of(VC2_HZ, 0x8U, 0x300U);
    assert(tetra_sm_on_grant(s.opts, s.state, &b) == 1 && followed_call(s.state) == 0x300);
    tetra_sm_on_call_renamed(s.state, 0x200U, 0x201U);
    assert(followed_call(s.state) == 0x300);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x201);
    uint16_t id = 0;
    assert(tetra_sm_followed_call_id(s.state, &id) && id == 0x201U);

    /* A retired followed call is the channel, no call: a call renamed from its identifier is not it. */
    tetra_sm_retire_call(s.state, 0x201U);
    tetra_sm_on_call_renamed(s.state, 0x201U, 0x202U);
    assert(followed_call(s.state) == 0x201 && !tetra_sm_followed_call_id(s.state, NULL));
    session_close(&s);
}

/* A tune that fails after being reported in flight puts everything back and opens the frame gate. */
static void
test_pending_grant_fails(void) {
    reset_backend();
    session s = session_open();
    s.state->last_vc_sync_time = 77;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const uint64_t request = g_request;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == request);
    tetra_sm_tick(s.opts, s.state);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    assert(s.opts->trunk_is_tuned == 0 && s.state->trunk_vc_freq[0] == 0 && s.state->last_vc_sync_time == 77);
    /* Nothing was forgotten: the receiver never left the control channel. */
    assert(tetra_confirm_carrier_proven(s.state));
    session_close(&s);
}

/* A tune away from a proven followed call that fails leaves the receiver on that call's carrier, so the call's proof
 * comes back with it: its speech keeps playing and keeps its hangtime running. A carrier reset while the tune was in
 * flight dropped that proof, and the rollback does not bring it back. */
static void
test_failed_tune_restores_followed_proof(void) {
    for (int reset_between = 0; reset_between < 2; reset_between++) {
        reset_backend();
        session s = session_open();
        const tetra_sm_grant a = grant_of(VC_HZ, 0x4U, 0x123U);
        assert(tetra_sm_on_grant(s.opts, s.state, &a) == 1);
        tetra_confirm_note_signalling(s.state);
        tetra_confirm_note_followed_call(s.state);
        assert(tetra_confirm_followed_call_proven(s.state));

        g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        const tetra_sm_grant b = grant_of(VC2_HZ, 0x8U, 0x124U);
        assert(tetra_sm_on_grant(s.opts, s.state, &b) == 1);
        assert(followed_call(s.state) == 0x124 && !tetra_confirm_followed_call_proven(s.state));
        if (reset_between) {
            tetra_confirm_reset(s.state);
            tetra_confirm_note_signalling(s.state);
        }
        dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
        tetra_sm_tick(s.opts, s.state);
        assert(followed_call(s.state) == 0x123 && dsd_trunk_tuning_pending_request() == 0U);
        assert(tetra_confirm_followed_call_proven(s.state) == !reset_between);
        session_close(&s);
    }

    /* A return in flight keeps the proof until it completes; one that fails after a carrier reset finds none. */
    reset_backend();
    session s = session_open();
    const tetra_sm_grant a = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &a) == 1);
    tetra_confirm_note_followed_call(s.state);
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    tetra_confirm_reset(s.state);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(followed_call(s.state) == 0x123 && !tetra_confirm_followed_call_proven(s.state));
    session_close(&s);
}

/* A tune the backend has not answered stays the backend's however long it takes: the state machine invents no result,
 * the frame gate stays closed, and the backend's completion is the tuner boundary, which the state machine then commits
 * (trunk_tuning_hooks.h). The same holds for a return in flight. */
static void
test_slow_backend_keeps_the_gate(void) {
    reset_backend();
    session s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const uint64_t request = g_request;
    const uint64_t generation = dsd_trunk_tuning_generation();
    at_seconds(1060.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x123);
    assert(dsd_trunk_tuning_pending_request() == request);
    assert(dsd_trunk_tuning_request_status(request, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(!dsd_trunk_tuning_frame_is_dispatchable(generation, 1));
    /* The receiver has not left the control channel yet: its evidence stands until the tune completes. */
    assert(tetra_confirm_carrier_proven(s.state));

    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == generation + 1U && dsd_trunk_tuning_pending_request() == 0U);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x123);
    assert(s.opts->trunk_is_tuned == 1 && !tetra_confirm_carrier_proven(s.state));
    assert(dsd_trunk_tuning_frame_is_dispatchable(generation + 1U, 1));

    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    const uint64_t back = g_request;
    assert(back != request && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    at_seconds(1120.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    assert(dsd_trunk_tuning_pending_request() == back && !dsd_trunk_tuning_frame_is_dispatchable(generation + 1U, 1));
    dsd_trunk_tuning_request_publish(back, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);
    assert(dsd_trunk_tuning_generation() == generation + 2U && dsd_trunk_tuning_pending_request() == 0U);
    session_close(&s);
}

/* A return reported in flight stages ON_CC; one that then fails puts the call back. */
static void
test_pending_return(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    uint64_t request = g_request;
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x123);
    assert(s.opts->trunk_is_tuned == 1 && s.state->trunk_vc_freq[0] == VC_HZ);
    assert(dsd_trunk_tuning_pending_request() == 0U);

    tetra_sm_on_release(s.opts, s.state, 0x123U);
    request = g_request;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);
    assert(!tetra_sm_followed(s.state, NULL) && dsd_trunk_tuning_pending_request() == 0U);
    session_close(&s);
}

/* The backend still owns @p request, which the state machine let go of: the frame gate stays closed until it answers,
 * and its completion is the tuner boundary it is (trunk_tuning_hooks.h). */
static void
assert_in_flight_left_to_backend(uint64_t request) {
    assert(request != 0U && dsd_trunk_tuning_pending_request() == request);
    assert(dsd_trunk_tuning_request_status(request, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    const uint64_t generation = dsd_trunk_tuning_generation();
    assert(!dsd_trunk_tuning_frame_is_dispatchable(generation, 0));
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_pending_request() == 0U && dsd_trunk_tuning_generation() == generation + 1U);
    assert(dsd_trunk_tuning_frame_is_dispatchable(generation + 1U, 0));
}

/* The engine returned the receiver itself: reconcile without tuning. A tune still in flight is the backend's. */
static void
test_external_return(void) {
    reset_backend();
    session s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const uint64_t request = g_request;
    tetra_st(s.state)->tdma_valid = 1;
    s.opts->trunk_is_tuned = 0;
    tetra_sm_on_external_cc_return(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    assert(g_returns == 0 && g_tunes == 1);
    assert(tetra_st(s.state)->tdma_valid == 0 && !tetra_confirm_carrier_proven(s.state));
    assert_in_flight_left_to_backend(request);
    /* The state machine no longer counts it as its own. */
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);

    s.state->trunk_cc_freq = 0;
    tetra_sm_on_external_cc_return(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    session_close(&s);
}

/* A carrier boundary drops the call without tuning and keeps the TETRA state allocated. */
static void
test_abandon_carrier(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const tetra_state_t* const before = tetra_st(s.state);
    tetra_sm_abandon_carrier(s.opts, s.state);
    assert(tetra_st_existing(s.state) == before);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    assert(s.opts->trunk_is_tuned == 0 && s.state->trunk_vc_freq[0] == 0 && g_returns == 0 && g_tunes == 1);
    tetra_sm_abandon_carrier(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 0);
    session_close(&s);

    dsd_state* bare = (dsd_state*)calloc(1U, sizeof(*bare));
    assert(bare != NULL);
    tetra_sm_abandon_carrier(NULL, bare);
    tetra_sm_on_external_cc_return(NULL, bare);
    tetra_sm_init(NULL, bare);
    tetra_forget_carrier(bare);
    assert(tetra_st_existing(bare) == NULL);
    free(bare);
}

/* Another owner released the shared tuning state: the call is dropped where the receiver is. Turning trunking off
 * while tuned leaves the receiver on the followed channel, as DMR's and P25's machines do: the machine keeps the
 * channel and lets go of the call, and asks for no return, at the tick or at a release, until trunking is on again,
 * whose next tick runs the grant timeout. */
static void
test_released_elsewhere_and_trunking_off(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    s.opts->trunk_is_tuned = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 0);

    tetra_confirm_note_signalling(s.state);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0 && s.opts->trunk_is_tuned == 1);
    assert(followed_call(s.state) == 0x123 && !tetra_sm_followed_call_id(s.state, NULL));
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0 && s.opts->trunk_is_tuned == 1);
    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S + 1.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    s.opts->trunk_enable = 1;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1 && s.opts->trunk_is_tuned == 0);
    session_close(&s);
}

/* Starting over retires a tune in flight. */
static void
test_init_retires_pending(void) {
    reset_backend();
    session s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_init(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE && dsd_trunk_tuning_pending_request() == 0U);
    session_close(&s);
}

/* Dropping the TETRA state (a decode-mode change, the end of the session) retires a tune that failed: nothing is left
 * to settle it. */
static void
test_dropped_state_retires_pending(void) {
    reset_backend();
    session s = session_open();
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == g_request);
    tetra_state_reset(s.state);
    assert(dsd_trunk_tuning_pending_request() == 0U && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    session_close(&s);
}

/* Letting go of a tune the backend has not answered yet -- dropping the TETRA state, starting over, a carrier abandon
 * -- leaves it to the backend: completing it as failed would open the frame gate while the tuner still moves, and the
 * completion that follows would then publish no tuner boundary. */
static void
test_in_flight_tune_survives_letting_go(void) {
    for (int how = 0; how < 3; how++) {
        reset_backend();
        session s = session_open();
        g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
        assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
        const uint64_t request = g_request;
        if (how == 0) {
            tetra_state_reset(s.state);
            assert(tetra_st_existing(s.state) == NULL);
        } else if (how == 1) {
            tetra_sm_init(s.opts, s.state);
            assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
        } else {
            tetra_sm_abandon_carrier(s.opts, s.state);
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);
        }
        assert_in_flight_left_to_backend(request);
        session_close(&s);
    }
}

/* CRC evidence: a burst that decoded a block proves itself; a reset during a burst leaves the rest of it to the
 * carrier that was left. */
static void
test_confirm_evidence(void) {
    reset_backend();
    session s = session_open();
    tetra_confirm_reset(s.state);
    tetra_confirm_burst_begin(s.state);
    assert(tetra_confirm_burst_end(s.state) == 0);

    tetra_confirm_burst_begin(s.state);
    tetra_confirm_note_block(s.state);
    assert(!tetra_confirm_carrier_proven(s.state));
    assert(tetra_confirm_burst_end(s.state) == 1);

    tetra_confirm_burst_begin(s.state);
    tetra_confirm_note_signalling(s.state);
    assert(tetra_confirm_carrier_proven(s.state) && tetra_confirm_burst_current(s.state));
    tetra_confirm_reset(s.state);
    assert(!tetra_confirm_burst_current(s.state));
    tetra_confirm_note_signalling(s.state);
    tetra_confirm_note_followed_call(s.state);
    assert(!tetra_confirm_carrier_proven(s.state) && !tetra_confirm_followed_call_proven(s.state));
    assert(tetra_confirm_burst_end(s.state) == 1);
    /* What a burst decoded after the carrier was left still proves the burst. */
    tetra_confirm_burst_begin(s.state);
    tetra_confirm_reset(s.state);
    tetra_confirm_note_signalling(s.state);
    assert(!tetra_confirm_carrier_proven(s.state));
    assert(tetra_confirm_burst_end(s.state) == 1);
    assert(tetra_confirm_burst_current(s.state));
    tetra_confirm_note_followed_call(s.state);
    assert(tetra_confirm_followed_call_proven(s.state));
    tetra_confirm_forget_followed_call(s.state);
    assert(!tetra_confirm_followed_call_proven(s.state));
    session_close(&s);
}

int
main(void) {
    test_control_channel();
    test_sessions_are_separate();
    test_grant_needs_carrier_evidence();
    test_ignored_grants();
    test_grant_timeout();
    test_hangtime_after_speech();
    test_unplaced_speech_does_not_count();
    test_rename_moves_the_followed_identifier();
    test_release_by_call_id();
    test_refused_return_keeps_call();
    test_grant_on_this_carrier();
    test_grant_on_the_received_carrier();
    test_same_carrier_grants();
    test_pending_grant_completes();
    test_pending_grant_fails();
    test_failed_tune_restores_followed_proof();
    test_slow_backend_keeps_the_gate();
    test_pending_return();
    test_external_return();
    test_abandon_carrier();
    test_released_elsewhere_and_trunking_off();
    test_init_retires_pending();
    test_dropped_state_retires_pending();
    test_in_flight_tune_survives_letting_go();
    test_confirm_evidence();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_system();
    DSD_FPRINTF(stdout, "TETRA_TRUNK_SM: OK\n");
    return 0;
}
