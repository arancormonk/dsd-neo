// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * NXDN control-channel hunt (see include/dsd-neo/protocol/nxdn/nxdn_cc_hunt.h).
 *
 * The request lifecycle is the DMR startup hunt's (src/protocol/dmr/dmr_trunk_sm.c): a probe is tracked by its tune
 * request and failed at a deadline, a probe keeps the shared control-channel aliases free of unproven frequencies, and
 * a hunt parks only once the tuning layer is settled -- a probe in flight is resolved first, any other tune in flight
 * is waited for, and only a failure is replaced, by a settle tune to the frequency the tuner names. The hunt never
 * disowns or completes a request.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <dsd-neo/protocol/nxdn/nxdn_cc_hunt.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_confirm.h"
#include "nxdn_internal.h"

typedef struct {
    double armed_m;         // decode-clock time the window opened; 0 while unarmed
    double retry_after_m;   // next probe decision
    double tune_deadline_m; // when an unanswered probe or settle tune is failed
    uint64_t tune_request_id;
    uint64_t rx_generation;
    long probe_freq_hz;          // last probe or settle tune (in flight or completed)
    long rx_freq_hz;             // frequency attributed to the receiver at rx_generation; 0 unknown
    long attempt_hz;             // last frequency a hunt tune was asked for, accepted or not
    int rx_valid;                // rx_freq_hz answers for rx_generation, a failed tuner query (0) included
    long cc_freq_hz;             // the control channel a CAC confirmed last; 0 none
    int cc_confirmed;            // a CAC confirmed cc_freq_hz (Type-C); a post-loss hunt needs it
    uint64_t foreign_request_id; // a tune in flight the hunt does not own, until it resolves
    int hunting;
    int park_pending; // 0 none; 1 park keeping the window, 2 park unarmed, once settled
    int hunt_index;
} nxdn_cc_hunt_ctx;

static void
nxdn_cc_hunt_log(const dsd_opts* opts, const char* tag) {
    if (opts && opts->trunk_enable == 1 && opts->verbose > 0) {
        DSD_FPRINTF(stderr, "\n[NXDN CC] %s\n", tag);
    }
}

static nxdn_cc_hunt_ctx*
nxdn_cc_hunt_peek(const dsd_state* state) {
    return state ? DSD_STATE_EXT_GET_AS(nxdn_cc_hunt_ctx, state, DSD_STATE_EXT_PROTO_NXDN_CC_HUNT) : NULL;
}

static nxdn_cc_hunt_ctx*
nxdn_cc_hunt_get(dsd_state* state) {
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    if (ctx || !state) {
        return ctx;
    }
    ctx = (nxdn_cc_hunt_ctx*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_NXDN_CC_HUNT, ctx, free) != 0) {
        free(ctx);
        return NULL;
    }
    if (state->trunk_recovery_protocol == (int)DSD_TRUNK_RECOVERY_NXDN) {
        /* Only this context names NXDN, so an owner without one is a previous run's on a reused state (the engine's
           cleanup frees the context): what that run proved is gone with it. */
        state->trunk_recovery_protocol = (int)DSD_TRUNK_RECOVERY_UNKNOWN;
    }
    return ctx;
}

/* Trunking that may own the control channel here: on, outside -Y and trunk scan, and off any voice channel. */
static int
nxdn_cc_hunt_on_control_side(const dsd_opts* opts, const dsd_state* state) {
    return opts->trunk_enable == 1 && opts->scanner_mode != 1 && !dsd_trunk_scan_owns_tuner(opts, state)
           && opts->trunk_is_tuned == 0;
}

static long
nxdn_cc_hunt_tuner_frequency(const dsd_opts* opts) {
    if (opts->use_rigctl == 1) {
        return dsd_rigctl_query_hook_get_current_freq_hz(opts);
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return (long)opts->rtlsdr_center_freq;
    }
    return 0;
}

