// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA trunking state machine (tetra_trunk_sm.h): one followed traffic channel per session.
 *
 * All of it lives in tetra_state_t::sm. Tunes go through the runtime trunk-tuning hooks. A request the backend reports
 * in flight (PENDING) is the backend's until it answers, however long that takes: the frame gate stays closed and the
 * state machine waits. The next entry point or tick after the answer settles it: a completed one commits, a failed one
 * puts the state machine and the shared tuning state back first and then completes the request, so the frame gate it
 * holds opens on restored state (trunk_tuning_hooks.h).
 *
 * The receiver's carrier changes only when a tune completes: then the carrier's evidence, partial TM-SDUs, TDMA clock
 * and speech in progress are forgotten (tetra_forget_carrier()). The pointer to the TETRA state the MAC holds while it
 * calls in here stays valid: nothing here frees or replaces the state extension.
 *
 * A session started on a secondary carrier of the cell learns the main carrier from its SYSINFO, which every carrier
 * broadcasts, and moves there once (tetra_sm_after_burst()): the MCCH, where calls are set up, is on the main carrier
 * (EN 300 392-2 clauses 3.1, 19.3.2.1.1, 23.3.1.2.1).
 */

#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"

enum { SM_PENDING_NONE = 0, SM_PENDING_GRANT = 1, SM_PENDING_RETURN = 2, SM_PENDING_ACQUIRE = 3 };

static const char*
sm_state_name(uint8_t s) {
    switch ((tetra_sm_state_e)s) {
        case TETRA_SM_IDLE: return "IDLE";
        case TETRA_SM_ACQUIRING: return "ACQUIRING";
        case TETRA_SM_ON_CC: return "ON_CC";
        case TETRA_SM_TUNED: return "TUNED";
        default: return "?";
    }
}

/* State-machine narration prints at -v @p level (tetra_log.h); @p ts is never NULL here. */
static int
sm_narrates(const tetra_state_t* ts, int level) {
    return ts->log_verbose >= level;
}

/* A change is published at once: the engine ticks between bursts, and the frontends read the publication. */
static void
sm_set_state(dsd_state* state, tetra_state_t* ts, tetra_sm_state_e next, const char* reason) {
    if (ts->sm.state == (uint8_t)next) {
        return;
    }
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] %s -> %s (%s)\n", sm_state_name(ts->sm.state), sm_state_name((uint8_t)next),
                    reason);
    }
    ts->sm.state = (uint8_t)next;
    tetra_publish(state);
}

/* Where the state machine rests when it follows nothing: on the control channel while trunking with one known. */
static tetra_sm_state_e
sm_rest_state(const dsd_opts* opts, const dsd_state* state) {
    return (opts && opts->trunk_enable == 1 && state->trunk_cc_freq > 0) ? TETRA_SM_ON_CC : TETRA_SM_IDLE;
}

static tetra_sm_rollback
sm_capture(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts) {
    tetra_sm_rollback r;
    DSD_MEMSET(&r, 0, sizeof r);
    r.tuning.trunk_is_tuned = opts->trunk_is_tuned;
    DSD_MEMCPY(r.tuning.p25_vc_freq, state->p25_vc_freq, sizeof r.tuning.p25_vc_freq);
    DSD_MEMCPY(r.tuning.trunk_vc_freq, state->trunk_vc_freq, sizeof r.tuning.trunk_vc_freq);
    r.tuning.last_vc_sync_time = state->last_vc_sync_time;
    r.tuning.last_vc_sync_time_m = state->last_vc_sync_time_m;
    r.tuning.last_cc_sync_time = state->last_cc_sync_time;
    r.tuning.last_cc_sync_time_m = state->last_cc_sync_time_m;
    r.tuning.p25_last_vc_tune_time = state->p25_last_vc_tune_time;
    r.tuning.p25_last_vc_tune_time_m = state->p25_last_vc_tune_time_m;
    r.receiver.center_hz = opts->rtlsdr_center_freq;
    r.receiver.p25_cc_freq = state->p25_cc_freq;
    r.followed = ts->sm.followed;
    r.t_tune_m = ts->sm.t_tune_m;
    r.t_voice_m = ts->sm.t_voice_m;
    r.t_maint_m = ts->sm.t_maint_m;
    r.confirm_epoch = tetra_confirm_carrier_epoch(state);
    r.followed_proven = (uint8_t)tetra_confirm_followed_call_proven(state);
    r.followed_rx = (uint8_t)tetra_confirm_followed_rx(state);
    r.followed_retired = ts->sm.followed_retired;
    r.state = ts->sm.state;
    return r;
}

/* The receiver a failed request @p request_id leaves: back where it was, unless another owner moved it since. A
 * correlated tune begun after it decides where the receiver goes, whatever its target, unless it was refused or
 * deferred where it was asked for, which moved nothing (dsd_trunk_tuning_retuned_since()); a centre or alias written
 * since by anyone else is theirs. A request the state machine let go of (tetra_sm_disown_pending()) never gets here. */
static void
sm_restore_receiver(dsd_opts* opts, dsd_state* state, const tetra_sm_receiver_snapshot* r, uint64_t request_id) {
    if (dsd_trunk_tuning_retuned_since(request_id)) {
        return;
    }
    if (opts->rtlsdr_center_freq == r->staged_center_hz) {
        opts->rtlsdr_center_freq = r->center_hz;
    }
    if (state->p25_cc_freq == r->staged_p25_cc_freq) {
        state->p25_cc_freq = r->p25_cc_freq;
    }
}

