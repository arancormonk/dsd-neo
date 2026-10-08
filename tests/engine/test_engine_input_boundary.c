// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The engine loop's side of an input switch (issue #634), run for real around a scripted getFrameSync():
 *  - a hunt read that left a silent input's wait for a queued command (input_interrupted) ended no reception, so the
 *    pass that follows skips noCarrier() and the open call stays open;
 *  - a replaced stream (input_boundary) ends the reception: noCarrier() runs and every call ends as a teardown, so the
 *    next source's call with the same identity is a new call, also when the switch drained inside the synced-frames
 *    loop, which breaks on it;
 *  - an input that ended (input_fallback_pending) is replaced by the Pulse input between frames, under the tick guard,
 *    as a new reception, unless a command replaced it first; the session ends when Pulse does not open;
 *  - a rigctl peer that comes to demodulate the input with the fallback (a symbol replay ended onto Pulse) is asked for
 *    the session's width under the same guard, as at start (issue #621); one that demodulated the ended input already
 *    is asked nothing new.
 * The input switch and the rigctl requests are link-time fakes (CORE_AUDIO_INPUT_SWITCH tests the real switch,
 * IO_RIGCTL_CONTROL the real requests).
 */

#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_failures = 0;
static int g_step = 0;
static int g_switch_calls = 0;
static int g_switch_result = DSD_AUDIO_INPUT_SWITCHED;
static int g_switch_guarded = 0;
static int g_reconfigure_calls = 0;
static int g_reconfigure_guarded = 0;
static int g_process_frame_calls = 0;
/* The passbands asked of the rigctl peer (issue #621), the last one, whether under the tick guard, and the marks. */
static int g_rowmod_calls = 0;
static int g_rowmod_kind = -1;
static int g_rowmod_bw = 0;
static int g_rowmod_guarded = 0;
static int g_session_marks = 0;
/* A rigctl socket no request reaches: every request to it is a fake below. */
static const dsd_socket_t k_peer_sockfd = (dsd_socket_t)987654;

static void
expect(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

/* A frame on the input in use; a switch the command pump then drains replaces the stream under the synced loop. */
void
processFrame(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_process_frame_calls++;
    state->input_boundary = 1;
}

static uint8_t
call_phase(const dsd_state* state, uint8_t* reason) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    if (dsd_call_state_get(state, 0U, &call) <= 0) {
        *reason = 0xFFU;
        return 0xFFU;
    }
    *reason = call.end_reason;
    return call.phase;
}

/* What the symbol grid keeps of a stream: raw samples its matched-filter seam would replay. */
static void
dirty_matched_filter(dsd_state* state) {
    state->matched_filter.kind = 1;
    state->matched_filter.sps = 10;
    state->matched_filter.delay = 36;
    for (int i = 0; i < 36; i++) {
        state->matched_filter.raw[i] = 12000.0f;
    }
    state->matched_filter.raw_head = 36;
    state->matched_filter.raw_count = 36;
    state->matched_filter.replay = 36;
}

/* What the hunt keeps of a stream: a 2400/4 profile it proved (NXDN48, dPMR), which holds off 4800/4 syncs a while. */
static void
prove_profile(dsd_state* state) {
    state->profile_proof_valid = 1;
    state->profile_proof_idx = 2;
    state->profile_proof_symbolcnt = 4800U;
}

static void
begin_call(dsd_state* state) {
    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, 5678U, 1234U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.policy_target_id = 1234U;
    expect("a call begins", dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
}

// GNU ld --wrap requires these exact external symbol names.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_dsd_audio_switch_input(dsd_opts* opts, dsd_state* state, const dsd_audio_input_request* req);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_getFrameSync(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
bool __wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind);

/* The session's width asked of the peer, under the tick guard: the watchdog's retunes use the same peer record. */
bool
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    expect("the width goes to the session's peer", sockfd == k_peer_sockfd);
    g_rowmod_calls++;
    g_rowmod_kind = kind;
    g_rowmod_bw = bandwidth;
    g_rowmod_guarded = !p25_sm_tick_guard_try_enter();
    if (!g_rowmod_guarded) {
        p25_sm_tick_guard_leave();
    }
    return true;
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind) {
    (void)sockfd;
    (void)kind;
    g_session_marks++;
}

/* A symbol replay with -fA, an explicit NFM width and a rigctl peer, which ends: symbols are not peer audio, so the
   peer was asked nothing at start, and the fallback onto Pulse brings it to demodulate the input. */
