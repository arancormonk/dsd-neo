// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The TETRA trunking state machine (tetra_trunk_sm.h): per-session state, the CRC evidence a grant needs, tunes the
 * backend completes at once or later, the grant timeout and the hangtime after speech, releases by call identifier,
 * and the hand-overs the engine makes (external control-channel return, carrier abandon).
 *
 * The tuning backend is a pair of stand-in hooks that do to the shared tuning state what the engine's do. Time is the
 * decode clock's TEST source.
 *
 * A session started on a secondary carrier moves to the main carrier its SYSINFO names: SYSINFO blocks go through the
 * real MAC (tetra_mac_parse_schd()) the way tetra.c decodes a burst, and the burst's end runs the move it staged.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_log.h"
#include "tetra_test_pdu.h"

enum {
    CC_HZ = 380012500,
    VC_HZ = 390012500,
    VC2_HZ = 391012500,
    P25_CC_HZ = 851012500, /* a control channel another protocol left as the shared return's alias (p25_cc_freq) */
    OTHER_HZ = 395012500,
};

#define SECOND_NS 1000000000ULL

static uint64_t g_now_ns;
static int g_tunes;
static int g_returns;
static long g_tuned_hz;
static long g_returned_hz;
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

/* And to it when it accepts a return to the control channel (dsd_engine_release_tuned_call_state()), which goes to the
 * P25 alias when one is set (dsd_engine_resolve_cc_freq()) and stamps control-channel sync as it tunes there
 * (dsd_engine_trunk_tune_to_cc_request()). */