/* The frequency the receiver is on, attributed once per tuning generation: the completed probe, else the tuner. */
static long
nxdn_cc_hunt_attributed_frequency(nxdn_cc_hunt_ctx* ctx, const dsd_opts* opts) {
    const uint64_t generation = dsd_trunk_tuning_generation();
    if (ctx->rx_valid && ctx->rx_generation == generation) {
        return ctx->rx_freq_hz;
    }
    /* A failed query is kept too: rigctl answers synchronously, and a peer that refuses or times out on every query
       must not cost one per CAC. */
    const long freq = nxdn_cc_hunt_tuner_frequency(opts);
    ctx->rx_freq_hz = freq > 0 ? freq : 0;
    ctx->rx_generation = generation;
    ctx->rx_valid = 1;
    return ctx->rx_freq_hz;
}

/* A hunt tune lands on another carrier: what the last one proved must not carry over, or a weak streak or a sticky
   confirmation would let one short CRC on the new frequency pass for a confirmed transmission, a SACCH superframe or
   alias assembly would complete with the new carrier's pieces, and a data call's header would vouch for its payload. The durable
   nxdn_confirm_lock_m stays, and so does the RAN, as at every trunked move within the system's map (issue #575). */
static void
nxdn_cc_hunt_reset_carrier_evidence(dsd_state* state) {
    nxdn_confirm_reset(state);
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    state->nxdn_part_of_frame = 0;
    state->nxdn_sf = 0;
    nxdn_alias_reset(state);           // the multi-frame assemblies the carrier boundary drops too
    nxdn_reset_data_call_state(state); // a data call's header must not vouch for the next carrier's payload
}

/* Tune for the hunt, keeping the shared control-channel aliases: the tuning helper stages the frequency as the control
   channel, which only a CAC may prove. */
static int
nxdn_cc_hunt_tune(nxdn_cc_hunt_ctx* ctx, dsd_opts* opts, dsd_state* state, long freq, double now_m) {
    const long trunk_cc_before = state->trunk_cc_freq;
    const long p25_cc_before = state->p25_cc_freq;
    ctx->attempt_hz = freq;
    uint64_t request_id = 0U;
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, 0, &request_id);
    state->trunk_cc_freq = trunk_cc_before;
    state->p25_cc_freq = p25_cc_before;
    if (!dsd_trunk_tune_result_is_ok(result)) {
        return 0;
    }
    nxdn_cc_hunt_reset_carrier_evidence(state);
    ctx->probe_freq_hz = freq;
    ctx->rx_freq_hz = 0;
    ctx->rx_valid = 0;
    ctx->tune_request_id = request_id;
    ctx->tune_deadline_m = now_m + NXDN_CC_HUNT_TUNE_TIMEOUT_S;
    return 1;
}

/* Resolve the hunt's own request. Returns 0 while it is still in flight; a completed one attributes the receiver to
   the probe and gives it its full dwell from the completion, a failed one (or one the deadline failed) only clears the
   request: the next probe replaces it. */
static int
nxdn_cc_hunt_resolve(nxdn_cc_hunt_ctx* ctx, const dsd_opts* opts, double now_m) {
    if (ctx->tune_request_id == 0U) {
        return 1;
    }
    double completed_m = 0.0;
    dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(ctx->tune_request_id, &completed_m);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        if (now_m < ctx->tune_deadline_m) {
            return 0;
        }
        /* Frames stay gated until a replacement tune succeeds; a late completion cannot resurrect it. */
        dsd_trunk_tuning_request_publish(ctx->tune_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
        // A completion that won the race stands: the request's own status says which.
        result = dsd_trunk_tuning_request_status(ctx->tune_request_id, &completed_m);
        if (result != DSD_TRUNK_TUNE_RESULT_OK) {
            nxdn_cc_hunt_log(opts, "cc-tune-timeout");
        }
    }
    ctx->tune_request_id = 0U;
    if (result == DSD_TRUNK_TUNE_RESULT_OK
        && ctx->probe_freq_hz > 0) { // an operator move superseded a probe it cleared
        ctx->rx_freq_hz = ctx->probe_freq_hz;
        ctx->rx_generation = dsd_trunk_tuning_generation();
        ctx->rx_valid = 1;
        if (ctx->hunting) {
            ctx->retry_after_m = (completed_m > 0.0 ? completed_m : now_m) + NXDN_CC_HUNT_PROBE_S;
        }
    }
    return 1;
}