static void
sm_restore(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_sm_rollback* r, uint64_t request_id) {
    sm_restore_receiver(opts, state, &r->receiver, request_id);
    opts->trunk_is_tuned = r->tuning.trunk_is_tuned;
    DSD_MEMCPY(state->p25_vc_freq, r->tuning.p25_vc_freq, sizeof state->p25_vc_freq);
    DSD_MEMCPY(state->trunk_vc_freq, r->tuning.trunk_vc_freq, sizeof state->trunk_vc_freq);
    state->last_vc_sync_time = r->tuning.last_vc_sync_time;
    state->last_vc_sync_time_m = r->tuning.last_vc_sync_time_m;
    state->last_cc_sync_time = r->tuning.last_cc_sync_time;
    state->last_cc_sync_time_m = r->tuning.last_cc_sync_time_m;
    state->p25_last_vc_tune_time = r->tuning.p25_last_vc_tune_time;
    state->p25_last_vc_tune_time_m = r->tuning.p25_last_vc_tune_time_m;
    ts->sm.followed = r->followed;
    ts->sm.followed_retired = r->followed_retired;
    ts->sm.t_tune_m = r->t_tune_m;
    ts->sm.t_voice_m = r->t_voice_m;
    ts->sm.t_maint_m = r->t_maint_m;
    /* The receiver never left the carrier: what proved the call there still does, unless the carrier was reset. */
    tetra_confirm_restore_followed_call(state, r->confirm_epoch, r->followed_proven, r->followed_rx);
    sm_set_state(state, ts, (tetra_sm_state_e)r->state, "tune failed; put back");
}

/* The receiver is on the followed channel as of @p at_m: the times the engine's generic no-carrier return and the
 * frontends measure the channel from. */
static void
sm_stamp_tuned(dsd_state* state, double at_m) {
    const time_t wall = dsd_decode_time();
    state->last_vc_sync_time = wall;
    state->last_vc_sync_time_m = at_m;
    state->last_cc_sync_time = wall;
    state->last_cc_sync_time_m = at_m;
    state->p25_last_vc_tune_time = wall;
    state->p25_last_vc_tune_time_m = at_m;
}

/* The shared state of a followed traffic channel the receiver did not have to retune for. */
static void
sm_claim_shared_tuning(dsd_opts* opts, dsd_state* state, long freq_hz) {
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq_hz;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq_hz;
}

static void
sm_release_shared_tuning(dsd_opts* opts, dsd_state* state) {
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
}

static void
sm_follow(dsd_state* state, tetra_state_t* ts, const tetra_sm_grant* grant) {
    ts->sm.followed = *grant;
    ts->sm.followed.timeslots &= 0x0Fu;
    ts->sm.followed.carrier &= 0x0FFFu;
    ts->sm.followed.call_id &= 0x3FFFu;
    ts->sm.followed_retired = 0;
    ts->sm.t_tune_m = 0.0;
    ts->sm.t_voice_m = 0.0;
    ts->sm.t_maint_m = 0.0;
    tetra_confirm_forget_followed_call(state);
}

/* The call stays in the call records (tetra_calls.h), which age it; its canonical call and its talkgroup-policy route
 * go with the following. @p opts is NULL when the engine ended and committed the canonical calls itself. */
static void
sm_unfollow(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    DSD_MEMSET(&ts->sm.followed, 0, sizeof ts->sm.followed);
    ts->sm.t_tune_m = 0.0;
    ts->sm.t_voice_m = 0.0;
    ts->sm.t_maint_m = 0.0;
    tetra_confirm_forget_followed_call(state);
    tetra_calls_unfollowed(opts, state);
}

void
tetra_sm_disown_pending(tetra_sm_t* sm) {
    const uint64_t request_id = sm ? sm->pending_request_id : 0U;
    if (request_id == 0U) {
        return;
    }
    sm->pending_request_id = 0U;
    sm->pending_kind = SM_PENDING_NONE;
    /* The request outlives what the machine staged for it, and may outlive the machine itself, which a decode-mode
     * change frees with the TETRA state: the runtime keeps one the backend has not answered in flight, so its
     * completion is still the tuner boundary the frame gate waits for, and retires its failure, already answered or
     * answered later, with nothing to put back (trunk_tuning_hooks.h). */
    dsd_trunk_tuning_request_disown(request_id);
}

/* Stage request @p request_id the backend reported in flight, with @p prev (sm_capture() before it was asked for) to put
 * back if it fails, and the receiver as the request left it. */
static void
sm_stage_pending(const dsd_opts* opts, const dsd_state* state, tetra_state_t* ts, uint8_t kind, uint64_t request_id,
                 const tetra_sm_rollback* prev) {
    ts->sm.pending_prev = *prev;
    ts->sm.pending_prev.receiver.staged_center_hz = opts->rtlsdr_center_freq;
    ts->sm.pending_prev.receiver.staged_p25_cc_freq = state->p25_cc_freq;
    ts->sm.pending_request_id = request_id;
    ts->sm.pending_kind = kind;
}

/* The receiver reached the followed traffic channel at @p completed_m (0: now). A followed call its own reallocation
 * moved stays the canonical call, on the channel it is followed on now. */
static void
sm_grant_tune_completed(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, double completed_m) {
    opts->trunk_is_tuned = 1;
    ts->sm.t_tune_m = completed_m > 0.0 ? completed_m : dsd_decode_now_mono_s();
    sm_stamp_tuned(state, ts->sm.t_tune_m);
    tetra_forget_carrier(state);
    tetra_calls_followed_moved(opts, state);
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] on traffic channel %ld Hz (carrier %u, timeslots 0x%X, call %u)\n",
                    ts->sm.followed.freq_hz, (unsigned)ts->sm.followed.carrier, (unsigned)ts->sm.followed.timeslots,
                    (unsigned)ts->sm.followed.call_id);
    }
}