static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_returns++;
    g_request = request_id;
    g_returned_hz = state->p25_cc_freq != 0 ? state->p25_cc_freq : state->trunk_cc_freq;
    if (dsd_trunk_tune_result_is_ok(g_return_result)) {
        state->last_cc_sync_time = dsd_decode_time();
        state->last_cc_sync_time_m = dsd_decode_now_mono_s();
        opts->rtlsdr_center_freq = (uint32_t)g_returned_hz;
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
    g_returned_hz = 0;
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

/* A tune that fails after being reported in flight puts everything back and opens the frame gate. That includes the
 * tuned centre the backend cached as the target when it reported the tune in flight (dsd_engine_tune_rtl()): the
 * receiver never left the control channel, so tetra_sm_receiver_freq() says so, and the same grant again is a retune
 * rather than a follow on a carrier the receiver never reached (regression: main left the target cached). */
static void
test_pending_grant_fails(void) {
    reset_backend();
    session s = session_open();
    s.state->last_vc_sync_time = 77;
    s.state->p25_cc_freq = P25_CC_HZ;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const uint64_t request = g_request;
    assert(s.opts->rtlsdr_center_freq == VC_HZ && tetra_sm_receiver_freq(s.opts, s.state) == 0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == request);
    tetra_sm_tick(s.opts, s.state);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    assert(s.opts->trunk_is_tuned == 0 && s.state->trunk_vc_freq[0] == 0 && s.state->last_vc_sync_time == 77);
    assert(s.opts->rtlsdr_center_freq == CC_HZ && dsd_opts_tuned_freq_hz(s.opts) == CC_HZ);
    assert(tetra_sm_receiver_freq(s.opts, s.state) == CC_HZ && s.state->p25_cc_freq == P25_CC_HZ);
    /* Nothing was forgotten: the receiver never left the control channel. */
    assert(tetra_confirm_carrier_proven(s.state));

    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    assert(g_tunes == 2 && g_tuned_hz == VC_HZ && s.opts->rtlsdr_center_freq == VC_HZ);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x123);
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

/* A return reported in flight stages ON_CC; one that then fails puts the call back, with the tuned centre the backend
 * cached for the return and the P25 alias the return cleared (regression: main left the control channel cached and the
 * alias cleared). The next return clears the alias again and goes to TETRA's own control channel. */
static void
test_pending_return(void) {
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    s.state->p25_cc_freq = P25_CC_HZ;
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    uint64_t request = g_request;
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
    assert(g_returned_hz == CC_HZ && s.opts->rtlsdr_center_freq == CC_HZ && s.state->p25_cc_freq == 0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x123);
    assert(s.opts->trunk_is_tuned == 1 && s.state->trunk_vc_freq[0] == VC_HZ);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(s.opts->rtlsdr_center_freq == VC_HZ && dsd_opts_tuned_freq_hz(s.opts) == VC_HZ);
    assert(tetra_sm_receiver_freq(s.opts, s.state) == VC_HZ && s.state->p25_cc_freq == P25_CC_HZ);

    tetra_sm_on_release(s.opts, s.state, 0x123U);
    request = g_request;
    assert(g_returns == 2 && g_returned_hz == CC_HZ && s.state->p25_cc_freq == 0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.opts->trunk_is_tuned == 0);
    assert(!tetra_sm_followed(s.state, NULL) && dsd_trunk_tuning_pending_request() == 0U);
    assert(s.opts->rtlsdr_center_freq == CC_HZ && tetra_sm_receiver_freq(s.opts, s.state) == CC_HZ);
    session_close(&s);
}

/* A request the state machine started fails after another owner moved the receiver: what that move left stands. A
 * carrier boundary or an engine return let go of the request (tetra_sm_disown_pending()); a correlated tune begun
 * since decides the receiver whatever its target; a tuned centre or P25 alias someone wrote since is theirs. */
static void
test_late_failure_after_another_tune_puts_back_no_receiver(void) {
    for (int how = 0; how < 4; how++) {
        reset_backend();
        session s = session_open();
        const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
        const int returning = how == 1;
        if (returning) {
            assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
            s.state->p25_cc_freq = P25_CC_HZ;
            g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
            tetra_sm_on_release(s.opts, s.state, 0x123U);
        } else {
            g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
            assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
        }
        const uint64_t request = g_request;
        uint64_t other = 0U;
        if (how <= 1) {
            /* The operator's tune elsewhere (a carrier boundary). */
            tetra_sm_abandon_carrier(s.opts, s.state);
            s.opts->rtlsdr_center_freq = OTHER_HZ;
            s.state->p25_cc_freq = OTHER_HZ;
        } else if (how == 2) {
            /* A correlated tune begun since, to the very frequency this request cached. */
            other = dsd_trunk_tuning_request_begin();
            s.opts->rtlsdr_center_freq = VC_HZ;
        } else {
            /* A tuned centre written since without a correlated request. */
            s.opts->rtlsdr_center_freq = OTHER_HZ;
        }
        const uint32_t centre = s.opts->rtlsdr_center_freq;
        const long alias = s.state->p25_cc_freq;
        dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
        tetra_sm_tick(s.opts, s.state);
        assert(s.opts->rtlsdr_center_freq == centre && s.state->p25_cc_freq == alias);
        if (other != 0U) {
            dsd_trunk_tuning_request_complete(other, DSD_TRUNK_TUNE_RESULT_OK);
        }
        assert(dsd_trunk_tuning_pending_request() == 0U);
        session_close(&s);
    }
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

/* The carrier a session starts on, a secondary carrier of the cell whose SYSINFO names main carrier 50 of band 4 (offset
 * -6.25 kHz), and two other main carriers a SYSINFO may name. */
enum { SECONDARY_HZ = 401268750 };

static const uint32_t k_main = 50U;
static const uint32_t k_other_main = 60U;

static long
main_hz(uint32_t carrier) {
    return tetra_carrier_to_dl_hz(carrier, 4U, 2U);
}

static int g_tuner_busy;

/* Decode-clock seconds compared to the nanosecond the test clock is set to. */
static int
same_s(double a, double b) {
    return a > b - 1e-6 && a < b + 1e-6;
}

static int
tuner_free_hook(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return !g_tuner_busy;
}

/* A trunking session on a live RTL stream tuned to a secondary carrier, whose control channel no SYSINFO has named yet;
 * a CRC-passed block there proved the carrier unless @p proven is 0. The engine's tuner-free answer is the stub's. */
static session
secondary_open(int proven) {
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 2.0F;
    s.opts->frame_tetra = 1;
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = SECONDARY_HZ;
    s.opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    assert(tetra_st(s.state) != NULL);
    if (proven) {
        tetra_confirm_note_signalling(s.state);
    }
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    hooks.tuner_free_for_protocol = tuner_free_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    g_tuner_busy = 0;
    return s;
}

/* One burst on the carrier the receiver is on whose signalling block is @p block, decoded as tetra.c decodes it: the MAC
 * acts on the block, the block then counts as evidence about the carrier, and the burst's end runs a move the block
 * staged. What the decoder writes meanwhile is kept out of the test's output. */
static void
block_burst(session* s, const ttb_block* block) {
    char log[2048];
    tetra_confirm_burst_begin(s->state);
    tetra_test_mac_logged(block->bits, block->n, 0, s->opts, s->state, log, sizeof log);
    tetra_confirm_note_signalling(s->state);
    (void)tetra_confirm_burst_end(s->state);
    tetra_sm_after_burst(s->opts, s->state);
}

/* A burst carrying SYSINFO that names main carrier @p carrier (band 4, or band 0: no main carrier). */
static void
sysinfo_burst(session* s, uint32_t carrier, uint32_t band) {
    ttb_block b = {0};
    ttb_sysinfo(&b, carrier, band, 2U, 987U);
    block_burst(s, &b);
}

static int
cc_candidates(const dsd_state* state) {
    const dsd_trunk_cc_candidates* c = dsd_trunk_cc_candidates_peek(state);
    return c ? c->count : 0;
}

/* The session's main-carrier acquisition; every session these tests open has TETRA state. */
static const tetra_sm_acquire*
acq_of(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    assert(ts != NULL);
    return &ts->sm.acq;
}

static const tetra_sm_move*
move_of(const dsd_state* state, long hz) {
    const tetra_sm_acquire* const acq = acq_of(state);
    for (int i = 0; i < TETRA_SM_MOVES; i++) {
        if (acq->moves[i].hz == hz) {
            return &acq->moves[i];
        }
    }
    return NULL;
}

/* Started on a secondary carrier, the session moves to the main carrier the SYSINFO there names, once a CRC-passed block
 * proved the carrier before that SYSINFO: one return to the control channel, with a stale P25 alias cleared for it, no
 * control-channel sync stamped (the engine's stamp taken back) and no candidate added. A valid BSCH on the main carrier
 * keeps it ACQUIRING; the main carrier's own SYSINFO makes it ON_CC and stamps the sync (regression: main stamped the
 * sync and rested ON_CC on the secondary carrier, never moving, so the MCCH's call set-ups were never heard). */
static void
test_secondary_carrier_moves_to_the_main_carrier(void) {
    reset_backend();
    session s = secondary_open(1);
    s.state->p25_cc_freq = P25_CC_HZ;
    sysinfo_burst(&s, k_main, 4U);
    assert(s.state->trunk_cc_freq == main_hz(k_main));
    assert(g_returns == 1 && g_returned_hz == main_hz(k_main) && g_tunes == 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && tetra_sm_owns_return(s.state));
    assert(s.state->tetra_pub.trunk_state == DSD_TETRA_TRUNK_IDLE);
    assert(s.opts->rtlsdr_center_freq == (uint32_t)main_hz(k_main) && s.state->p25_cc_freq == 0);
    assert(s.state->last_cc_sync_time == 0 && s.state->last_cc_sync_time_m <= 0.0 && cc_candidates(s.state) == 0);
    /* The move forgot the carrier left. */
    assert(!tetra_confirm_carrier_proven(s.state));

    at_seconds(1001.0);
    tetra_confirm_note_signalling(s.state); /* a valid BSCH on the main carrier, without SYSINFO */
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && s.state->last_cc_sync_time == 0);
    sysinfo_burst(&s, k_main, 4U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_owns_return(s.state));
    assert(s.state->tetra_pub.trunk_state == DSD_TETRA_TRUNK_ON_CONTROL);
    assert(same_s(s.state->last_cc_sync_time_m, 1001.0) && cc_candidates(s.state) == 1 && g_returns == 1);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    session_close(&s);
}

/* A SYSINFO never vouches for itself: one on a carrier nothing proved before it stages nothing, and the next one, with
 * the first as evidence, moves. A carrier reset between the SYSINFO and its burst's end (a tune completed in the burst)
 * drops what it staged. */
static void
test_move_needs_prior_evidence_on_the_carrier(void) {
    reset_backend();
    session s = secondary_open(0);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE && s.state->last_cc_sync_time == 0);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
    session_close(&s);

    reset_backend();
    s = secondary_open(1);
    ttb_block b = {0};
    ttb_sysinfo(&b, k_main, 4U, 2U, 987U);
    char log[2048];
    tetra_confirm_burst_begin(s.state);
    tetra_test_mac_logged(b.bits, b.n, 0, s.opts, s.state, log, sizeof log);
    tetra_confirm_reset(s.state);
    (void)tetra_confirm_burst_end(s.state);
    tetra_sm_after_burst(s.opts, s.state);
    assert(g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    session_close(&s);
}

/* What names no main carrier never moves: a SYSINFO without one (band 0, EN 300 392-2 table 21.65 note 3) and a
 * neighbour cell broadcast (D-NWRK-BROADCAST). */
static void
test_no_main_carrier_no_move(void) {
    reset_backend();
    session s = secondary_open(1);
    sysinfo_burst(&s, 0U, 0U);
    assert(g_returns == 0 && s.state->trunk_cc_freq == 0 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);

    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0xFFFFFFu);
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 2, 4); /* BL-UDATA */
    ttb_put(&b, TETRA_MLE_PD_MLE, 3);
    ttb_put(&b, TETRA_MLE_D_NWRK_BROADCAST, 3);
    ttb_put(&b, 0xA55Au, 16); /* cell re-select parameters */
    ttb_put(&b, 2, 2);        /* cell load CA */
    ttb_put(&b, 0, 1);        /* O-bit */
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    block_burst(&s, &b);
    assert(g_returns == 0 && g_tunes == 0 && s.state->trunk_cc_freq == 0);
    session_close(&s);
}

