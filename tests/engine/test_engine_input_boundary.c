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
 *    as a new reception, unless a command replaced it first; the session ends when Pulse does not open.
 * The input switch itself is a link-time fake (CORE_AUDIO_INPUT_SWITCH tests the real one).
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
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/exitflag.h>
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
            /* Now a command replaced the stream. */
            state->dstar_confirmed = 1;
            state->input_boundary = 1;
            state->input_interrupted = 1;
            return DSD_SYNC_NONE;
        case 3:
            expect("a replaced stream ends the call as a teardown",
                   call_phase(state, &reason) == DSD_CALL_PHASE_ENDED && reason == DSD_CALL_END_EXPLICIT);
            expect("the new stream inherits no confirmation", state->dstar_confirmed == 0);
            expect("the boundary is consumed", state->input_boundary == 0);
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
            /* A fallback whose Pulse input does not open ends the session. */
            opts->audio_in_type = AUDIO_IN_WAV;
            opts->audio_in_file = NULL;
            g_switch_result = DSD_AUDIO_INPUT_KEPT;
            state->input_fallback_pending = 1;
            state->input_interrupted = 1;
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
    expect("a failed fallback ended the session", g_step == 7 && dsd_exitflag_load() == 1);
    expect("the failed fallback switched", g_switch_calls == 2);

    freeState(state);
    free(state);
    free(opts);
    if (g_failures == 0) {
        printf("ENGINE_INPUT_BOUNDARY: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
