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

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
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

/* A frame on the input in use; a switch the command pump then drains replaces the stream under the synced loop. The
   replay phase's frame instead carries a recorded retune (decode_frame_across_a_recorded_retune()). */
static void decode_frame_across_a_recorded_retune(dsd_opts* opts, dsd_state* state);
static int g_in_frame_retune = 0;

void
processFrame(dsd_opts* opts, dsd_state* state) {
    if (g_in_frame_retune) {
        decode_frame_across_a_recorded_retune(opts, state);
        g_in_frame_retune = 0;
        return;
    }
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

/*
 * Issue #575: an I/Q replay plays the retunes its capture recorded, which the read path adopts as the centre of the
 * sample it just read (dsd_frame_sync_note_replay_center()). With trunking off a recorded retune is another
 * conventional carrier, so the reception ends across it as at an accepted live retune, right where the read adopts the
 * new centre and before that sample reaches the protocol: a retune can land inside a frame, whose decoding goes on
 * publishing calls before the next frame-sync return. The outgoing call ends and commits with the colour code it was
 * heard with; the incoming one, decoded after the adoption in the same frame, carries the new carrier's frequency and
 * code, and nothing ends it later. With trunking on a recorded retune is the system following itself, and nothing
 * changes. Each frame-sync pass here is a hunt read that left for a queued command, so no noCarrier() runs between
 * them.
 */
static int g_replay_step = 0;

static void
begin_call_with_target(dsd_state* state, uint32_t target) {
    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, 5678U, target);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.policy_target_id = target;
    expect("a call begins", dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
}

static uint32_t
slot0_target(const dsd_state* state) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               ? (uint32_t)call.ota_target_id
               : 0U;
}

/* The frame the in-frame retune lands in: its later samples are the new carrier's. The read that takes the first of
   them adopts the new centre, and the frame then decodes the incoming call on a radio input. */
static void
decode_frame_across_a_recorded_retune(dsd_opts* opts, dsd_state* state) {
    opts->audio_in_type = AUDIO_IN_RTL;
    dsd_frame_sync_note_replay_center(opts, state, 860000000U);
    expect("the read that adopts the new centre ends the outgoing call", slot0_target(state) == 0U);
    expect("the outgoing call commits with the colour code it was heard with",
           state->event_history_s[0].Event_History_Items[1].target_id == 1234U
               && state->event_history_s[0].Event_History_Items[1].sys_id2 == 5U);
    expect("the incoming carrier starts with no colour code", state->dmr_color_code == 16U);
    state->dmr_color_code = 9U;
    begin_call_with_target(state, 4321U);
    dsd_event_sync_slot(opts, state, 0U);
    opts->audio_in_type = AUDIO_IN_NULL;
}

/* A clean DMR voice LC header for a group call on slot 0: FLCO 0, @p tg from @p src. */
static void
decode_dmr_group_voice_lc(dsd_opts* opts, dsd_state* state, uint32_t tg, uint32_t src) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof bits);
    for (unsigned int i = 0U; i < 24U; i++) {
        bits[24U + i] = (uint8_t)((tg >> (23U - i)) & 1U);
        bits[48U + i] = (uint8_t)((src >> (23U - i)) & 1U);
    }
    uint32_t errors = 0U;
    state->currentslot = 0;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dmr_flco(opts, state, bits, 1U, &errors, 1U);
}

static int
replay_retune_step(dsd_opts* opts, dsd_state* state) {
    g_replay_step++;
    state->input_interrupted = 1;
    const Event_History* staged = &state->event_history_s[0].Event_History_Items[0];
    switch (g_replay_step) {
        case 1:
            begin_call(state);
            state->dmr_color_code = 5U;
            dsd_frame_sync_note_replay_center(opts, state, 851012500U);
            return DSD_SYNC_NONE;
        case 2:
            expect("a replay's first centre ends nothing", slot0_target(state) == 1234U);
            expect("a replay's first centre forgets nothing", state->dmr_color_code == 5U);
            g_in_frame_retune = 1;
            return DSD_SYNC_DMR_BS_VOICE_POS;
        case 3:
            expect("the frame across the retune was processed", g_in_frame_retune == 0);
            expect("the incoming call is open", slot0_target(state) == 4321U);
            expect("the incoming call carries the new carrier's frequency",
                   staged->target_id == 4321U && staged->freq_hz == 860000000);
            expect("the incoming call carries the new carrier's colour code",
                   staged->access_code_kind == (uint8_t)DSD_ACCESS_CODE_COLOR_CODE && staged->access_code == 9U);
            return DSD_SYNC_NONE;
        case 4:
            expect("no late cleanup ends the incoming call", slot0_target(state) == 4321U);
            expect("no late cleanup erases the incoming colour code", state->dmr_color_code == 9U);
            /* Trunking on: a recorded retune is the system following itself. */
            opts->trunk_enable = 1;
            state->dmr_color_code = 7U;
            dsd_frame_sync_note_replay_center(opts, state, 851012500U);
            expect("a recorded trunked retune ends nothing", slot0_target(state) == 4321U);
            expect("a recorded trunked retune forgets nothing", state->dmr_color_code == 7U);
            opts->trunk_enable = 0;
            dsd_opts_forget_iq_replay_center(opts);
            (void)dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_EXPLICIT);
            state->input_interrupted = 0;
            return DSD_SYNC_NONE;
        case 5:
            /* A trunking session following a voice channel heard within the last seconds, which noCarrier() keeps as a
               fade would want it, has its radio input replaced (issue #575). */
            opts->trunk_enable = 1;
            opts->trunk_is_tuned = 1;
            state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 851012500L;
            state->last_vc_sync_time = dsd_decode_time();
            state->last_cc_sync_time = dsd_decode_time();
            begin_call(state);
            state->input_boundary = 1;
            return DSD_SYNC_NONE;
        case 6:
            expect("a trunked input switch ends the followed call", slot0_target(state) == 0U);
            expect("a trunked input switch leaves the followed voice channel",
                   opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
            decode_dmr_group_voice_lc(opts, state, 4700U, 4701U);
            expect("the new input's call is open", slot0_target(state) == 4700U);
            expect("the new input's call takes no frequency of the voice channel left behind", staged->freq_hz == 0);
            opts->trunk_enable = 0;
            (void)dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_EXPLICIT);
            state->input_interrupted = 0;
            return DSD_SYNC_NONE;
        default: return DSD_SYNC_NONE;
    }
}

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    if (g_replay_step < 6) {
        return replay_retune_step(opts, state);
    }
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