/* The receiver is back on the control channel. */
static void
sm_return_completed(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    opts->trunk_is_tuned = 0;
    sm_unfollow(opts, state, ts);
    tetra_forget_carrier(state);
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] back on the control channel %ld Hz\n", state->trunk_cc_freq);
    }
}

/* The history entry of main carrier @p hz, NULL when it has none. */
static tetra_sm_move*
sm_move_find(tetra_state_t* ts, long hz) {
    for (int i = 0; i < TETRA_SM_MOVES; i++) {
        if (ts->sm.acq.moves[i].hz == hz) {
            return &ts->sm.acq.moves[i];
        }
    }
    return NULL;
}

/* An entry that holds nothing a later move needs: free, or only a deferral whose hold is over (a deferred move counts
 * as none, so it never keeps its entry once its hold ends). */
static int
sm_move_reusable(const tetra_sm_move* m, double now_m) {
    return m->hz == 0 || (!m->consumed && m->failures == 0U && now_m >= m->next_m);
}

/* The history entry of main carrier @p hz, a reusable one taken for it when it has none, or NULL when none is: an entry
 * of a carrier moved to stays consumed and one of a failed move keeps its backoff, so neither is ever given up. Only a
 * table of such entries is full (said once); one some deferral still holds frees itself when the hold ends. */
static tetra_sm_move*
sm_move_entry(tetra_state_t* ts, long hz) {
    tetra_sm_move* entry = sm_move_find(ts, hz);
    if (entry) {
        return entry;
    }
    const double now_m = dsd_decode_now_mono_s();
    int held = 0;
    for (int i = 0; i < TETRA_SM_MOVES; i++) {
        tetra_sm_move* const m = &ts->sm.acq.moves[i];
        if (sm_move_reusable(m, now_m)) {
            DSD_MEMSET(m, 0, sizeof *m);
            m->hz = hz;
            return m;
        }
        held |= !m->consumed && m->failures == 0U;
    }
    if (!held && !ts->sm.acq.full_noted) {
        ts->sm.acq.full_noted = 1;
        if (sm_narrates(ts, 1)) {
            DSD_FPRINTF(stderr, "[TETRA SM] not moving to the main carrier %ld Hz: %d main carriers tried already\n",
                        hz, TETRA_SM_MOVES);
        }
    }
    return NULL;
}

static void
sm_forget_moves(tetra_state_t* ts) {
    DSD_MEMSET(ts->sm.acq.moves, 0, sizeof ts->sm.acq.moves);
    ts->sm.acq.staged_hz = 0;
    ts->sm.acq.full_noted = 0;
    ts->sm.acq.stay_noted = 0;
}

/* Trunking turned on again starts the moves to the main carrier over: the operator may have fixed what stopped them.
 * The tick, the SYSINFO entry and the burst's end note trunking before anything else, a request in flight included;
 * the trunk commands forget the moves themselves when they turn trunking on (tetra_sm_forget_moves()), so a toggle
 * none of those sees counts too. */
static void
sm_note_trunking(const dsd_opts* opts, tetra_state_t* ts) {
    if (opts->trunk_enable != 1) {
        ts->sm.acq.trunk_off = 1;
    } else if (ts->sm.acq.trunk_off) {
        ts->sm.acq.trunk_off = 0;
        sm_forget_moves(ts);
    }
}

/* How long the next move to a main carrier waits after @p failures failed in a row: 5 s, then 15 s, then 60 s. */
static double
sm_move_backoff_s(uint8_t failures) {
    if (failures <= 1U) {
        return 5.0;
    }
    return failures == 2U ? 15.0 : 60.0;
}

/* A move to main carrier @p hz was refused or failed. */
static void
sm_move_failed(tetra_state_t* ts, long hz, dsd_trunk_tune_result result) {
    tetra_sm_move* const entry = sm_move_find(ts, hz);
    if (!entry) {
        return;
    }
    if (entry->failures < UINT8_MAX) {
        entry->failures++;
    }
    const double backoff_s = sm_move_backoff_s(entry->failures);
    entry->next_m = dsd_decode_now_mono_s() + backoff_s;
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] move to the main carrier %ld Hz failed (result %d); next try in %.0f s\n", hz,
                    (int)result, backoff_s);
    }
}

/* The receiver reached the main carrier at @p completed_m (0: now). It is consumed: never moved to again. The carrier
 * left is forgotten as after any tune, the history is not: it is this move's own. */
static void
sm_move_completed(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, double completed_m) {
    tetra_sm_move* const entry = sm_move_find(ts, ts->sm.acq.target_hz);
    if (entry) {
        entry->consumed = 1;
        entry->failures = 0;
        entry->next_m = 0.0;
    }
    opts->trunk_is_tuned = 0;
    ts->sm.acq.landed_m = completed_m > 0.0 ? completed_m : dsd_decode_now_mono_s();
    tetra_forget_carrier(state);
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] on the main carrier %ld Hz; waiting for its SYSINFO\n", ts->sm.acq.target_hz);
    }
}

static const char*
sm_pending_name(uint8_t kind) {
    switch (kind) {
        case SM_PENDING_GRANT: return "tune";
        case SM_PENDING_ACQUIRE: return "move to the main carrier";
        default: return "return";
    }
}

/* The backend completed the request of @p kind in flight. */
static void
sm_pending_completed(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, uint8_t kind, double completed_m) {
    if (kind == SM_PENDING_GRANT) {
        sm_grant_tune_completed(opts, state, ts, completed_m);
    } else if (kind == SM_PENDING_ACQUIRE) {
        sm_move_completed(opts, state, ts, completed_m);
    } else {
        sm_return_completed(opts, state, ts);
    }
}

/* Settle the tune in flight. Returns 1 when none is in flight any more. A request the backend has not answered is left
 * pending: completing it here would open the frame gate while the hardware may still move, and the backend's later
 * completion would then publish no tuner boundary (trunk_tuning_hooks.h). */