/* Case @p how of test_no_move_unless_the_tuner_is_the_machines(), set up on a fresh session. */
static void
no_move_setup(session* s, int how) {
    const tetra_sm_grant g = grant_of(how == 5 ? SECONDARY_HZ : VC_HZ, 0x4U, 0x123U);
    switch (how) {
        case 0: s->opts->trunk_enable = 0; break;
        case 1:
            s->opts->tetra_uplink = 1;
            tetra_st(s->state)->uplink = 1;
            break;
        case 2: DSD_SNPRINTF(s->opts->audio_in_dev, sizeof s->opts->audio_in_dev, "%s", "iqreplay:capture.cu8"); break;
        case 3: s->opts->audio_in_type = AUDIO_IN_SYMBOL_BIN; break;
        case 4: s->opts->rtlsdr_center_freq = (uint32_t)main_hz(k_main); break;
        case 5:
        case 6:
            s->state->trunk_cc_freq = main_hz(k_main);
            g_tune_result = how == 6 ? DSD_TRUNK_TUNE_RESULT_PENDING : DSD_TRUNK_TUNE_RESULT_OK;
            assert(tetra_sm_on_grant(s->opts, s->state, &g) == 1);
            break;
        case 7: g_tuner_busy = 1; break;
        default: break;
    }
}