static void
end_a_symbol_replay_with_a_peer(dsd_opts* opts, dsd_state* state) {
    opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    opts->symbolfile = NULL;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = k_peer_sockfd;
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = 20000;
    state->input_fallback_pending = 1;
    state->input_interrupted = 1;
}

/* The output follows the fallback's Pulse input under the tick guard too: the watchdog may flush audio into the stream
   a reconfigure closes. */
int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    (void)opts;
    g_reconfigure_calls++;
    g_reconfigure_guarded = !p25_sm_tick_guard_try_enter();
    if (!g_reconfigure_guarded) {
        p25_sm_tick_guard_leave();
    }
    return 0;
}

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_audio_switch_input(dsd_opts* opts, dsd_state* state, const dsd_audio_input_request* req) {
    (void)state;
    g_switch_calls++;
    /* Under the guard: a tick that tried to run now would be refused. */
    g_switch_guarded = !p25_sm_tick_guard_try_enter();
    if (!g_switch_guarded) {
        p25_sm_tick_guard_leave();
    }
    expect("the fallback asks for the configured pulse device", req->kind == DSD_AUDIO_INPUT_PULSE && !req->path);
    if (g_switch_result == DSD_AUDIO_INPUT_SWITCHED) {
        opts->audio_in_type = AUDIO_IN_PULSE;
    }
    return g_switch_result;
}

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    uint8_t reason = 0U;
    g_step++;
    switch (g_step) {
        case 1:
            /* An open call, the symbol grid's history of the stream, and a hunt read that left for a queued command. */
            begin_call(state);
            dirty_matched_filter(state);
            prove_profile(state);
            state->input_interrupted = 1;
            return DSD_SYNC_NONE;
        case 2:
            expect("an interrupted pass skips noCarrier: the call stays open",
                   call_phase(state, &reason) == DSD_CALL_PHASE_ACTIVE);
            expect("the interruption is consumed", state->input_interrupted == 0);
            expect("the same stream keeps the grid's history",
                   state->matched_filter.raw_count == 36 && state->matched_filter.replay == 36);
            expect("the same stream keeps its acquisition proof",
                   state->profile_proof_valid == 1 && state->profile_proof_symbolcnt == 4800U);
            /* Now a command replaced the stream, whose grants named a voice frequency within the last ten seconds, so
               no pass forgets it as stale (issue #575). */
            state->dstar_confirmed = 1;
            state->dmr_color_code = 5U;
            state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 851012500L;
            state->last_cc_sync_time = dsd_decode_time();
            state->input_boundary = 1;
            state->input_interrupted = 1;
            return DSD_SYNC_NONE;
        case 3:
            expect("a replaced stream ends the call as a teardown",
                   call_phase(state, &reason) == DSD_CALL_PHASE_ENDED && reason == DSD_CALL_END_EXPLICIT);
            expect("the new stream inherits no confirmation", state->dstar_confirmed == 0);
            expect("the boundary is consumed", state->input_boundary == 0);
            /* The old stream's call commits as a teardown before noCarrier() forgets what that stream decoded, so its
               row keeps the colour code it was heard with in every field (issue #575). */
            expect("the replaced stream's call commits with its colour code",
                   state->event_history_s[0].Event_History_Items[1].target_id == 1234U
                       && state->event_history_s[0].Event_History_Items[1].sys_id2 == 5U);
            expect("the new stream inherits no colour code", state->dmr_color_code == 16U);
            expect("the new stream inherits no voice frequency the old one's grants named",
                   state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
            state->trunk_vc_freq[0] = 851012500L;
            state->last_cc_sync_time = dsd_decode_time();
            expect("nothing of the old stream is replayed into the new one",
                   state->matched_filter.raw_count == 0 && state->matched_filter.replay == 0
                       && state->matched_filter.kind == 0 && state->matched_filter.raw[0] == 0.0f);
            expect("the old stream's acquisition proof does not hold the new one's hunt",
                   state->profile_proof_valid == 0 && state->profile_proof_symbolcnt == 0U
                       && state->profile_proof_idx == 0);
            begin_call(state);
            return DSD_SYNC_NONE;
        case 4:
            expect("a plain pass ends the call as a fade",
                   call_phase(state, &reason) == DSD_CALL_PHASE_ENDED && reason == DSD_CALL_END_SYNC_LOSS);
            expect("a plain pass keeps a recent grant's voice frequency", state->trunk_vc_freq[0] == 851012500L);
            /* A sync, whose frame is followed by a switch drained in the synced loop. */
            begin_call(state);
            state->dstar_confirmed = 1;
            return DSD_SYNC_P25P1_POS;
        case 5:
            expect("the synced loop breaks on a replaced stream, and the outer loop ends the reception",
                   g_process_frame_calls == 1 && call_phase(state, &reason) == DSD_CALL_PHASE_ENDED
                       && reason == DSD_CALL_END_EXPLICIT && state->dstar_confirmed == 0);
            /* The file ended: the reader closed it and hands the switch to Pulse to the engine. */
            opts->audio_in_type = AUDIO_IN_WAV;
            opts->audio_in_file = NULL;
            state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
            state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
            state->analog_rx.ctcss_tenths_hz = 1000;
            begin_call(state);
            state->input_fallback_pending = 1;
            state->input_interrupted = 1;
            return DSD_SYNC_NONE;
        case 6:
            expect("the fallback switched once", g_switch_calls == 1);
            expect("the fallback ran under the tick guard", g_switch_guarded == 1);
            expect("the output followed under it too", g_reconfigure_calls == 1 && g_reconfigure_guarded == 1);
            expect("the fallback is consumed", state->input_fallback_pending == 0);
            expect("pulse runs", opts->audio_in_type == AUDIO_IN_PULSE);
            expect("the file's tone goes with it",
                   state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.ctcss_tenths_hz == 0);
            expect("the fallback is a new reception",
                   call_phase(state, &reason) == DSD_CALL_PHASE_ENDED && reason == DSD_CALL_END_EXPLICIT);
            /* The file ended mid-frame, and a command pumped in the synced-frames loop replaced it before the engine
               got to the fallback. */
            opts->audio_in_type = AUDIO_IN_UDP;
            state->input_fallback_pending = 1;
            state->input_boundary = 1;
            state->input_interrupted = 1;
            return DSD_SYNC_NONE;
        case 7:
            expect("the fallback leaves the input a command chose",
                   g_switch_calls == 1 && opts->audio_in_type == AUDIO_IN_UDP);
            expect("the fallback is consumed", state->input_fallback_pending == 0);
            expect("the command's boundary is consumed", state->input_boundary == 0);
            expect("no peer, no passband asked", g_rowmod_calls == 0);
            end_a_symbol_replay_with_a_peer(opts, state);
            return DSD_SYNC_NONE;
        case 8:
            expect("the replay fell back to pulse", g_switch_calls == 2 && opts->audio_in_type == AUDIO_IN_PULSE);
            expect("the peer now demodulating the input is asked for the session's width",
                   g_rowmod_calls == 1 && g_rowmod_kind == DSD_ANALOG_DEMOD_FM && g_rowmod_bw == 20000);
            expect("the width was asked under the fallback's tick guard", g_rowmod_guarded == 1);
            expect("off any scan the width is the session's", g_session_marks == 1);
            /* A WAV file the peer already demodulated ends: the request in force is the same on Pulse. */
            opts->audio_in_type = AUDIO_IN_WAV;
            opts->audio_in_file = NULL;
            state->input_fallback_pending = 1;
            state->input_interrupted = 1;
            return DSD_SYNC_NONE;
        case 9:
            expect("the wav file fell back to pulse", g_switch_calls == 3 && opts->audio_in_type == AUDIO_IN_PULSE);
            expect("a peer that demodulated the ended input is asked nothing new", g_rowmod_calls == 1);
            /* A fallback whose Pulse input does not open ends the session, and asks the peer nothing. */
            end_a_symbol_replay_with_a_peer(opts, state);
            g_switch_result = DSD_AUDIO_INPUT_KEPT;
            return DSD_SYNC_NONE;
        default:
            expect("a failed fallback ends the session before another hunt", 0);
            dsd_exitflag_store(1);
            return DSD_SYNC_NONE;
    }
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;

    const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    expect("the engine ran", rc == 0);
    expect("a failed fallback ended the session", g_step == 9 && dsd_exitflag_load() == 1);
    expect("the failed fallback switched", g_switch_calls == 4);
    expect("the failed fallback asked the peer nothing", g_rowmod_calls == 1 && g_session_marks == 1);

    freeState(state);
    free(state);
    free(opts);
    if (g_failures == 0) {
        printf("ENGINE_INPUT_BOUNDARY: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