static int
sm_settle_pending(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    const uint64_t request_id = ts->sm.pending_request_id;
    if (request_id == 0U) {
        return 1;
    }
    double completed_m = 0.0;
    const dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(request_id, &completed_m);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return 0;
    }
    const uint8_t kind = ts->sm.pending_kind;
    ts->sm.pending_request_id = 0U;
    ts->sm.pending_kind = SM_PENDING_NONE;
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        sm_pending_completed(opts, state, ts, kind, completed_m);
        return 1;
    }
    /* Put back first: completing the request opens the frame gate onto the state it finds. */
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] %s failed (result %d)\n", sm_pending_name(kind), (int)result);
    }
    sm_restore(opts, state, ts, &ts->sm.pending_prev, request_id);
    if (kind == SM_PENDING_ACQUIRE) {
        sm_move_failed(ts, ts->sm.acq.target_hz, result);
    }
    dsd_trunk_tuning_request_complete(request_id, result);
    return 1;
}

/* Settle the request in flight at an entry point the decoder reaches while it decodes signalling or speech. A burst
 * being decoded settles nothing: its entry did, once, before any of its blocks (tetra_sm_settle() from tetra.c), so
 * every block of the burst sees one state machine, and an answer that comes during it is taken at the next burst or
 * tick. Until then the request counts as in flight. Outside a burst (the engine, the frontends' commands) the entry
 * point settles as the tick does. */
static int
sm_settle_at_entry(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    if (ts->confirm.burst_open) {
        return ts->sm.pending_request_id == 0U;
    }
    return sm_settle_pending(opts, state, ts);
}

/* Ask the backend for a return to TETRA's own control channel. The shared return prefers the P25 control channel; an
 * alias another protocol left would also bring that protocol's symbol profile, so it is cleared for the request. A
 * request refused or deferred gets it back at once; one in flight leaves it cleared, and its rollback (sm_capture()
 * before this, sm_stage_pending() after) puts it back if it fails. */
static dsd_trunk_tune_result
sm_request_cc_return(dsd_opts* opts, dsd_state* state, uint64_t* request_id) {
    const long p25_cc = state->p25_cc_freq;
    state->p25_cc_freq = 0;
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_return_to_cc(opts, state, request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        state->p25_cc_freq = p25_cc;
    }
    return result;
}

/* Return to the control channel, or stop following on its carrier. Returns 1 when the call was let go (the return
 * done or in flight), 0 when the call is kept: the backend refused or deferred the return, or trunking is off. With
 * trunking off the receiver stays where it is, as DMR's and P25's machines leave it, which do not tick then: the
 * engine's return tunes nothing without trunking (dsd_engine_return_to_cc_request()) yet answers OK, so a return asked
 * for then would report the receiver back on the control channel while it is still on the traffic channel. */
static int
sm_release(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const char* reason) {
    if (opts->trunk_enable != 1) {
        if (sm_narrates(ts, 1)) {
            DSD_FPRINTF(stderr, "[TETRA SM] %s with trunking off; the receiver stays on the followed channel\n",
                        reason);
        }
        return 0;
    }
    const long cc = state->trunk_cc_freq;
    if (cc <= 0 || ts->sm.followed.freq_hz == cc) {
        /* A traffic timeslot of the control channel's carrier, or no control channel to go to: the receiver stays. */
        sm_release_shared_tuning(opts, state);
        sm_unfollow(opts, state, ts);
        sm_set_state(state, ts, sm_rest_state(opts, state), reason);
        return 1;
    }
    const tetra_sm_rollback prev = sm_capture(opts, state, ts);
    uint64_t request_id = 0U;
    const dsd_trunk_tune_result result = sm_request_cc_return(opts, state, &request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        if (sm_narrates(ts, 1)) {
            DSD_FPRINTF(stderr, "[TETRA SM] return to the control channel (%s) %s; call kept\n", reason,
                        result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "deferred" : "refused");
        }
        return 0;
    }
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        sm_stage_pending(opts, state, ts, SM_PENDING_RETURN, request_id, &prev);
        DSD_MEMSET(&ts->sm.followed, 0, sizeof ts->sm.followed);
        sm_set_state(state, ts, sm_rest_state(opts, state), reason);
        return 1;
    }
    sm_return_completed(opts, state, ts);
    sm_set_state(state, ts, sm_rest_state(opts, state), reason);
    return 1;
}

/* Calls are the same when the allocations match and no call identifier says otherwise. A retired followed call is
 * no call record's: no grant is its (tetra_sm_retire_call()). */
static int
sm_same_call(const tetra_sm_t* sm, const tetra_sm_grant* grant) {
    const tetra_sm_grant* const followed = &sm->followed;
    return !sm->followed_retired && followed->freq_hz == grant->freq_hz
           && followed->timeslots == (grant->timeslots & 0x0Fu)
           && (followed->call_id == 0U || grant->call_id == 0U || followed->call_id == (grant->call_id & 0x3FFFu));
}

/* A grant on the carrier the receiver is on: followed without a retune. The grant itself is CRC-passed signalling of
 * the call on this carrier. */
static int
sm_follow_here(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_sm_grant* grant) {
    const int was_tuned = ts->sm.state == (uint8_t)TETRA_SM_TUNED;
    if (was_tuned && sm_same_call(&ts->sm, grant)) {
        if (ts->sm.followed.call_id == 0U) {
            ts->sm.followed.call_id = (uint16_t)(grant->call_id & 0x3FFFu);
        }
        return 0;
    }
    sm_follow(state, ts, grant);
    ts->sm.t_tune_m = dsd_decode_now_mono_s();
    if (!was_tuned) {
        sm_claim_shared_tuning(opts, state, grant->freq_hz);
        sm_stamp_tuned(state, ts->sm.t_tune_m);
    }
    tetra_confirm_note_followed_call(state);
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] follow call %u on this carrier (timeslots 0x%X)\n",
                    (unsigned)ts->sm.followed.call_id, (unsigned)ts->sm.followed.timeslots);
    }
    sm_set_state(state, ts, TETRA_SM_TUNED, "grant on this carrier");
    return 1;
}