/* The SYSINFO burst of case @p how: in case 8 a later block of the same burst starts a grant's tune. */
static void
no_move_burst(session* s, int how) {
    if (how != 8) {
        sysinfo_burst(s, k_main, 4U);
        return;
    }
    ttb_block b = {0};
    ttb_sysinfo(&b, k_main, 4U, 2U, 987U);
    char log[2048];
    tetra_confirm_burst_begin(s->state);
    tetra_test_mac_logged(b.bits, b.n, 0, s->opts, s->state, log, sizeof log);
    tetra_confirm_note_signalling(s->state);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s->opts, s->state, &g) == 1);
    (void)tetra_confirm_burst_end(s->state);
    tetra_sm_after_burst(s->opts, s->state);
}

/* Nothing moves while the move is not the state machine's to make: trunking off, an uplink session, an I/Q replay, a
 * symbol capture, the receiver already on the main carrier, a followed call, a request of its own in flight (one a
 * later block of the same burst started included), or the engine saying the tuner is busy (a manual tune pending, a
 * scan owning it). A symbol capture names no frequency and cannot be moved, so its SYSINFO is the control channel's,
 * as on the main carrier itself. */
static void
test_no_move_unless_the_tuner_is_the_machines(void) {
    for (int how = 0; how < 9; how++) {
        reset_backend();
        session s = secondary_open(1);
        no_move_setup(&s, how);
        no_move_burst(&s, how);
        assert(g_returns == 0);
        const tetra_sm_state_e st = tetra_sm_get_state(s.state);
        if (how == 3 || how == 4) {
            assert(st == TETRA_SM_ON_CC && s.state->last_cc_sync_time != 0);
        } else if (how == 5) {
            assert(st == TETRA_SM_TUNED);
        } else if (how != 6 && how != 8) {
            assert(st == TETRA_SM_IDLE && s.state->last_cc_sync_time == 0);
        }
        const uint64_t pending = dsd_trunk_tuning_pending_request();
        if (pending != 0U) {
            dsd_trunk_tuning_request_publish(pending, DSD_TRUNK_TUNE_RESULT_FAILED);
            tetra_sm_tick(s.opts, s.state);
        }
        session_close(&s);
    }
}

/* Main's display for an I/Q replay of the main carrier recorded off-centre: an input the state machine cannot move is on
 * the main carrier when its centre is within the TETRA chain's tracking range of it (DSD_TETRA_PREMIX_LIMIT_HZ), so the
 * SYSINFO there rests ON_CC, stamps the sync and adds the candidate. Past that range it stays IDLE: 10 kHz off (the
 * repo's tetra_call_offset_10k_72k capture), and a replay of a secondary carrier 25 kHz away (case 2 of
 * test_no_move_unless_the_tuner_is_the_machines()). A live radio the machine can move keeps the exact-Hz rule and is
 * moved from 5 kHz off. */