/* A tune in flight the hunt does not own (a call's return to the control channel, a grant, an operator's tune), or 0. */
static uint64_t
nxdn_cc_hunt_foreign_tune_in_flight(const nxdn_cc_hunt_ctx* ctx) {
    const uint64_t newest = dsd_trunk_tuning_pending_request();
    return (newest != 0U && newest != ctx->tune_request_id
            && dsd_trunk_tuning_request_status(newest, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING)
               ? newest
               : 0U;
}

/* The window restarts while a foreign tune is in flight, and at its completion once it lands: the decoder may not tick
   at all while the stream waits for it. */
static int
nxdn_cc_hunt_follow_foreign_tune(nxdn_cc_hunt_ctx* ctx, double now_m) {
    const uint64_t foreign = nxdn_cc_hunt_foreign_tune_in_flight(ctx);
    if (foreign != 0U) {
        ctx->foreign_request_id = foreign;
        ctx->armed_m = now_m;
        return 1;
    }
    if (ctx->foreign_request_id != 0U) {
        double completed_m = 0.0;
        if (dsd_trunk_tuning_request_status(ctx->foreign_request_id, &completed_m) == DSD_TRUNK_TUNE_RESULT_OK
            && completed_m > ctx->armed_m) {
            ctx->armed_m = completed_m;
        }
        ctx->foreign_request_id = 0U;
    }
    return 0;
}

/* The window runs from the later of its opening and the last NXDN frame whose own CRCs confirmed the transmission:
   validated content means the frequency is not dead, even when it carries no control channel. */
static int
nxdn_cc_hunt_window_expired(const nxdn_cc_hunt_ctx* ctx, const dsd_state* state, double now_m) {
    double start_m = ctx->armed_m;
    if (state->nxdn_confirm_lock_m > start_m) {
        start_m = state->nxdn_confirm_lock_m;
    }
    return (now_m - start_m) > NXDN_CC_HUNT_GRACE_S;
}

/* Whether the session may arm or run a hunt: admitted, eligible, and with an NXDN owner only after a CAC confirmed a
   control channel (a Type-D site never hunts once proven). */
static int
nxdn_cc_hunt_may_run(const nxdn_cc_hunt_ctx* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (!dsd_trunk_nxdn_recovery_allowed(opts, state) || !dsd_state_trunk_nxdn_hunt_eligible(opts, state)) {
        return 0;
    }
    /* No context under an NXDN owner is a reused state's next run, whose first tick forgets that owner. */
    return state->trunk_recovery_protocol != (int)DSD_TRUNK_RECOVERY_NXDN || !ctx || ctx->cc_confirmed;
}

static void
nxdn_cc_hunt_park_now(nxdn_cc_hunt_ctx* ctx, const dsd_opts* opts, int keep_window, const char* reason) {
    if (!keep_window) {
        ctx->armed_m = 0.0;
    }
    ctx->hunting = 0;
    ctx->park_pending = 0;
    ctx->tune_request_id = 0U;
    ctx->probe_freq_hz = 0;
    ctx->attempt_hz = 0;
    ctx->rx_freq_hz = 0;
    ctx->rx_valid = 0;
    ctx->retry_after_m = 0.0;
    nxdn_cc_hunt_log(opts, reason);
}

/* Park once settled (see the file comment). A settle tune is tracked like a probe, its deadline included. */
static void
nxdn_cc_hunt_settle(nxdn_cc_hunt_ctx* ctx, dsd_opts* opts, dsd_state* state, double now_m, int keep_window,
                    const char* reason) {
    ctx->park_pending = keep_window ? 1 : 2;
    if (!nxdn_cc_hunt_resolve(ctx, opts, now_m)) {
        return;
    }
    const uint64_t newest = dsd_trunk_tuning_pending_request();
    if (newest != 0U) {
        if (dsd_trunk_tuning_request_status(newest, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING) {
            return;
        }
        const long freq = nxdn_cc_hunt_tuner_frequency(opts);
        if (freq <= 0 || !nxdn_cc_hunt_tune(ctx, opts, state, freq, now_m)) {
            nxdn_cc_hunt_log(opts, "cc-settle-deferred-or-failed");
            return;
        }
        nxdn_cc_hunt_log(opts, "cc-settle-tune");
        (void)nxdn_cc_hunt_resolve(ctx, opts, now_m);
        return;
    }
    nxdn_cc_hunt_park_now(ctx, opts, keep_window, reason);
}

/* Whether a running hunt goes on probing; otherwise it is parked, or settling toward a park. At a probe decision
   (@p decision) an unexpired window -- validated content on the probe -- parks it too. */
static int
nxdn_cc_hunt_gate(nxdn_cc_hunt_ctx* ctx, dsd_opts* opts, dsd_state* state, double now_m, int decision) {
    if (opts->trunk_enable != 1 || state->input_boundary != 0) {
        // Trunking off retires failures in the engine, and an input replacement resets every request: nothing is left
        // for the hunt to settle.
        nxdn_cc_hunt_park_now(ctx, opts, 0, "cc-hunt-trunking-or-input-left");
        return 0;
    }
    if (!nxdn_cc_hunt_may_run(ctx, opts, state)) {
        nxdn_cc_hunt_settle(ctx, opts, state, now_m, 0, "cc-hunt-not-admitted");
        return 0;
    }
    if (ctx->park_pending != 0) {
        nxdn_cc_hunt_settle(ctx, opts, state, now_m, ctx->park_pending == 1, "cc-hunt-operator-move");
        return 0;
    }
    if (decision && !nxdn_cc_hunt_window_expired(ctx, state, now_m)) {
        nxdn_cc_hunt_settle(ctx, opts, state, now_m, 1, "cc-hunt-window");
        return 0;
    }
    return 1;
}

static int
nxdn_cc_hunt_map_has_frequency(const dsd_state* state, long freq) {
    for (int i = 0; i < state->lcn_freq_count; ++i) {
        if (*dsd_state_trunk_lcn_slot_const(state, i) == freq && !dsd_state_trunk_lcn_avoid_get(state, (size_t)i)) {
            return 1;
        }
    }
    return 0;
}

/* The channel map in file order, skipping zero, avoided and adjacent duplicate rows. With no other row, a probe whose
   tune never completed is probed again (only a success retires the failure that gates every frame), and one the radio
   is on keeps listening. */
static long
nxdn_cc_hunt_next_mapped(nxdn_cc_hunt_ctx* ctx, const dsd_state* state) {
    int probe_listed = 0;
    for (int tries = 0; tries < state->lcn_freq_count; ++tries) {
        if (ctx->hunt_index >= state->lcn_freq_count || ctx->hunt_index < 0) {
            ctx->hunt_index = 0;
        }
        const int index = ctx->hunt_index++;
        const long freq = *dsd_state_trunk_lcn_slot_const(state, index);
        if (freq <= 0 || dsd_state_trunk_lcn_avoid_get(state, (size_t)index)) {
            continue;
        }
        if (freq == ctx->probe_freq_hz) {
            probe_listed = 1;
            continue;
        }
        while (ctx->hunt_index < state->lcn_freq_count
               && *dsd_state_trunk_lcn_slot_const(state, ctx->hunt_index) == freq) {
            ++ctx->hunt_index;
        }
        return freq;
    }
    return probe_listed ? ctx->probe_freq_hz : 0;
}

/* After a control channel a CAC confirmed, it is revisited between alternate rows, so one that recovers does not
   wait for the whole map. The last attempt decides, accepted or not, so an anchor the tuner refuses does not hold the
   hunt on itself. */
static long
nxdn_cc_hunt_next_candidate(nxdn_cc_hunt_ctx* ctx, const dsd_state* state) {
    const long anchor = ctx->cc_confirmed ? ctx->cc_freq_hz : 0;
    if (anchor > 0 && ctx->attempt_hz != anchor && nxdn_cc_hunt_map_has_frequency(state, anchor)) {
        return anchor;
    }
    return nxdn_cc_hunt_next_mapped(ctx, state);
}

static void
nxdn_cc_hunt_probe(nxdn_cc_hunt_ctx* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (ctx->retry_after_m > now_m || !nxdn_cc_hunt_gate(ctx, opts, state, now_m, 1)) {
        return;
    }
    ctx->retry_after_m = now_m + NXDN_CC_HUNT_PROBE_S;
    const long freq = nxdn_cc_hunt_next_candidate(ctx, state);
    if (freq <= 0) {
        return;
    }
    if (freq == ctx->rx_freq_hz && freq == ctx->probe_freq_hz
        && dsd_trunk_tuning_frame_is_current(ctx->rx_generation)) {
        ctx->attempt_hz = freq; // a visit all the same: the next decision moves on
        return;                 // the radio is on it: keep listening without resetting demodulation
    }
    if (!nxdn_cc_hunt_tune(ctx, opts, state, freq, now_m)) {
        nxdn_cc_hunt_log(opts, "cc-probe-deferred-or-failed");
        return;
    }
    if (opts->verbose > 0) {
        DSD_FPRINTF(stderr, "\n[NXDN CC] probe %.6lf MHz; acquisition %.2f s\n", (double)freq / 1000000.0,
                    NXDN_CC_HUNT_PROBE_S);
    }
    (void)nxdn_cc_hunt_resolve(ctx, opts, now_m);
}

void
nxdn_cc_hunt_tick_locked(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    if (!ctx) {
        if (!dsd_trunk_nxdn_recovery_allowed(opts, state)) {
            return;
        }
        ctx = nxdn_cc_hunt_get(state);
        if (!ctx) {
            return;
        }
    }
    const double now_m = dsd_decode_now_mono_s();
    if (ctx->hunting) {
        if (!nxdn_cc_hunt_gate(ctx, opts, state, now_m, 0) || !nxdn_cc_hunt_resolve(ctx, opts, now_m)) {
            return;
        }
        nxdn_cc_hunt_probe(ctx, opts, state, now_m);
        return;
    }
    if (!nxdn_cc_hunt_may_run(ctx, opts, state)) {
        ctx->armed_m = 0.0;
        return;
    }
    if (ctx->armed_m <= 0.0) {
        ctx->armed_m = now_m;
        (void)nxdn_cc_hunt_follow_foreign_tune(ctx, now_m); // a return already in flight when the window re-arms
        return;
    }
    if (nxdn_cc_hunt_follow_foreign_tune(ctx, now_m)) {
        return; // a return to the control channel restarts the window once it completes
    }
    if (!nxdn_cc_hunt_window_expired(ctx, state, now_m)) {
        return;
    }
    ctx->hunting = 1;
    ctx->retry_after_m = 0.0;
    nxdn_cc_hunt_log(opts, ctx->cc_confirmed ? "cc-lost-hunt" : "cold-start-hunt");
    nxdn_cc_hunt_probe(ctx, opts, state, now_m);
}

void
nxdn_cc_hunt_note_control(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || !nxdn_cc_hunt_on_control_side(opts, state)) {
        return;
    }
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_get(state);
    if (!ctx) {
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    if (!nxdn_cc_hunt_resolve(ctx, opts, now_m)) {
        return; // a probe whose tune is unresolved is not where this CAC was heard
    }
    const long freq = nxdn_cc_hunt_attributed_frequency(ctx, opts);
    const int was_hunting = ctx->hunting;
    ctx->hunting = 0;
    ctx->park_pending = 0;
    ctx->retry_after_m = 0.0;
    ctx->armed_m = now_m;
    ctx->cc_confirmed = 1; // with no frequency the map is still hunted, without an anchor
    if (freq > 0) {
        ctx->probe_freq_hz = freq; // the receiver is there: a hunt after its loss starts with the next row
        ctx->attempt_hz = freq;
        ctx->cc_freq_hz = freq;
        state->p25_cc_freq = freq;
        state->trunk_cc_freq = freq;
    }
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = now_m;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_NXDN);
    if (was_hunting) {
        nxdn_cc_hunt_log(opts, "decoded-cc");
    }
}

void
nxdn_cc_hunt_note_type_d(const dsd_opts* opts, dsd_state* state) {
    /* On a voice channel a busy-repeater message sent the receiver to as well: a Type-D site needs no control channel.
       The SCCH's own run of short CRCs must confirm it: a confirmation left from a CAC or another carrier must not
       turn a Type-C session's recovery off on one noise frame. */
    if (!opts || !state || opts->trunk_enable != 1 || opts->scanner_mode == 1 || dsd_trunk_scan_owns_tuner(opts, state)
        || !nxdn_confirm_scch_confirmed(state)) {
        return;
    }
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_get(state);
    if (!ctx || !nxdn_cc_hunt_resolve(ctx, opts, dsd_decode_now_mono_s())) {
        return;
    }
    const int was_hunting = ctx->hunting;
    ctx->hunting = 0;
    ctx->park_pending = 0;
    ctx->retry_after_m = 0.0;
    ctx->armed_m = 0.0;
    ctx->cc_confirmed = 0;
    ctx->cc_freq_hz = 0;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_NXDN);
    if (was_hunting) {
        nxdn_cc_hunt_log(opts, "type-d-site");
    }
}