static int
sm_tune_grant(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_sm_grant* grant) {
    const tetra_sm_rollback prev = sm_capture(opts, state, ts);
    uint64_t request_id = 0U;
    /* No symbol-timing override: a TETRA traffic carrier keeps the control channel's symbol rate. */
    const dsd_trunk_tune_result result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, grant->freq_hz, 0, &request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        if (sm_narrates(ts, 1)) {
            DSD_FPRINTF(stderr, "[TETRA SM] tune to %ld Hz %s; grant ignored\n", grant->freq_hz,
                        result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "deferred" : "refused");
        }
        return 0;
    }
    sm_follow(state, ts, grant);
    sm_set_state(state, ts, TETRA_SM_TUNED, "grant");
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        sm_stage_pending(opts, state, ts, SM_PENDING_GRANT, request_id, &prev);
        return 1;
    }
    sm_grant_tune_completed(opts, state, ts, 0.0);
    return 1;
}

long
tetra_sm_receiver_freq(const dsd_opts* opts, const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || ts->sm.pending_request_id != 0U) {
        return 0;
    }
    if (ts->sm.state == (uint8_t)TETRA_SM_TUNED) {
        return ts->sm.followed.freq_hz;
    }
    return (long)dsd_opts_tuned_freq_hz(opts);
}

/* A receiver the state machine can move: a live radio stream (an I/Q replay plays the tuning its capture recorded) or a
 * connected rigctl peer behind an audio input. A symbol capture has neither. */
static int
sm_input_tunable(const dsd_opts* opts) {
    if (dsd_opts_input_is_radio(opts)) {
        return !dsd_opts_input_is_iq_replay(opts);
    }
    return dsd_opts_input_is_pcm_audio(opts) && dsd_opts_rigctl_live(opts);
}

/* Whether a receiver on @p rx_hz is on main carrier @p hz. One the state machine can move is on it only at the exact
 * frequency, which a move there reaches. One it cannot move (an I/Q replay, whose centre is what the capture recorded)
 * is on it within the TETRA chain's tracking range, which brings that carrier to the centre: a capture of the main
 * carrier recorded a few kHz off it is still the control channel, while a neighbouring carrier 25 kHz away is not. */
static int
sm_receiver_on(const dsd_opts* opts, long rx_hz, long hz) {
    if (sm_input_tunable(opts)) {
        return rx_hz == hz;
    }
    const long off_hz = rx_hz > hz ? rx_hz - hz : hz - rx_hz;
    return off_hz <= DSD_TETRA_PREMIX_LIMIT_HZ;
}

/* Whether the receiver is on main carrier @p hz: the frequency it is on, when that is known (sm_receiver_on()). With
 * none known (an audio input) it is, as far as anything here can tell, unless the state machine can move the receiver,
 * a rigctl peer: then the frequency last sent to the peer says so (opts->rigctl_sent_freq_hz), or the machine's own
 * completed move there. */
static int
sm_on_main_carrier(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts, long hz) {
    const long rx_hz = tetra_sm_receiver_freq(opts, state);
    if (rx_hz != 0) {
        return sm_receiver_on(opts, rx_hz, hz);
    }
    if (!sm_input_tunable(opts)) {
        return 1;
    }
    if (opts->rigctl_sent_freq_hz != 0U && (long)opts->rigctl_sent_freq_hz == hz) {
        return 1;
    }
    return ts->sm.state == (uint8_t)TETRA_SM_ACQUIRING && ts->sm.acq.target_hz == hz && ts->sm.acq.landed_m > 0.0;
}

/* A SYSINFO decoded off main carrier @p hz: the move there is staged for the end of the burst (tetra_sm_after_burst()),
 * after its bookkeeping, and only on a carrier a CRC-passed downlink block proved before the SYSINFO's own block, which
 * tetra.c notes only after the MAC acted on it. */
static void
sm_stage_move(const dsd_state* state, tetra_state_t* ts, long hz) {
    if (!tetra_confirm_downlink_proven(state)) {
        return;
    }
    ts->sm.acq.staged_hz = hz;
    ts->sm.acq.staged_epoch = tetra_confirm_carrier_epoch(state);
}

/* A SYSINFO heard off main carrier @p hz: the carrier is no control channel, so ON_CC, which a tune elsewhere rests in,
 * is not claimed here, and a move that landed is over, its carrier heard (ACQUIRING waits only for a carrier that
 * sends nothing). The move there is staged. */
static void
sm_off_main_carrier(dsd_state* state, tetra_state_t* ts, long hz) {
    if (ts->sm.state == (uint8_t)TETRA_SM_ON_CC || ts->sm.state == (uint8_t)TETRA_SM_ACQUIRING) {
        sm_set_state(state, ts, TETRA_SM_IDLE, "SYSINFO names another main carrier");
    }
    sm_stage_move(state, ts, hz);
}

/* What a staged move needs at the end of its burst: trunking on a downlink, a receiver the state machine can move,
 * nothing followed and no request of its own in flight, the receiver off main carrier @p hz, which is still the control
 * channel the SYSINFO named, and the tuner nobody else's now (the engine's answer). */