static void
test_off_centre_replay_of_the_main_carrier(void) {
    static const struct {
        long offset_hz;
        int on_cc;
    } k_cases[] = {
        {5000L, 1}, {-5000L, 1}, {DSD_TETRA_PREMIX_LIMIT_HZ, 1}, {-DSD_TETRA_PREMIX_LIMIT_HZ, 1}, {-10000L, 0},
    };

    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        reset_backend();
        session s = secondary_open(1);
        DSD_SNPRINTF(s.opts->audio_in_dev, sizeof s.opts->audio_in_dev, "%s", "iqreplay:capture.cu8");
        s.opts->iq_replay_center_freq = (uint32_t)(main_hz(k_main) + k_cases[i].offset_hz);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 0 && g_tunes == 0);
        if (k_cases[i].on_cc) {
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
            assert(s.state->tetra_pub.trunk_state == DSD_TETRA_TRUNK_ON_CONTROL);
            assert(s.state->last_cc_sync_time != 0 && cc_candidates(s.state) == 1);
        } else {
            assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
            assert(s.state->last_cc_sync_time == 0 && cc_candidates(s.state) == 0);
        }
        session_close(&s);
    }

    reset_backend();
    session s = secondary_open(1);
    s.opts->rtlsdr_center_freq = (uint32_t)(main_hz(k_main) + 5000L);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && g_returned_hz == main_hz(k_main));
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && s.state->last_cc_sync_time == 0);
    session_close(&s);
}

/* A deferred move counts as none and is staged again by a SYSINFO after TETRA_SM_MOVE_HOLD_S; a refused one waits 5 s,
 * then 15 s, then 60 s at most. */
static void
test_deferred_and_refused_moves(void) {
    reset_backend();
    session s = secondary_open(1);
    s.state->p25_cc_freq = P25_CC_HZ;
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE && s.state->p25_cc_freq == P25_CC_HZ);
    assert(move_of(s.state, main_hz(k_main))->failures == 0);
    at_seconds(1000.0 + TETRA_SM_MOVE_HOLD_S - 0.1);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1);
    g_return_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    double now = 1000.0 + TETRA_SM_MOVE_HOLD_S;
    static const double k_backoff[] = {5.0, 15.0, 60.0, 60.0};
    for (int i = 0; i < 4; i++) {
        at_seconds(now);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 2 + i && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
        assert(s.state->p25_cc_freq == P25_CC_HZ && s.opts->rtlsdr_center_freq == SECONDARY_HZ);
        at_seconds(now + k_backoff[i] - 0.1);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 2 + i);
        now += k_backoff[i];
    }
    session_close(&s);
}

/* A move the backend reported in flight is ACQUIRING with the target cached; one that then fails puts back the receiver
 * centre, the stale P25 alias and the control-channel sync, so the receiver reads as on the secondary carrier again and
 * a SYSINFO heard there cannot pass for the main carrier's. Failures in a row escalate the backoff across the carrier
 * resets between them. */
static void
test_pending_move_fails_and_backs_off(void) {
    reset_backend();
    session s = secondary_open(1);
    s.state->p25_cc_freq = P25_CC_HZ;
    s.state->last_cc_sync_time = 55;
    s.state->last_cc_sync_time_m = 55.0;
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    double now = 1000.0;
    static const double k_backoff[] = {5.0, 15.0, 60.0};
    for (int i = 0; i < 3; i++) {
        at_seconds(now);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 1 + i && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
        assert(s.opts->rtlsdr_center_freq == (uint32_t)main_hz(k_main) && s.state->p25_cc_freq == 0);
        assert(tetra_sm_receiver_freq(s.opts, s.state) == 0 && s.state->last_cc_sync_time == 55);
        dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE && dsd_trunk_tuning_pending_request() == 0U);
        assert(s.opts->rtlsdr_center_freq == SECONDARY_HZ && tetra_sm_receiver_freq(s.opts, s.state) == SECONDARY_HZ);
        assert(s.state->p25_cc_freq == P25_CC_HZ && s.state->last_cc_sync_time == 55);
        assert(same_s(s.state->last_cc_sync_time_m, 55.0) && move_of(s.state, main_hz(k_main))->failures == i + 1);
        /* A carrier reset (a hop) between failures keeps the history. */
        tetra_forget_carrier(s.state);
        tetra_confirm_note_signalling(s.state);
        at_seconds(now + k_backoff[i] - 0.1);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 1 + i && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
        now += k_backoff[i];
    }
    session_close(&s);
}

/* A tune elsewhere while ACQUIRING (the engine's return, a carrier boundary) lets go of the move: a completion that
 * comes later is ignored, consumes nothing, and the next SYSINFO off the main carrier moves again. */