void
nxdn_cc_hunt_note_retune(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (!state) {
        return;
    }
    nxdn_cc_hunt_reset_carrier_evidence(state); // a trunked channel cycle crosses no carrier boundary
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    if (!ctx) {
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    ctx->rx_freq_hz = 0;
    ctx->rx_valid = 0;
    if (ctx->hunting) {
        ctx->armed_m = now_m;
        ctx->park_pending = 1;
        ctx->probe_freq_hz = 0;
    } else if (ctx->armed_m > 0.0) {
        ctx->armed_m = now_m;
    }
}

void
nxdn_cc_hunt_note_tuner_frequency(const dsd_state* state, long freq_hz) {
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    if (ctx && freq_hz > 0) {
        ctx->rx_freq_hz = freq_hz;
        ctx->rx_generation = dsd_trunk_tuning_generation();
        ctx->rx_valid = 1;
    }
}

long
nxdn_cc_hunt_receiver_frequency(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_get(state);
    return ctx ? nxdn_cc_hunt_attributed_frequency(ctx, opts) : nxdn_cc_hunt_tuner_frequency(opts);
}

void
nxdn_cc_hunt_forget_attribution(const dsd_state* state) {
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    if (ctx) {
        ctx->rx_freq_hz = 0;
        ctx->rx_valid = 0;
    }
}

void
nxdn_cc_hunt_note_system_replaced(dsd_state* state) {
    nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    if (ctx) {
        ctx->cc_confirmed = 0;
        ctx->cc_freq_hz = 0;
        ctx->hunt_index = 0;
    }
    if (state && state->trunk_recovery_protocol == (int)DSD_TRUNK_RECOVERY_NXDN) {
        // What the old site proved (a Type-C control channel, a Type-D site) says nothing about the new one.
        state->trunk_recovery_protocol = (int)DSD_TRUNK_RECOVERY_UNKNOWN;
    }
}

int
nxdn_cc_hunt_active(const dsd_state* state) {
    const nxdn_cc_hunt_ctx* ctx = nxdn_cc_hunt_peek(state);
    return ctx && (ctx->hunting || ctx->armed_m > 0.0);
}

int
nxdn_cc_hunt_due(const dsd_opts* opts, const dsd_state* state) {
    if (nxdn_cc_hunt_active(state)) {
        return 1;
    }
    return opts && state && nxdn_cc_hunt_may_run(nxdn_cc_hunt_peek(state), opts, state);
}
