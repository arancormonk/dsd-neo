// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>

#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/nxdn/nxdn_cc_hunt.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "engine_hooks_install.h"

static int
scan_visit_should_yield_from_frame(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    if (dsd_engine_signal_search_active(state)) {
        return dsd_engine_signal_search_should_yield(opts, state);
    }
    if (opts->trunk_scan_enabled == 1) {
        return dsd_engine_trunk_scan_visit_expired(opts, state) || dsd_engine_trunk_scan_receive_expired(opts, state);
    }
    if (dsd_engine_trunk_receive_release_due(opts, state)) {
        return 1;
    }
    const double now_m = dsd_decode_now_mono_s();
    dsd_engine_scan_visit_tick(opts, state, now_m);
    dsd_engine_scan_y_timing_tick(opts, state, now_m, dsd_decode_now_realtime_s());
    return dsd_engine_scan_visit_expired(opts, state, now_m)
           || (opts->scanner_mode == 1 && !state->lcn_scan_hold
               && dsd_scan_receive_should_leave(opts, state, state->scan_voice_gate_arrive_m, now_m)
               && dsd_engine_channel_scan_has_other_row(opts, state));
}

static void
p25_sm_release_from_frame_sync(dsd_opts* opts, dsd_state* state) {
    if (!dsd_trunk_p25_recovery_allowed(opts, state)) {
        return;
    }
    /* Frame sync runs outside processFrame()'s hold on the tick guard, and a release drains the P25 Phase 2 playout
       and ends calls the watchdog's tick reads too: it runs under the guard. A no-sync pass that finds the guard taken
       leaves the release to the next one (issue #651). */
    if (!p25_sm_tick_guard_try_enter()) {
        return;
    }
    state->p25_sm_force_release = 1;
    p25_sm_release(p25_sm_get_ctx(), opts, state, "frame-sync-no-sync");
    p25_sm_tick_guard_leave();
}

static void
p25_sm_vc_sync_from_frame_sync(dsd_opts* opts, const dsd_state* state) {
    if (!dsd_trunk_p25_recovery_allowed(opts, state)) {
        return;
    }
    p25_sm_note_vc_frame_sync(p25_sm_get_ctx(), opts, state);
}

static void
p25_sm_vc_no_sync_from_frame_sync(dsd_opts* opts, const dsd_state* state) {
    if (!dsd_trunk_p25_recovery_allowed(opts, state)) {
        return;
    }
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_note_cc_no_sync_pass(ctx, opts, state);
    p25_sm_note_vc_no_sync_pass(ctx, opts, state);
}

/* Decoder-thread trunking timers a silent UDP input must not starve (issue #703): a startup control-channel hunt, armed
   or due to arm (a session that became eligible after its pass's tick, or whose tick found the guard busy), or DMR
   recovery, whose ticks run on this thread only. Trunking turned off is no exception: an armed window or a hunt still
   needs the tick that lets it go. A busy guard counts as due: leaving the wait costs one pass. */
static int
engine_decoder_ticks_due(const dsd_opts* opts, const dsd_state* state) {
    if (dsd_engine_analog_call_due(state)) {
        return 1;
    }
    if (!opts || !state || opts->trunk_scan_enabled == 1) {
        return 0;
    }
    if (!p25_sm_tick_guard_try_enter()) {
        return 1;
    }
    const dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    const int due = p25_sm_cold_start_due(p25_sm_get_ctx(), opts, state) || dmr_sm_cold_start_due(dmr, opts, state)
                    || (dsd_trunk_dmr_recovery_allowed(opts, state) && dmr_sm_get_state(dmr) != DMR_SM_IDLE)
                    || nxdn_cc_hunt_due(opts, state);
    p25_sm_tick_guard_leave();
    return due;
}

/* The probe answer engine_p25_probe_in_flight() last read, which stands while the watchdog holds the tick guard.
   Decoder thread only; every run starts it over (dsd_engine_frame_sync_hooks_install()). */
static int s_p25_probe_last_answer = 0;

/* A P25-driven control-channel probe in flight (the startup hunt, or recovery under a P25 owner; no other protocol
   owns the control channel), for the frame-sync SPS hunt's rotation. The hunt asks once per step on the decoder
   thread, holding the tick guard or not; the owner and the machine are read under the guard, and while the watchdog
   holds it the last answer stands. Without trunking no probe runs: the startup hunt parks on its next tick and the
   watchdog ticks only an admitted recovery, so a machine still in HUNTING is no probe, and a conventional session
   never takes the guard here. */
static int
engine_p25_probe_in_flight(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1) {
        s_p25_probe_last_answer = 0;
        return 0;
    }
    const int held = p25_sm_tick_guard_held_by_caller();
    if (!held && !p25_sm_tick_guard_try_enter()) {
        return s_p25_probe_last_answer;
    }
    s_p25_probe_last_answer = (state->trunk_recovery_protocol == (int)DSD_TRUNK_RECOVERY_UNKNOWN
                               || state->trunk_recovery_protocol == (int)DSD_TRUNK_RECOVERY_P25)
                              && p25_sm_hunt_probe_in_flight(p25_sm_get_ctx());
    if (!held) {
        p25_sm_tick_guard_leave();
    }
    return s_p25_probe_last_answer;
}

/* The tick guard for a P25p1 chain step of the frame sync, re-entrant: the watchdog's tunes run under it, so a step
   taken under it cannot interleave with one (frame_sync_p25p1_compare.c). */
static int
engine_tick_guard_enter(void) {
    if (p25_sm_tick_guard_held_by_caller()) {
        return 0;
    }
    p25_sm_tick_guard_enter();
    return 1;
}

void
dsd_engine_frame_sync_hooks_install(void) {
    s_p25_probe_last_answer = 0; // a previous run's probe is not this run's
    dsd_frame_sync_hooks hooks = {0};
    hooks.receive_policy_boundary = dsd_engine_receive_policy_boundary;
    hooks.analog_call_block = dsd_engine_analog_call_block;
    hooks.p25_sm_try_tick = p25_sm_try_tick;
    hooks.decoder_ticks_due = engine_decoder_ticks_due;
    hooks.p25_probe_in_flight = engine_p25_probe_in_flight;
    hooks.tick_guard_enter = engine_tick_guard_enter;
    hooks.tick_guard_leave = p25_sm_tick_guard_leave;
    hooks.p25_sm_release = p25_sm_release_from_frame_sync;
    hooks.p25_sm_vc_sync = p25_sm_vc_sync_from_frame_sync;
    hooks.p25_sm_vc_no_sync = p25_sm_vc_no_sync_from_frame_sync;
    hooks.eot_cc = eot_cc;
    hooks.no_carrier = noCarrier;
    hooks.scan_visit_should_yield = scan_visit_should_yield_from_frame;
    hooks.signal_search_should_yield = dsd_engine_signal_search_should_yield;
    hooks.replay_retune = dsd_engine_leave_replay_carrier;
    dsd_frame_sync_hooks_set(&hooks);
}