static int
sm_move_allowed(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts, long hz) {
    if (opts->trunk_enable != 1 || opts->tetra_uplink != 0 || ts->uplink || !sm_input_tunable(opts)) {
        return 0;
    }
    if (ts->sm.state == (uint8_t)TETRA_SM_TUNED || ts->sm.pending_request_id != 0U) {
        return 0;
    }
    if (state->trunk_cc_freq != hz || sm_on_main_carrier(opts, state, ts, hz)) {
        return 0;
    }
    return dsd_trunk_tuning_hook_tuner_free(opts, state);
}

/* Move to main carrier @p hz: the return to the control channel a release makes, with what it changes kept to put
 * back. */
static void
sm_move(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, long hz, tetra_sm_move* entry) {
    const long from_hz = tetra_sm_receiver_freq(opts, state);
    const tetra_sm_rollback prev = sm_capture(opts, state, ts);
    uint64_t request_id = 0U;
    const dsd_trunk_tune_result result = sm_request_cc_return(opts, state, &request_id);
    if (result == DSD_TRUNK_TUNE_RESULT_DEFERRED) {
        entry->next_m = dsd_decode_now_mono_s() + TETRA_SM_MOVE_HOLD_S;
        if (sm_narrates(ts, 2)) {
            DSD_FPRINTF(stderr, "[TETRA SM] move to the main carrier %ld Hz deferred\n", hz);
        }
        return;
    }
    if (!dsd_trunk_tune_result_is_ok(result)) {
        sm_move_failed(ts, hz, result);
        return;
    }
    /* The engine stamps control-channel sync when it tunes to one (dsd_engine_trunk_tune_to_cc_request()); here only
     * the main carrier's own SYSINFO proves it. */
    state->last_cc_sync_time = prev.tuning.last_cc_sync_time;
    state->last_cc_sync_time_m = prev.tuning.last_cc_sync_time_m;
    if (sm_narrates(ts, 1)) {
        if (from_hz > 0) {
            DSD_FPRINTF(stderr, "[TETRA SM] on a secondary carrier (%ld Hz); moving to the main carrier %ld Hz\n",
                        from_hz, hz);
        } else {
            DSD_FPRINTF(stderr, "[TETRA SM] moving to the main carrier %ld Hz\n", hz);
        }
    }
    ts->sm.acq.target_hz = hz;
    ts->sm.acq.landed_m = 0.0;
    ts->sm.acq.silent_noted = 0;
    ts->sm.acq.stay_noted = 0;
    sm_set_state(state, ts, TETRA_SM_ACQUIRING, "moving to the main carrier");
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        sm_stage_pending(opts, state, ts, SM_PENDING_ACQUIRE, request_id, &prev);
        return;
    }
    sm_move_completed(opts, state, ts, 0.0);
}

/* A SYSINFO here names main carrier @p hz, which was moved to once already (a cell whose carriers name each other):
 * the receiver stays where it is. Said once per move made. */
static void
sm_note_stay(tetra_state_t* ts, long hz) {
    if (ts->sm.acq.stay_noted) {
        return;
    }
    ts->sm.acq.stay_noted = 1;
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] main carrier %ld Hz was moved to once already; staying on this carrier\n", hz);
    }
}

void
tetra_sm_after_burst(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!opts || !ts || ts->sm.acq.staged_hz == 0) {
        return;
    }
    sm_note_trunking(opts, ts);
    const long hz = ts->sm.acq.staged_hz;
    ts->sm.acq.staged_hz = 0;
    /* A carrier reset since the SYSINFO (a tune completed in the burst) leaves it naming nothing about this carrier. */
    if (ts->sm.acq.staged_epoch != tetra_confirm_carrier_epoch(state) || !sm_move_allowed(opts, state, ts, hz)) {
        return;
    }
    tetra_sm_move* const entry = sm_move_entry(ts, hz);
    if (entry && entry->consumed) {
        sm_note_stay(ts, hz);
        return;
    }
    if (!entry || dsd_decode_now_mono_s() < entry->next_m) {
        return;
    }
    sm_move(opts, state, ts, hz, entry);
}

void
tetra_sm_forget_moves(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        sm_forget_moves(ts);
    }
}

/* Trunking on, a control channel known, a frequency to go to, and a CRC-passed downlink block on this carrier before
 * the one that carried the grant. */
static int
sm_grant_followable(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts,
                    const tetra_sm_grant* grant) {
    if (opts->trunk_enable != 1) {
        return 0;
    }
    if (state->trunk_cc_freq <= 0 || grant->freq_hz <= 0) {
        if (sm_narrates(ts, 2)) {
            DSD_FPRINTF(stderr, "[TETRA SM] grant ignored: no control channel or no traffic frequency known\n");
        }
        return 0;
    }
    if (!tetra_confirm_downlink_proven(state)) {
        if (sm_narrates(ts, 2)) {
            DSD_FPRINTF(stderr, "[TETRA SM] grant ignored: no CRC-passed downlink block on this carrier before it\n");
        }
        return 0;
    }
    return 1;
}

void
tetra_sm_init(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    (void)opts;
    if (!ts) {
        return;
    }
    tetra_sm_disown_pending(&ts->sm);
    if (ts->sm.state == (uint8_t)TETRA_SM_TUNED) {
        sm_unfollow(opts, state, ts);
    }
    DSD_MEMSET(&ts->sm, 0, sizeof ts->sm);
    tetra_confirm_forget_followed_call(state);
}