static void
test_external_tune_cancels_acquiring(void) {
    for (int how = 0; how < 2; how++) {
        reset_backend();
        session s = secondary_open(1);
        g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        sysinfo_burst(&s, k_main, 4U);
        const uint64_t request = g_request;
        assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && tetra_sm_owns_return(s.state));
        if (how == 0) {
            tetra_sm_on_external_cc_return(s.opts, s.state);
        } else {
            tetra_sm_abandon_carrier(s.opts, s.state);
        }
        assert(!tetra_sm_owns_return(s.state) && tetra_sm_get_state(s.state) != TETRA_SM_ACQUIRING);
        s.opts->rtlsdr_center_freq = OTHER_HZ;
        tetra_confirm_note_signalling(s.state);
        dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) != TETRA_SM_ACQUIRING && tetra_confirm_carrier_proven(s.state));
        assert(dsd_trunk_tuning_pending_request() == 0U && !move_of(s.state, main_hz(k_main))->consumed);
        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 2 && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
        session_close(&s);
    }

    /* Landed and waiting for the main carrier's SYSINFO: the same. */
    reset_backend();
    session s = secondary_open(1);
    sysinfo_burst(&s, k_main, 4U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && tetra_sm_owns_return(s.state));
    tetra_sm_abandon_carrier(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_owns_return(s.state));
    /* The operator tuned back to the secondary carrier: its SYSINFO names the main carrier moved to already, so no move,
     * and the carrier is no control channel: ON_CC is not claimed there, nor its sync stamped. */
    s.opts->rtlsdr_center_freq = SECONDARY_HZ;
    tetra_confirm_note_signalling(s.state);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE && s.state->last_cc_sync_time == 0);
    assert(s.state->tetra_pub.trunk_state == DSD_TETRA_TRUNK_IDLE && acq_of(s.state)->stay_noted);
    session_close(&s);
}

/* @p ticks of the state machine at @p at_s each, with its narration on, and whether it said @p line. */
static int
ticks_say(session* s, const double* at_s, int ticks, const char* line) {
    dsd_test_capture_stderr cap;
    const int captured = dsd_test_capture_stderr_begin(&cap, "tetra_sm") == 0;
    tetra_st(s->state)->log_verbose = 1;
    for (int i = 0; i < ticks; i++) {
        at_seconds(at_s[i]);
        tetra_sm_tick(s->opts, s->state);
    }
    tetra_st(s->state)->log_verbose = 0;
    char log[2048] = "";
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, sizeof log);
    }
    const char* const first = strstr(log, line);
    assert(!first || !strstr(first + 1, line)); /* never twice */
    return first != NULL;
}

/* On the main carrier a SYSINFO naming another main carrier (a shared channel, or a changed cell) is one more move;
 * one naming the first again is not: a main carrier moved to is moved to once. Nothing moves back to the secondary
 * carrier. The carrier reached was heard, so the move is over (IDLE there, no control-channel sync), and nothing says
 * it sends no SYSINFO (regression: the 10 s line was printed while its SYSINFO was decoded). With no SYSINFO at all on
 * the carrier reached for TETRA_SM_MOVE_SILENT_S the machine says so, once. */
static void
test_main_carrier_naming_another(void) {
    reset_backend();
    session s = secondary_open(1);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && g_returned_hz == main_hz(k_main));
    tetra_confirm_note_signalling(s.state);
    sysinfo_burst(&s, k_other_main, 4U);
    assert(g_returns == 2 && g_returned_hz == main_hz(k_other_main));
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && move_of(s.state, main_hz(k_main))->consumed);
    tetra_confirm_note_signalling(s.state);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 2 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE && s.state->last_cc_sync_time == 0);
    assert(acq_of(s.state)->stay_noted && !tetra_sm_owns_return(s.state));
    const double later[] = {1000.0 + TETRA_SM_MOVE_SILENT_S, 1000.0 + 3.0 * TETRA_SM_MOVE_SILENT_S};
    assert(!ticks_say(&s, later, 2, "no SYSINFO on main carrier"));
    assert(g_returns == 2 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    session_close(&s);

    /* Landed where nothing is heard: said once, at TETRA_SM_MOVE_SILENT_S, and the machine stays there. */
    reset_backend();
    s = secondary_open(1);
    sysinfo_burst(&s, k_main, 4U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
    const double early[] = {1000.0 + TETRA_SM_MOVE_SILENT_S - 0.1};
    assert(!ticks_say(&s, early, 1, "no SYSINFO on main carrier"));
    const double silent[] = {1000.0 + TETRA_SM_MOVE_SILENT_S, 1000.0 + 3.0 * TETRA_SM_MOVE_SILENT_S};
    assert(ticks_say(&s, silent, 2, "no SYSINFO on main carrier"));
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING && g_returns == 1);
    session_close(&s);
}