/*
 * Issue #575: the watchdog writes the P25 state machine, the followed assignment and the call state under the P25 SM
 * tick guard. An input switch, a tune or a source change during live trunking runs the carrier boundary beside its
 * ticks, so the boundary holds the guard from the state machines' inspection through the teardown: taken when the
 * caller does not hold it, kept (never taken again: it is not re-entrant) when the caller does. Each state machine
 * access the boundary makes is checked for the hold while it is watched.
 */
static int g_watch_tick_guard = 0;
static int g_sm_calls_guarded = 0;
static int g_sm_calls_unguarded = 0;

static void
note_sm_call_guard(void) {
    if (!g_watch_tick_guard) {
        return;
    }
    if (p25_sm_tick_guard_try_enter()) {
        p25_sm_tick_guard_leave();
        g_sm_calls_unguarded++;
    } else {
        g_sm_calls_guarded++;
    }
}

// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
p25_sm_ctx_t* __real_p25_sm_get_ctx(void);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
p25_sm_ctx_t* __wrap_p25_sm_get_ctx(void);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
dmr_sm_ctx_t* __real_dmr_sm_get_ctx(void);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
dmr_sm_ctx_t* __wrap_dmr_sm_get_ctx(void);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __real_p25_sm_abandon_carrier(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_p25_sm_abandon_carrier(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __real_dmr_sm_abandon_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_dmr_sm_abandon_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __real_dsd_engine_release_tuned_call_state(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_dsd_engine_release_tuned_call_state(dsd_opts* opts, dsd_state* state);

p25_sm_ctx_t*
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_p25_sm_get_ctx(void) {
    note_sm_call_guard();
    return __real_p25_sm_get_ctx();
}

dmr_sm_ctx_t*
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dmr_sm_get_ctx(void) {
    note_sm_call_guard();
    return __real_dmr_sm_get_ctx();
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_p25_sm_abandon_carrier(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    note_sm_call_guard();
    __real_p25_sm_abandon_carrier(ctx, opts, state, reason);
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dmr_sm_abandon_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    note_sm_call_guard();
    __real_dmr_sm_abandon_carrier(ctx, opts, state, reason);
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_engine_release_tuned_call_state(dsd_opts* opts, dsd_state* state) {
    note_sm_call_guard();
    __real_dsd_engine_release_tuned_call_state(opts, state);
}

static void
check_the_boundary_holds_the_tick_guard(dsd_opts* opts, dsd_state* state) {
    for (int caller_holds = 0; caller_holds < 2; caller_holds++) {
        p25_sm_ctx_t* p25 = p25_sm_get_ctx();
        dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
        p25->state = P25_SM_TUNED;
        dmr->state = DMR_SM_TUNED;
        opts->trunk_enable = 1;
        opts->trunk_is_tuned = 1;
        g_sm_calls_guarded = 0;
        g_sm_calls_unguarded = 0;
        if (caller_holds) {
            p25_sm_tick_guard_enter();
        }
        g_watch_tick_guard = 1;
        dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_INPUT_SWITCH, caller_holds);
        g_watch_tick_guard = 0;
        expect(caller_holds ? "under the caller's hold, the boundary inspects and tears down only inside it"
                            : "the boundary takes the guard before it inspects the state machines and holds it through "
                              "the teardown",
               g_sm_calls_unguarded == 0 && g_sm_calls_guarded >= 5);
        expect("the boundary left both followed voice channels",
               p25_sm_get_state(p25) != P25_SM_TUNED && dmr->state != DMR_SM_TUNED && opts->trunk_is_tuned == 0);
        const int guard_free = p25_sm_tick_guard_try_enter();
        if (guard_free) {
            p25_sm_tick_guard_leave();
        }
        if (caller_holds) {
            expect("the caller's hold is kept", guard_free == 0);
            p25_sm_tick_guard_leave();
        } else {
            expect("the hold the boundary took is released", guard_free == 1);
        }
    }
    opts->trunk_enable = 0;
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
    check_the_boundary_holds_the_tick_guard(opts, state);

    freeState(state);
    free(state);
    free(opts);
    if (g_failures == 0) {
        printf("ENGINE_INPUT_BOUNDARY: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