void
tetra_sm_on_cc_sync(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    sm_note_trunking(opts, ts);
    if (!sm_settle_at_entry(opts, state, ts)) {
        return;
    }
    const long main_hz = state->trunk_cc_freq;
    if (opts->trunk_enable != 1 || main_hz <= 0) {
        return;
    }
    if (ts->sm.state != (uint8_t)TETRA_SM_TUNED && !sm_on_main_carrier(opts, state, ts, main_hz)) {
        /* Not the control channel (regression: main stamped its sync and rested ON_CC here, never moving there). */
        sm_off_main_carrier(state, ts, main_hz);
        return;
    }
    (void)dsd_trunk_cc_candidates_add(state, main_hz, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    if (ts->sm.state == (uint8_t)TETRA_SM_TUNED) {
        return;
    }
    dsd_mark_cc_sync(state);
    if (ts->sm.state != (uint8_t)TETRA_SM_ON_CC) {
        sm_set_state(state, ts, TETRA_SM_ON_CC,
                     ts->sm.state == (uint8_t)TETRA_SM_ACQUIRING ? "SYSINFO on the main carrier" : "control channel");
    }
}

int
tetra_sm_on_grant(dsd_opts* opts, dsd_state* state, const tetra_sm_grant* grant) {
    if (!opts || !state || !grant) {
        return 0;
    }
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return 0;
    }
    if (!sm_settle_at_entry(opts, state, ts)) {
        if (sm_narrates(ts, 3)) {
            DSD_FPRINTF(stderr, "[TETRA SM] grant ignored: a tune is in flight\n");
        }
        return 0;
    }
    if (!sm_grant_followable(opts, state, ts, grant)) {
        return 0;
    }
    if (grant->freq_hz == tetra_sm_receiver_freq(opts, state)) {
        return sm_follow_here(opts, state, ts, grant);
    }
    return sm_tune_grant(opts, state, ts, grant);
}

int
tetra_sm_settle(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    tetra_state_t* const ts = tetra_st_existing(state);
    return !ts || sm_settle_pending(opts, state, ts);
}

void
tetra_sm_on_release(dsd_opts* opts, dsd_state* state, uint16_t call_id) {
    if (!opts || !state) {
        return;
    }
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !sm_settle_at_entry(opts, state, ts)) {
        return;
    }
    if (ts->sm.state != (uint8_t)TETRA_SM_TUNED) {
        return;
    }
    if (ts->sm.followed.call_id != (uint16_t)(call_id & 0x3FFFu)) {
        if (sm_narrates(ts, 3)) {
            DSD_FPRINTF(stderr, "[TETRA SM] release of call %u ignored: the followed call is %u\n", (unsigned)call_id,
                        (unsigned)ts->sm.followed.call_id);
        }
        return;
    }
    (void)sm_release(opts, state, ts, "release");
}

void
tetra_sm_note_voice(dsd_state* state, uint8_t tn) {
    if (!state) {
        return;
    }
    /* Speech whose CRC passed is reception activity, whichever timeslot it is on. */
    dsd_mark_vc_sync(state);
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || ts->sm.state != (uint8_t)TETRA_SM_TUNED || ts->sm.pending_request_id != 0U) {
        return;
    }
    /* Only a followed timeslot (bit 3 of the bitmap is timeslot 1, EN 300 392-2 table 21.80) keeps the channel: speech
     * the TDMA clock cannot place (0) may be any timeslot's, another call's included. */
    if ((ts->sm.followed.timeslots & tetra_slot_bit(tn)) == 0U) {
        return;
    }
    ts->sm.t_voice_m = dsd_decode_now_mono_s();
}

void
tetra_sm_note_maintenance(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || ts->sm.state != (uint8_t)TETRA_SM_TUNED || ts->sm.pending_request_id != 0U || ts->sm.t_voice_m > 0.0) {
        return;
    }
    ts->sm.t_maint_m = dsd_decode_now_mono_s();
}

/* Trunking off while TUNED: the receiver stays on the followed channel (sm_release()), and what it decodes there is
 * decoded as on any carrier with trunking off, not as the followed call's: the machine keeps the channel and its timers
 * but lets go of the call (no call record, as tetra_sm_retire_call() leaves it), so the record ages, its signalling
 * publishes it only as the heard call, and the proof the follow gate reads goes. With trunking on again the hangtime or
 * the grant timeout returns from the channel as before. */
static void
sm_hold_untrunked(dsd_state* state, tetra_state_t* ts) {
    if (ts->sm.followed_retired) {
        return;
    }
    ts->sm.followed_retired = 1;
    tetra_confirm_forget_followed_call(state);
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] trunking off: call %u let go, the receiver stays on its channel\n",
                    (unsigned)ts->sm.followed.call_id);
    }
}