/* A rigctl peer behind an audio input names no frequency the decoder can read, so the frequency last sent to it says
 * where it is: on the main carrier no move is made and the SYSINFO is the control channel's; elsewhere it moves. */
static void
test_rigctl_peer_on_the_main_carrier(void) {
    for (int on_main = 1; on_main >= 0; on_main--) {
        reset_backend();
        session s = secondary_open(1);
        s.opts->audio_in_type = AUDIO_IN_PULSE;
        s.opts->use_rigctl = 1;
        s.opts->rigctl_sockfd = 7; /* never written: the stand-in hooks tune */
        s.opts->rigctl_sent_freq_hz = on_main ? (uint32_t)main_hz(k_main) : (uint32_t)SECONDARY_HZ;
        sysinfo_burst(&s, k_main, 4U);
        if (on_main) {
            assert(g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
            assert(s.state->last_cc_sync_time != 0);
        } else {
            assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
        }
        s.opts->rigctl_sockfd = DSD_INVALID_SOCKET;
        session_close(&s);
    }
}

/* Trunking turned off and on while a request of the machine's is in flight is seen all the same: the moves start over.
 * And a deferred move never keeps its entry once its hold is over, so carriers the backend deferred never fill the
 * history. */
static void
test_history_reset_and_deferred_entries(void) {
    reset_backend();
    session s = secondary_open(1);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 1 && move_of(s.state, main_hz(k_main))->consumed);
    tetra_sm_abandon_carrier(s.opts, s.state);
    s.opts->rtlsdr_center_freq = SECONDARY_HZ;
    tetra_confirm_note_signalling(s.state);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    const uint64_t request = g_request;
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->trunk_enable = 1;
    tetra_sm_tick(s.opts, s.state);
    assert(dsd_trunk_tuning_pending_request() == request);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(s.opts->rtlsdr_center_freq == SECONDARY_HZ && move_of(s.state, main_hz(k_main)) == NULL);
    tetra_confirm_note_signalling(s.state);
    sysinfo_burst(&s, k_main, 4U);
    assert(g_returns == 2 && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
    session_close(&s);

    /* Main carriers away from the secondary carrier's own number (51). */
    reset_backend();
    s = secondary_open(1);
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    for (uint32_t i = 0; i < TETRA_SM_MOVES; i++) {
        sysinfo_burst(&s, k_other_main + i, 4U);
        assert(g_returns == (int)i + 1);
    }
    /* Every entry held by a deferral: nothing more now, and no "tried already" either. */
    sysinfo_burst(&s, k_other_main + TETRA_SM_MOVES, 4U);
    assert(g_returns == TETRA_SM_MOVES && !acq_of(s.state)->full_noted);
    at_seconds(1000.0 + TETRA_SM_MOVE_HOLD_S);
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    sysinfo_burst(&s, k_other_main + TETRA_SM_MOVES, 4U);
    assert(g_returns == TETRA_SM_MOVES + 1 && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
    assert(move_of(s.state, main_hz(k_other_main + TETRA_SM_MOVES))->consumed);
    session_close(&s);
}

/* The history holds TETRA_SM_MOVES main carriers and never gives one up: with every entry taken nothing more is moved
 * to. An RR import or another source (tetra_sm_forget_moves()), tetra_sm_init() and trunking turned on again forget
 * it, so a main carrier moved to may be moved to again. */
static void
test_move_history_capacity_and_resets(void) {
    reset_backend();
    session s = secondary_open(1);
    for (uint32_t i = 0; i < TETRA_SM_MOVES; i++) {
        sysinfo_burst(&s, k_main + i, 4U);
        assert(g_returns == (int)i + 1 && g_returned_hz == main_hz(k_main + i));
        tetra_confirm_note_signalling(s.state);
    }
    sysinfo_burst(&s, k_main + TETRA_SM_MOVES, 4U);
    assert(g_returns == TETRA_SM_MOVES && acq_of(s.state)->full_noted);
    assert(move_of(s.state, main_hz(k_main + TETRA_SM_MOVES)) == NULL);
    tetra_sm_forget_moves(s.state);
    tetra_confirm_note_signalling(s.state);
    sysinfo_burst(&s, k_main + TETRA_SM_MOVES, 4U);
    assert(g_returns == TETRA_SM_MOVES + 1);
    session_close(&s);

    for (int how = 0; how < 3; how++) {
        reset_backend();
        s = secondary_open(1);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 1 && move_of(s.state, main_hz(k_main))->consumed);
        /* Back on the secondary carrier (a tune elsewhere): the main carrier was moved to already. */
        tetra_sm_abandon_carrier(s.opts, s.state);
        s.opts->rtlsdr_center_freq = SECONDARY_HZ;
        tetra_confirm_note_signalling(s.state);
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE && s.state->last_cc_sync_time == 0);
        if (how == 0) {
            tetra_sm_forget_moves(s.state);
        } else if (how == 1) {
            tetra_sm_init(s.opts, s.state);
            tetra_confirm_note_signalling(s.state);
        } else {
            s.opts->trunk_enable = 0;
            tetra_sm_tick(s.opts, s.state);
            s.opts->trunk_enable = 1;
            tetra_sm_tick(s.opts, s.state);
        }
        sysinfo_burst(&s, k_main, 4U);
        assert(g_returns == 2 && tetra_sm_get_state(s.state) == TETRA_SM_ACQUIRING);
        session_close(&s);
    }
}