/* TUNED: whether the followed call is over, and the move back to the control channel when it is. */
static void
sm_tick_tuned(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    if (opts->trunk_is_tuned != 1) {
        /* Another owner released the shared tuning state (a tuner hand-back, a stale follow cleanup): the receiver is
         * no longer ours to move. */
        sm_unfollow(opts, state, ts);
        sm_set_state(state, ts, sm_rest_state(opts, state), "released elsewhere");
        return;
    }
    if (opts->trunk_enable != 1) {
        sm_hold_untrunked(state, ts);
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    if (ts->sm.t_voice_m > 0.0) {
        /* Hangtime counts from the end of speech: the first speech gap longer than a frame 18 slot. */
        const double hangtime = opts->trunk_hangtime > 0.0F ? (double)opts->trunk_hangtime : 0.0;
        if (now_m - ts->sm.t_voice_m >= TETRA_SM_SPEECH_GAP_S + hangtime) {
            (void)sm_release(opts, state, ts, "hangtime");
        }
        return;
    }
    /* Before the first speech the channel is kept while it is maintained for the call (tetra_sm_note_maintenance()). */
    const double since_m = ts->sm.t_maint_m > ts->sm.t_tune_m ? ts->sm.t_maint_m : ts->sm.t_tune_m;
    if (ts->sm.t_tune_m > 0.0 && now_m - since_m >= TETRA_SM_GRANT_TIMEOUT_S) {
        (void)sm_release(opts, state, ts, "grant timeout");
    }
}

/* ACQUIRING on the main carrier: the cell may not transmit it, or not to this receiver. Said once per move; nothing
 * moves back to the carrier left. */
static void
sm_tick_acquiring(tetra_state_t* ts) {
    const tetra_sm_acquire* const acq = &ts->sm.acq;
    if (acq->landed_m <= 0.0 || acq->silent_noted || dsd_decode_now_mono_s() - acq->landed_m < TETRA_SM_MOVE_SILENT_S) {
        return;
    }
    ts->sm.acq.silent_noted = 1;
    if (sm_narrates(ts, 1)) {
        DSD_FPRINTF(stderr, "[TETRA SM] no SYSINFO on main carrier %ld Hz after %.0f s\n", acq->target_hz,
                    TETRA_SM_MOVE_SILENT_S);
    }
}

static void
sm_tick(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    sm_note_trunking(opts, ts);
    if (!sm_settle_pending(opts, state, ts)) {
        return;
    }
    const int resting = ts->sm.state == (uint8_t)TETRA_SM_ON_CC || ts->sm.state == (uint8_t)TETRA_SM_ACQUIRING;
    if (resting && sm_rest_state(opts, state) != TETRA_SM_ON_CC) {
        sm_set_state(state, ts, TETRA_SM_IDLE, opts->trunk_enable != 1 ? "trunking off" : "no control channel");
        return;
    }
    if (ts->sm.state == (uint8_t)TETRA_SM_ACQUIRING) {
        sm_tick_acquiring(ts);
    } else if (ts->sm.state == (uint8_t)TETRA_SM_TUNED) {
        sm_tick_tuned(opts, state, ts);
    }
}

void
tetra_sm_tick(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    sm_tick(opts, state, ts);
    /* The call records age on the same clock, whatever the state machine did. */
    tetra_calls_tick(opts, state);
}

void
tetra_sm_on_external_cc_return(const dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    tetra_sm_disown_pending(&ts->sm);
    /* The engine ended and committed the canonical calls before it handed over. */
    sm_unfollow(NULL, state, ts);
    tetra_forget_carrier(state);
    sm_set_state(state, ts, sm_rest_state(opts, state), "returned to the control channel");
}

void
tetra_sm_abandon_carrier(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    const int held = ts->sm.state == (uint8_t)TETRA_SM_TUNED || ts->sm.pending_request_id != 0U;
    tetra_sm_disown_pending(&ts->sm);
    if (held && opts) {
        sm_release_shared_tuning(opts, state);
    }
    sm_unfollow(opts, state, ts);
    sm_set_state(state, ts, sm_rest_state(opts, state), "carrier abandoned");
}

tetra_sm_state_e
tetra_sm_get_state(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts ? (tetra_sm_state_e)ts->sm.state : TETRA_SM_IDLE;
}

int
tetra_sm_owns_return(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts
           && (ts->sm.state == (uint8_t)TETRA_SM_TUNED || ts->sm.state == (uint8_t)TETRA_SM_ACQUIRING
               || ts->sm.pending_request_id != 0U);
}

int
tetra_sm_followed(const dsd_state* state, tetra_sm_grant* out) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || ts->sm.state != (uint8_t)TETRA_SM_TUNED) {
        return 0;
    }
    if (out) {
        *out = ts->sm.followed;
    }
    return 1;
}

int
tetra_sm_followed_call_id(const dsd_state* state, uint16_t* call_id) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || ts->sm.state != (uint8_t)TETRA_SM_TUNED || ts->sm.followed_retired) {
        return 0;
    }
    if (call_id) {
        *call_id = ts->sm.followed.call_id;
    }
    return 1;
}

void
tetra_sm_retire_call(dsd_state* state, uint16_t call_id) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    const uint16_t id = (uint16_t)(call_id & 0x3FFFu);
    if (ts->sm.state == (uint8_t)TETRA_SM_TUNED && !ts->sm.followed_retired && ts->sm.followed.call_id == id) {
        ts->sm.followed_retired = 1;
        tetra_confirm_forget_followed_call(state);
        if (sm_narrates(ts, 1)) {
            DSD_FPRINTF(stderr, "[TETRA SM] call %u has no call record any more; following its channel, no call\n",
                        (unsigned)id);
        }
    }
    /* What a failing tune in flight puts back is retired too: the call it would bring back no longer exists. */
    tetra_sm_rollback* const prev = &ts->sm.pending_prev;
    if (ts->sm.pending_request_id != 0U && prev->state == (uint8_t)TETRA_SM_TUNED && prev->followed.call_id == id) {
        prev->followed_retired = 1;
        prev->followed_proven = 0;
        prev->followed_rx = 0;
    }
}

void
tetra_sm_on_call_renamed(dsd_state* state, uint16_t old_id, uint16_t new_id) {
    tetra_state_t* const ts = tetra_st_existing(state);
    const uint16_t from = (uint16_t)(old_id & 0x3FFFu);
    const uint16_t to = (uint16_t)(new_id & 0x3FFFu);
    if (!ts || from == to) {
        return;
    }
    if (ts->sm.state == (uint8_t)TETRA_SM_TUNED && !ts->sm.followed_retired && ts->sm.followed.call_id == from) {
        ts->sm.followed.call_id = to;
        if (sm_narrates(ts, 1)) {
            DSD_FPRINTF(stderr, "[TETRA SM] followed call %u is call %u now\n", (unsigned)from, (unsigned)to);
        }
    }
    tetra_sm_rollback* const prev = &ts->sm.pending_prev;
    if (ts->sm.pending_request_id != 0U && prev->state == (uint8_t)TETRA_SM_TUNED && !prev->followed_retired
        && prev->followed.call_id == from) {
        prev->followed.call_id = to;
    }
}