/* A tune another owner asked for after a request of the machine's, which the backend refused or deferred where it was
 * asked, moved nothing: the request's later failure still puts the receiver back (regression: any newer request blocked
 * the restore and left the centre cached on the carrier the receiver never reached). A P25 alias someone wrote since
 * stays theirs, and a newer tune the backend accepted decides the receiver. */
static void
test_restore_after_another_owners_request(void) {
    for (int refused = 0; refused < 2; refused++) {
        reset_backend();
        session s = session_open();
        g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
        assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1 && s.opts->rtlsdr_center_freq == VC_HZ);
        const uint64_t request = g_request;
        g_tune_result = refused ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_DEFERRED;
        uint64_t other = 0U;
        assert(!dsd_trunk_tune_result_is_ok(dsd_trunk_tuning_hook_tune_to_freq(s.opts, s.state, VC2_HZ, 0, &other)));
        assert(other > request && !dsd_trunk_tuning_retuned_since(request));
        dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
        tetra_sm_tick(s.opts, s.state);
        assert(s.opts->rtlsdr_center_freq == CC_HZ && tetra_sm_receiver_freq(s.opts, s.state) == CC_HZ);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && dsd_trunk_tuning_pending_request() == 0U);
        session_close(&s);
    }

    /* A return in flight; another owner writes the P25 alias: the centre comes back, the alias stays theirs. */
    reset_backend();
    session s = session_open();
    const tetra_sm_grant g = grant_of(VC_HZ, 0x4U, 0x123U);
    assert(tetra_sm_on_grant(s.opts, s.state, &g) == 1);
    s.state->p25_cc_freq = P25_CC_HZ;
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    uint64_t request = g_request;
    assert(s.state->p25_cc_freq == 0 && s.opts->rtlsdr_center_freq == CC_HZ);
    s.state->p25_cc_freq = OTHER_HZ;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(s.opts->rtlsdr_center_freq == VC_HZ && s.state->p25_cc_freq == OTHER_HZ);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED);

    /* A return in flight overtaken by a tune the backend accepted: neither the centre nor the alias comes back. */
    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    s.state->p25_cc_freq = P25_CC_HZ;
    tetra_sm_on_release(s.opts, s.state, 0x123U);
    request = g_request;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    uint64_t other = 0U;
    assert(dsd_trunk_tuning_hook_tune_to_freq(s.opts, s.state, VC2_HZ, 0, &other) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(dsd_trunk_tuning_retuned_since(request) && s.opts->rtlsdr_center_freq == VC2_HZ);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(s.opts->rtlsdr_center_freq == VC2_HZ && s.state->p25_cc_freq == 0);
    dsd_trunk_tuning_request_publish(other, DSD_TRUNK_TUNE_RESULT_OK);
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
    test_late_failure_after_another_tune_puts_back_no_receiver();
    test_external_return();
    test_abandon_carrier();
    test_released_elsewhere_and_trunking_off();
    test_init_retires_pending();
    test_dropped_state_retires_pending();
    test_in_flight_tune_survives_letting_go();
    test_confirm_evidence();
    test_secondary_carrier_moves_to_the_main_carrier();
    test_move_needs_prior_evidence_on_the_carrier();
    test_no_main_carrier_no_move();
    test_no_move_unless_the_tuner_is_the_machines();
    test_off_centre_replay_of_the_main_carrier();
    test_deferred_and_refused_moves();
    test_pending_move_fails_and_backs_off();
    test_external_tune_cancels_acquiring();
    test_main_carrier_naming_another();
    test_move_history_capacity_and_resets();
    test_restore_after_another_owners_request();
    test_rigctl_peer_on_the_main_carrier();
    test_history_reset_and_deferred_entries();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_system();
    DSD_FPRINTF(stdout, "TETRA_TRUNK_SM: OK\n");
    return 0;
}
