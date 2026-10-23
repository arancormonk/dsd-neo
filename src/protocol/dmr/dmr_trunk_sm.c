// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * DMR Tier III trunking state machine - event-driven, tick-based.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);

#define DMR_CC_PROBE_GRACE_S  2.0
#define DMR_CC_TUNE_TIMEOUT_S 5.0
/* How recent a slot's last accepted voice must be for a voice sync to hold a followed call: about three superframes. */
#define DMR_VOICE_EVIDENCE_S  1.0
/* How long a failed rigctl frequency read is not repeated (real time: the read itself is what blocks). */
#define DMR_RX_QUERY_RETRY_S  2.0
/* How long after its last accepted voice a slot's voice still counts as live: DMR voice frames are ~60 ms apart. */
#define DMR_VOICE_STALE_S     0.2

/* ============================================================================
 * Per-slot call identity
 * ============================================================================ */

static void
dmr_sm_slot_identity_clear(dmr_sm_slot_ctx_t* slot) {
    slot->target = 0U;
    slot->source = 0U;
    slot->kind = (uint8_t)DSD_CALL_KIND_UNKNOWN;
    slot->prov = (uint8_t)DMR_SM_ID_NONE;
    slot->judged_group = 0U;
    slot->data_call = 0U;
    slot->channel = 0;
    slot->carrier_hz = 0;
    slot->follow_m = 0.0;
}

static void
dmr_sm_slots_reset(dmr_sm_ctx_t* ctx) {
    for (int s = 0; s < 2; s++) {
        ctx->slots[s].voice_active = 0;
        ctx->slots[s].last_active_m = 0.0;
        dmr_sm_slot_identity_clear(&ctx->slots[s]);
    }
}

static int
dmr_sm_slot_identity_known(const dmr_sm_slot_ctx_t* slot) {
    return slot->kind != (uint8_t)DSD_CALL_KIND_UNKNOWN && slot->prov != (uint8_t)DMR_SM_ID_NONE;
}

/* Name the call on @p slot. Its follow time stays while the same call (policy class, target, carrier) is named again,
   whatever names it, so dwell runs from the conversation's first follow; a different call starts at @p follow_m. */
static void
dmr_sm_slot_identity_set(dmr_sm_slot_ctx_t* slot, dsd_call_kind kind, uint32_t target, uint32_t source,
                         int judged_group, int data_call, dmr_sm_identity_prov_e prov, int channel, long carrier_hz,
                         double follow_m) {
    const int same_call = dmr_sm_slot_identity_known(slot) && slot->target == target
                          && slot->judged_group == (uint8_t)(judged_group != 0) && slot->carrier_hz == carrier_hz;
    if (!same_call) {
        slot->follow_m = follow_m;
    }
    slot->target = target;
    slot->source = source;
    slot->kind = (uint8_t)kind;
    slot->prov = (uint8_t)prov;
    slot->judged_group = (uint8_t)(judged_group != 0);
    slot->data_call = (uint8_t)(data_call != 0);
    slot->channel = channel;
    slot->carrier_hz = carrier_hz;
}

/* The policy's verdict on a slot's call, judged as it was judged when followed. */
static int
dmr_sm_slot_identity_decision(const dsd_opts* opts, const dsd_state* state, const dmr_sm_slot_ctx_t* slot,
                              dsd_tg_policy_decision* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    const int rc =
        slot->judged_group
            ? dsd_tg_policy_evaluate_group_call(opts, state, slot->target, slot->source, 0, slot->data_call, out)
            : dsd_tg_policy_evaluate_private_call(opts, state, slot->source, slot->target, 0, slot->data_call, out);
    return rc == 0;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static void
sm_log(const dsd_opts* opts, const char* tag) {
    if (opts && opts->trunk_enable == 1 && opts->verbose > 1 && tag) {
        DSD_FPRINTF(stderr, "\n[DMR SM] %s\n", tag);
    }
}

static int
sm_status_log_enabled(const dsd_opts* opts) {
    return opts && opts->trunk_enable == 1 && opts->verbose > 0;
}

static void
sm_recovery_log(const dsd_opts* opts, const char* tag) {
    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n[DMR SM] %s\n", tag);
    }
}

static long
resolve_freq(const dsd_state* state, long freq_hz, int lpcn) {
    if (freq_hz > 0) {
        return freq_hz;
    }
    if (state && lpcn > 0 && lpcn < 0xFFFF) {
        return state->trunk_chan_map[lpcn];
    }
    return 0;
}

static int
lpcn_is_trusted(const dsd_opts* opts, dsd_state* state, int lpcn) {
    if (!state || lpcn <= 0 || lpcn >= 0x1000) {
        return 1;
    }
    uint8_t trust = state->dmr_lcn_trust[lpcn];
    int on_cc = (state->trunk_cc_freq != 0 && opts && opts->trunk_is_tuned == 0);
    if (trust < 2 && !on_cc) {
        if (sm_status_log_enabled(opts)) {
            DSD_FPRINTF(stderr, "\n  DMR SM: block tune LPCN=%d (untrusted off-CC)\n", lpcn);
        }
        return 0;
    }
    return 1;
}

static void
set_state(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dmr_sm_state_e new_state, const char* reason) {
    if (!ctx || ctx->state == new_state) {
        return;
    }
    dmr_sm_state_e old = ctx->state;
    ctx->state = new_state;
    ctx->cc_window_lost = 0; // only tick_on_cc()'s own transition marks it, after this
    if (new_state != DMR_SM_IDLE && !ctx->cold_start_hunt) {
        ctx->cold_start_armed_m = 0.0; // only an IDLE machine keeps a startup window (issue #703)
    }

    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n[DMR SM] %s -> %s (%s)\n", dmr_sm_state_name(old), dmr_sm_state_name(new_state),
                    reason ? reason : "");
    }
}

/* Resolve completion before accepting any heartbeat. A successful backend tune
 * is an acquisition boundary, not decoded control-channel evidence. */
static int
dmr_cc_resolve_pending(dmr_sm_ctx_t* ctx, const dsd_opts* opts, double now_m) {
    if (ctx->cc_tune_request_id == 0U) {
        return 1;
    }
    double completed_m = 0.0;
    dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(ctx->cc_tune_request_id, &completed_m);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        if (now_m < ctx->cc_tune_deadline_m) {
            return 0;
        }
        /* Keep frames gated until a replacement tune succeeds. Late completion
         * cannot resurrect the expired request. */
        dsd_trunk_tuning_request_publish(ctx->cc_tune_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
        result = dsd_trunk_tuning_request_status(ctx->cc_tune_request_id, &completed_m);
        sm_recovery_log(opts, "cc-tune-timeout");
    }
    ctx->cc_tune_request_id = 0U;
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        ctx->cc_acquire_start_m = completed_m > 0.0 ? completed_m : now_m;
        ctx->cc_rx_freq_hz = ctx->cc_probe_freq_hz;
        ctx->cc_rx_generation = dsd_trunk_tuning_generation();
        return 1;
    }
    ctx->cc_acquiring = 0;
    ctx->cc_retry_after_m = now_m + 2.0;
    set_state(ctx, opts, DMR_SM_HUNTING, "cc-tune-failed");
    return 0;
}

void
dmr_sm_begin_cc_acquisition(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, long freq_hz,
                            uint64_t request_id) {
    if (!ctx || !state || freq_hz <= 0) {
        return;
    }
    const int site_without_cc = ctx->site_without_cc; // the same system: what it identified itself as stays
    dmr_sm_init_ctx(ctx, opts, state);
    ctx->site_without_cc = site_without_cc;
    ctx->cc_freq_hz = freq_hz;
    ctx->cc_probe_freq_hz = freq_hz;
    ctx->cc_acquiring = 1;
    ctx->cc_acquire_start_m = dsd_decode_now_mono_s();
    ctx->cc_tune_request_id = request_id;
    ctx->cc_tune_deadline_m = ctx->cc_acquire_start_m + DMR_CC_TUNE_TIMEOUT_S;
    ctx->state = DMR_SM_ON_CC;
    if (request_id == 0U) {
        ctx->cc_rx_freq_hz = freq_hz;
        ctx->cc_rx_generation = dsd_trunk_tuning_generation();
    }
    (void)dmr_cc_resolve_pending(ctx, opts, ctx->cc_acquire_start_m);
}

dsd_trunk_tune_result
dmr_sm_return_to_cc(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, long freq_hz) {
    if (!ctx || !opts || !state || freq_hz <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    const long previous_cc = state->trunk_cc_freq;
    const long previous_p25_cc = state->p25_cc_freq;
    /* The shared return helper prefers the P25 alias. DMR supplies its own
     * destination and must not inherit that alias or its symbol profile. */
    state->p25_cc_freq = 0;
    state->trunk_cc_freq = freq_hz;
    uint64_t request_id = 0U;
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_return_to_cc(opts, state, &request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
        state->p25_cc_freq = previous_p25_cc;
        state->trunk_cc_freq = previous_cc;
        sm_recovery_log(opts, "cc-return-deferred-or-failed");
        return result;
    }
    state->is_con_plus = 0;
    set_state(ctx, opts, DMR_SM_ON_CC, "cc-return");
    dmr_sm_begin_cc_acquisition(ctx, opts, state, freq_hz, request_id);
    return result;
}

/* A rigctl read is a network round trip: one per tuning generation, until an operator move drops it, and a peer that
   stopped answering is asked again only after a while, since each read can block for its timeout. */
static long
dmr_cc_rigctl_frequency(const dsd_opts* opts, dmr_sm_ctx_t* ctx, uint64_t generation, int use_cache) {
    if (!ctx) {
        return dsd_rigctl_query_hook_get_current_freq_hz(opts);
    }
    if (use_cache && ctx->rx_seen_hz > 0 && ctx->rx_seen_generation == generation) {
        return ctx->rx_seen_hz;
    }
    if (ctx->rx_query_retry_m > 0.0 && dsd_realtime_mono_s() < ctx->rx_query_retry_m) {
        return 0;
    }
    const long freq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
    if (freq > 0) {
        ctx->rx_seen_hz = freq;
        ctx->rx_seen_generation = generation;
        ctx->rx_query_retry_m = 0.0;
    } else {
        ctx->rx_query_retry_m = dsd_realtime_mono_s() + DMR_RX_QUERY_RETRY_S;
    }
    return freq;
}

static long
dmr_cc_current_frequency(const dsd_opts* opts, dmr_sm_ctx_t* ctx) {
    const uint64_t generation = dsd_trunk_tuning_generation();
    if (ctx && ctx->cc_rx_freq_hz > 0 && ctx->cc_rx_generation == generation) {
        return ctx->cc_rx_freq_hz;
    }
    if (opts->use_rigctl == 1) {
        return dmr_cc_rigctl_frequency(opts, ctx, generation, 1);
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return (long)opts->rtlsdr_center_freq;
    }
    /* Fixed audio/file inputs have no tuner query. Their seeded/probed channel
     * remains the only available frequency attribution. */
    if (!ctx) {
        return 0;
    }
    return ctx->cc_probe_freq_hz != 0 ? ctx->cc_probe_freq_hz : ctx->cc_freq_hz;
}

/* Where the tuner itself says it is, for a settle tune: never the attribution a tune in question may have left, nor a
   cached reading, but still with the back-off after a failed rigctl read. 0 for an input with no tuner query. */
static long
dmr_cc_tuner_frequency(const dsd_opts* opts, dmr_sm_ctx_t* ctx) {
    if (opts->use_rigctl == 1) {
        return dmr_cc_rigctl_frequency(opts, ctx, dsd_trunk_tuning_generation(), 0);
    }
    return opts->audio_in_type == AUDIO_IN_RTL ? (long)opts->rtlsdr_center_freq : 0;
}

static long
dmr_cc_anchor(const dmr_sm_ctx_t* ctx, const dsd_state* state) {
    return ctx->cc_freq_hz > 0 ? ctx->cc_freq_hz : state->trunk_cc_freq;
}

/* Whether the control-channel window still runs: a probe's two seconds from its completion, or the grace from the last
   control on a channel being listened to, either running from validated DMR content (the colour-code gate) instead
   when that is later. Sync alone, which noise can produce, never holds it. A window never stamped is not running. */
static int
dmr_cc_window_open(const dmr_sm_ctx_t* ctx, const dsd_state* state, double now_m) {
    double since = ctx->cc_acquiring ? ctx->cc_acquire_start_m : ctx->t_cc_sync_m;
    if (since <= 0.0) {
        return 0;
    }
    if (state && state->dmr_confidence_lock_m > since) {
        since = state->dmr_confidence_lock_m;
    }
    return (now_m - since) <= (ctx->cc_acquiring ? DMR_CC_PROBE_GRACE_S : ctx->cc_grace_s);
}

/* ----------------------------------------------------------------------------
 * Startup control-channel hunt (issue #703)
 *
 * A single-system session whose start frequency yields no validated control within cc_grace_s walks its channel map
 * with the ordinary hunt. Decoder thread only, like every DMR tick.
 * ---------------------------------------------------------------------------- */

static void
dmr_sm_cold_start_clear(dmr_sm_ctx_t* ctx) {
    ctx->cold_start_armed_m = 0.0;
    ctx->cold_start_hunt = 0;
    ctx->cold_start_park_pending = 0;
}

/* Whether this machine may run the startup hunt: an eligible session with no owner yet, or one DMR owns whose validated
   content named the protocol without confirming a control channel here (a data preamble, a Capacity Plus status
   naming a rest channel the map lacks, a traffic channel). A Hytera XPT site has no control channel to find. */
static int
dmr_sm_hunt_eligible(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (ctx->site_without_cc) {
        return 0;
    }
    if (dsd_state_trunk_cold_start_eligible(opts, state)) {
        return 1;
    }
    return state->trunk_recovery_protocol == (int)DSD_TRUNK_RECOVERY_DMR && !ctx->cc_confirmed
           && dsd_state_trunk_dmr_hunt_eligible(opts, state);
}

/* The window runs from the later of its opening and the last DMR content that passed its colour-code gate: validated
   traffic means the frequency is not dead, even when it carries no control channel (a Hytera XPT call). */
static int
dmr_sm_cold_start_window_expired(const dmr_sm_ctx_t* ctx, const dsd_state* state, double now_m) {
    double start_m = ctx->cold_start_armed_m;
    if (state->dmr_confidence_lock_m > start_m) {
        start_m = state->dmr_confidence_lock_m;
    }
    return (now_m - start_m) > ctx->cc_grace_s;
}

/* Leave the startup hunt for IDLE, dropping the probe's attribution: only once the tuning layer is settled, or when
   trunking or the input went away, or where validated control named an owner. */
static void
dmr_sm_cold_start_park_now(dmr_sm_ctx_t* ctx, const dsd_opts* opts, int keep_window, const char* reason) {
    const double armed_m = ctx->cold_start_armed_m;
    ctx->cc_tune_request_id = 0U;
    ctx->cc_acquiring = 0;
    ctx->cc_probe_freq_hz = 0;
    ctx->cc_rx_freq_hz = 0;
    ctx->cc_retry_after_m = 0.0;
    dmr_sm_cold_start_clear(ctx);
    if (keep_window) {
        ctx->cold_start_armed_m = armed_m;
    }
    sm_recovery_log(opts, reason);
    set_state(ctx, opts, DMR_SM_IDLE, reason);
}

/* One step toward a settled tuning layer: a probe in flight is resolved here (never replaced by a new probe), any other
   tune in flight (one the machine does not track) is waited for, and only a failure is replaced, by a settle tune to
   the frequency the tuner reports, whose success makes radio and options agree and retires every older failure. A
   settle tune is tracked like a probe, its deadline included: one the backend never answers is failed at the deadline,
   as a probe is, and replaced. The machine never disowns or completes a request. 1 while the layer is not settled yet,
   0 once nothing gates frames. */
static int
dmr_sm_settle_step(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (ctx->cc_tune_request_id != 0U) {
        (void)dmr_cc_resolve_pending(ctx, opts, now_m);
        if (ctx->cc_tune_request_id != 0U) {
            return 1;
        }
    }
    const uint64_t newest = dsd_trunk_tuning_pending_request();
    if (newest != 0U) {
        if (dsd_trunk_tuning_request_status(newest, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING) {
            return 1;
        }
        const long freq = dmr_cc_tuner_frequency(opts, ctx);
        const long alias_before = state->trunk_cc_freq;
        uint64_t request_id = 0U;
        const dsd_trunk_tune_result result = freq > 0
                                                 ? dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, 0, &request_id)
                                                 : DSD_TRUNK_TUNE_RESULT_FAILED;
        state->trunk_cc_freq = alias_before;
        if (dsd_trunk_tune_result_is_ok(result)) {
            ctx->cc_probe_freq_hz = freq; // its attribution goes with the settle
            ctx->cc_tune_request_id = request_id;
            ctx->cc_tune_deadline_m = now_m + DMR_CC_TUNE_TIMEOUT_S;
        }
        sm_recovery_log(opts, "settle-tune");
        return 1;
    }
    return 0;
}

/* A startup hunt parks once settled (dmr_sm_settle_step()). */
static void
dmr_sm_cold_start_settle(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, int keep_window,
                         const char* reason) {
    ctx->cold_start_park_pending = keep_window ? 1 : 2;
    if (dmr_sm_settle_step(ctx, opts, state, now_m)) {
        return;
    }
    dmr_sm_cold_start_park_now(ctx, opts, keep_window, reason);
}

/* After an operator move: the tuning layer settles first, a failure the move left being replaced by a settle tune to
   where the receiver now is, since a manual tune is not correlated and its success retires nothing. The new
   frequency's listening window runs from the move, or from the settle's completion when frames stayed gated past it.
   1 while unsettled: nothing else may run. */
static int
dmr_sm_settle_after_move(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx->cc_tune_superseded) {
        return 0;
    }
    if (dmr_sm_settle_step(ctx, opts, state, now_m)) {
        ctx->cc_settle_waited = 1;
        return 1;
    }
    // Frames flowed from the move, or only from now: the tune they waited for may be one no request here tracks.
    ctx->t_cc_sync_m = ctx->cc_settle_waited ? now_m : ctx->cc_moved_m;
    ctx->cc_tune_superseded = 0;
    ctx->cc_settle_waited = 0;
    ctx->cc_acquiring = 0;
    ctx->cc_retry_after_m = 0.0;
    set_state(ctx, opts, DMR_SM_ON_CC, "operator-move-settled");
    return 0;
}

/* Whether a startup hunt goes on probing; otherwise it is ended, parked, or settling toward a park. At a probe
   decision point (@p decision) an unexpired window parks it too. */
static int
dmr_sm_cold_start_gate(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, int decision) {
    const int owner_named = state->trunk_recovery_protocol != (int)DSD_TRUNK_RECOVERY_UNKNOWN
                            && (state->trunk_recovery_protocol != (int)DSD_TRUNK_RECOVERY_DMR || ctx->site_without_cc);
    if (owner_named) {
        // Another protocol's validated control named the owner, or a Hytera XPT site identified itself: the hunt ends
        // where it is, as an XPT site is followed. DMR content that named DMR without confirming a control channel
        // here (a confirmation clears the hunt in dmr_cc_note_activity_ctx()) does not end it. A grant it followed
        // stands: only the startup bookkeeping ends.
        if (ctx->state == DMR_SM_TUNED) {
            dmr_sm_cold_start_clear(ctx);
            sm_recovery_log(opts, "cold-start-owner-named");
        } else {
            dmr_sm_cold_start_park_now(ctx, opts, 0, "cold-start-owner-named");
        }
        return 0;
    }
    if (opts->trunk_enable != 1 || state->input_boundary != 0) {
        // The tuning layer retires failures once no tune owner is active, and the engine resets its requests when the
        // input is replaced: nothing is left for this hunt to settle.
        dmr_sm_cold_start_park_now(ctx, opts, 0, "cold-start-trunking-or-input-left");
        return 0;
    }
    if ((ctx->state == DMR_SM_TUNED || opts->trunk_is_tuned == 1) && !dmr_sm_hunt_eligible(ctx, opts, state)) {
        // A call the hunt followed (a grant, a Capacity Plus status) once DMR content named DMR stands: only the
        // startup bookkeeping ends, and the call's own deadlines return it.
        dmr_sm_cold_start_clear(ctx);
        sm_recovery_log(opts, "cold-start-call-followed");
        return 0;
    }
    if (!dmr_sm_hunt_eligible(ctx, opts, state) || !dsd_trunk_dmr_recovery_allowed(opts, state)) {
        dmr_sm_cold_start_settle(ctx, opts, state, now_m, 0, "cold-start-not-admitted");
        return 0;
    }
    if (ctx->cold_start_park_pending != 0) {
        dmr_sm_cold_start_settle(ctx, opts, state, now_m, ctx->cold_start_park_pending == 1,
                                 "cold-start-operator-move");
        return 0;
    }
    if (decision && !dmr_sm_cold_start_window_expired(ctx, state, now_m)) {
        dmr_sm_cold_start_settle(ctx, opts, state, now_m, 1, "cold-start-window");
        return 0;
    }
    return 1;
}

static dmr_sm_ctx_t*
dmr_cc_activity_context(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1 || opts->trunk_is_tuned == 1) {
        return NULL;
    }
    if (opts->trunk_scan_enabled && !dsd_trunk_scan_hook_dmr_ctx()) {
        return NULL;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    if (!dmr_cc_resolve_pending(ctx, opts, dsd_decode_now_mono_s())) {
        return NULL;
    }
    return ctx;
}

static void
dmr_cc_note_monitor_frequency(const dsd_opts* opts, dsd_state* state, long freq_hz) {
    const long current = dmr_cc_current_frequency(opts, NULL);
    if (current > 0 && (freq_hz <= 0 || freq_hz == current)) {
        state->trunk_cc_freq = current;
    }
}

static void
dmr_cc_note_activity_ctx(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, long freq_hz) {
    const long current = dmr_cc_current_frequency(opts, ctx);
    if (current <= 0 || (freq_hz > 0 && freq_hz != current)
        || (ctx->cc_acquiring && current != ctx->cc_probe_freq_hz)) {
        return;
    }
    ctx->cc_freq_hz = current;
    ctx->cc_probe_freq_hz = current;
    ctx->cc_rx_freq_hz = current;
    ctx->cc_rx_generation = dsd_trunk_tuning_generation();
    const int acquired = !ctx->cc_confirmed || ctx->cc_acquiring;
    dmr_sm_cold_start_clear(ctx); // validated control on this frequency ends any startup hunt (issue #703)
    ctx->cc_confirmed = 1;
    ctx->cc_acquiring = 0;
    ctx->cc_retry_after_m = 0.0;
    ctx->t_cc_sync_m = dsd_decode_now_mono_s();
    state->trunk_cc_freq = current;
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = ctx->t_cc_sync_m;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_DMR);
    if (acquired && ctx->state == DMR_SM_ON_CC) {
        sm_recovery_log(opts, "decoded-cc");
    }
    set_state(ctx, opts, DMR_SM_ON_CC, "decoded-cc");
}

void
dmr_sm_note_cc_activity(const dsd_opts* opts, dsd_state* state, long freq_hz) {
    if (opts && state && opts->trunk_enable != 1 && opts->trunk_is_tuned == 0) {
        /* Preserve decoded CC attribution in monitor mode without starting a
         * trunk state machine or giving it permission to retune. */
        dmr_cc_note_monitor_frequency(opts, state, freq_hz);
        return;
    }
    dmr_sm_ctx_t* ctx = dmr_cc_activity_context(opts, state);
    if (!ctx) {
        return;
    }
    dmr_cc_note_activity_ctx(ctx, opts, state, freq_hz);
}

static void
dmr_cc_note_heartbeat_ctx(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (!ctx || !opts || !state || opts->trunk_is_tuned == 1 || !ctx->cc_confirmed || ctx->cc_acquiring
        || !dmr_cc_resolve_pending(ctx, opts, dsd_decode_now_mono_s())) {
        return;
    }
    /* A heartbeat is in the decode hot path. Attribute it to the completed
     * tuning boundary, without a blocking rigctl round trip per CSBK. */
    if (ctx->cc_rx_freq_hz == ctx->cc_freq_hz && dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation)) {
        ctx->t_cc_sync_m = dsd_decode_now_mono_s();
        set_state(ctx, opts, DMR_SM_ON_CC, "cc-heartbeat");
    }
}

void
dmr_sm_note_cc_heartbeat(const dsd_opts* opts, const dsd_state* state) {
    dmr_sm_ctx_t* ctx = dmr_cc_activity_context(opts, state);
    dmr_cc_note_heartbeat_ctx(ctx, opts, state);
}

static int
dmr_cc_map_has_frequency(const dsd_state* state, long freq) {
    for (int i = 0; i < state->lcn_freq_count; ++i) {
        if (*dsd_state_trunk_lcn_slot_const(state, i) == freq && !dsd_state_trunk_lcn_avoid_get(state, (size_t)i)) {
            return 1;
        }
    }
    return 0;
}

static long
dmr_cc_next_mapped_candidate(dmr_sm_ctx_t* ctx, const dsd_state* state) {
    int probe_listed = 0;
    for (int tries = 0; tries < state->lcn_freq_count; ++tries) {
        if (ctx->cc_hunt_index >= state->lcn_freq_count || ctx->cc_hunt_index < 0) {
            ctx->cc_hunt_index = 0;
        }
        const int index = ctx->cc_hunt_index++;
        const long freq = *dsd_state_trunk_lcn_slot_const(state, index);
        if (freq <= 0 || dsd_state_trunk_lcn_avoid_get(state, (size_t)index)) {
            continue;
        }
        if (freq == ctx->cc_probe_freq_hz) {
            probe_listed = 1;
            continue;
        }
        /* Adjacent LSNs often name the same RF channel. */
        while (ctx->cc_hunt_index < state->lcn_freq_count
               && *dsd_state_trunk_lcn_slot_const(state, ctx->cc_hunt_index) == freq) {
            ++ctx->cc_hunt_index;
        }
        return freq;
    }
    /* No eligible alternative: keep listening without a retune on a row the radio is on. One whose tune never completed
       (a failed or expired probe) is probed again, since only a success retires the failure that gates every frame. */
    if (probe_listed
        && (ctx->cc_rx_freq_hz != ctx->cc_probe_freq_hz || !dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation))) {
        return ctx->cc_probe_freq_hz;
    }
    return 0;
}

static long
dmr_cc_next_candidate(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    /* A startup hunt has no confirmed control channel to revisit: the shared alias a DMR message on an earlier probe
       staged (dmr_cspdu_init_cc_anchor()) names a frequency that carried no control. */
    const long anchor = ctx->cc_freq_hz > 0 ? ctx->cc_freq_hz : (ctx->cold_start_hunt ? 0 : state->trunk_cc_freq);
    if (ctx->site_without_cc) {
        return anchor; // XPT: only the frequency it parks on, retried while a failed return gates frames
    }
    const char* scan_map = dsd_trunk_scan_hook_active_chan_csv(state);
    const int user_list = opts->chan_in_file[0] != '\0' || (scan_map && scan_map[0] != '\0');
    if (user_list && state->lcn_freq_count > 0) {
        /* Revisit the anchor between alternate probes so a recovered marginal
         * CC does not wait for the whole map. Respect session avoids here too. */
        if (ctx->cc_probe_freq_hz != anchor && anchor > 0 && dmr_cc_map_has_frequency(state, anchor)) {
            return anchor;
        }
        return dmr_cc_next_mapped_candidate(ctx, state);
    }
    if (ctx->cc_probe_freq_hz != anchor && anchor > 0) {
        return anchor;
    }
    long candidate = 0;
    if (dsd_trunk_cc_candidates_next(state, dsd_decode_now_mono_s(), DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, &candidate)) {
        return candidate;
    }
    return anchor;
}

static void
dmr_cc_hunt(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!dsd_trunk_dmr_recovery_allowed(opts, state) || opts->trunk_is_tuned == 1 || ctx->cc_retry_after_m > now_m) {
        return;
    }
    if (ctx->cold_start_hunt && !dmr_sm_cold_start_gate(ctx, opts, state, now_m, 1)) {
        return;
    }
    if (ctx->cc_window_lost && dmr_cc_window_open(ctx, state, now_m)) {
        // Validated content arrived after the window ran out: the window it holds is open again, and the frequency is
        // not left mid-transmission.
        set_state(ctx, opts, DMR_SM_ON_CC, "content-hold");
        return;
    }
    ctx->cc_window_lost = 0;
    ctx->cc_retry_after_m = now_m + 2.0;
    const long freq = dmr_cc_next_candidate(ctx, opts, state);
    if (freq <= 0) {
        return;
    }
    if (freq == ctx->cc_rx_freq_hz && dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation)) {
        /* No alternate candidate: keep listening without resetting demodulation
         * every acquisition window. Only decoded control evidence confirms it. */
        ctx->cc_probe_freq_hz = freq;
        ctx->cc_acquiring = 1;
        ctx->cc_acquire_start_m = now_m;
        set_state(ctx, opts, DMR_SM_ON_CC, "cc-listen");
        return;
    }
    const long anchor = state->trunk_cc_freq;
    uint64_t request_id = 0U;
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, 0, &request_id);
    /* The shared tune helper stages a CC frequency for legacy callers. DMR
     * probes retain their return destination until a control message proves it. */
    state->trunk_cc_freq = anchor;
    if (!dsd_trunk_tune_result_is_ok(result)) {
        sm_recovery_log(opts, "cc-probe-deferred-or-failed");
        return;
    }
    ctx->cc_probe_freq_hz = freq;
    ctx->cc_acquiring = 1;
    ctx->cc_acquire_start_m = dsd_decode_now_mono_s();
    ctx->cc_tune_request_id = request_id;
    ctx->cc_tune_deadline_m = ctx->cc_acquire_start_m + DMR_CC_TUNE_TIMEOUT_S;
    set_state(ctx, opts, DMR_SM_ON_CC, "cc-probe");
    (void)dmr_cc_resolve_pending(ctx, opts, ctx->cc_acquire_start_m);
    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n[DMR SM] CC probe %.6lf MHz; acquisition %.2f s\n", (double)freq / 1000000.0,
                    DMR_CC_PROBE_GRACE_S);
    }
}

static void
dmr_sm_tick_idle_cold_start(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!dmr_sm_hunt_eligible(ctx, opts, state)) {
        ctx->cold_start_armed_m = 0.0;
        return;
    }
    if (ctx->cold_start_armed_m <= 0.0) {
        ctx->cold_start_armed_m = now_m;
        return;
    }
    if (!dmr_sm_cold_start_window_expired(ctx, state, now_m)) {
        return;
    }
    ctx->cold_start_hunt = 1;
    ctx->cc_retry_after_m = 0.0;
    sm_recovery_log(opts, "cold-start-hunt");
    set_state(ctx, opts, DMR_SM_HUNTING, "cold-start");
    dmr_cc_hunt(ctx, opts, state, now_m);
}

/* ============================================================================
 * Release to CC
 * ============================================================================ */

static void
dmr_sm_release_without_cc(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (state) {
        const double ended_m = dsd_decode_now_mono_s();
        for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; ++slot) {
            if (dsd_call_state_end_ex(state, (uint8_t)slot, ended_m, DSD_CALL_END_EXPLICIT) > 0) {
                dsd_event_sync_slot(opts, state, (uint8_t)slot);
            }
        }
        state->is_con_plus = 0;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        dmr_reset_blocks(opts, state);
    }
    ctx->cc_acquiring = 0;
    ctx->cc_confirmed = 0;
    ctx->cc_tune_request_id = 0U;
}

/* The teardown every release shares, once the destination of the tuner is settled. `cc_returned`
 * says whether a control-channel tune is following this release: when none is -- no CC to return
 * to, or a caller that owns the tuner and is moving it itself -- the call rows and DMR block
 * state have to be cleared here, because nothing downstream will do it. */
static void
dmr_sm_clear_released_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int cc_returned, int count_release,
                              dmr_sm_state_e next_state, const char* reason) {
    dmr_sm_slots_reset(ctx);

    ctx->vc_freq_hz = 0;
    ctx->vc_lpcn = 0;
    ctx->vc_tg = 0;
    ctx->vc_dst = 0;
    ctx->vc_src = 0;
    ctx->vc_slot = -1;
    ctx->vc_is_group = 0;
    ctx->vc_identity_published = 0;
    ctx->vc_status_follow = 0;
    ctx->vc_status_grant = 0;
    ctx->vc_identity_known = 0;
    ctx->vc_data_call = 0;
    ctx->vc_crc_flags = 0;
    ctx->t_tune_m = 0.0;
    ctx->t_voice_m = 0.0;
    ctx->receive_check_started_m = 0.0;
    ctx->receive_check_freq_hz = 0;

    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        // The calls the machine held are gone: nothing is left for a later candidate to preempt (talkgroup policy
        // ledger; every caller holds the P25 SM tick guard).
        (void)dsd_tg_policy_clear_active_call(state, -1);
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        state->dmr_mono_slot = 0;
        if (count_release) {
            state->p25_sm_release_count++;
        }
    }

    if (!cc_returned) {
        dmr_sm_release_without_cc(ctx, opts, state);
    }
    set_state(ctx, opts, next_state, reason);
}

/* Where a release returns: the control channel, or for a call a Capacity Plus status followed, the rest channel
   announced last, by a status or a voice LC, as the no-carrier return reads it (no_carrier_select_control_channel()). */
static long
dmr_sm_release_destination(const dmr_sm_ctx_t* ctx, const dsd_state* state) {
    if (!state) {
        return ctx->cc_freq_hz;
    }
    if (ctx->vc_status_follow && state->dmr_rest_channel >= 1 && state->dmr_rest_channel <= 16
        && state->trunk_chan_map[state->dmr_rest_channel] > 0) {
        return state->trunk_chan_map[state->dmr_rest_channel];
    }
    return state->trunk_cc_freq > 0 ? state->trunk_cc_freq : ctx->cc_freq_hz;
}

static void
do_release(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    int had_force_release = 0;
    if (!ctx) {
        return;
    }

    if (state) {
        had_force_release = (state->trunk_sm_force_release != 0) ? 1 : 0;
        state->trunk_sm_force_release = 0;
    }

    sm_log(opts, reason);

    const long cc = dmr_sm_release_destination(ctx, state);
    dsd_trunk_tune_result tune_result = cc > 0 ? dmr_sm_return_to_cc(ctx, opts, state, cc) : DSD_TRUNK_TUNE_RESULT_OK;
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        sm_log(opts, "release-tune-deferred");
        if (state && had_force_release) {
            state->trunk_sm_force_release = 1;
        }
        return;
    }

    dmr_sm_clear_released_carrier(ctx, opts, state, cc > 0, 1, cc > 0 ? DMR_SM_ON_CC : DMR_SM_HUNTING, reason);
}

void
dmr_sm_abandon_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    if (!ctx) {
        return;
    }
    const int had_carrier = ctx->state == DMR_SM_TUNED || (opts && opts->trunk_is_tuned == 1);
    if (state) {
        /* The teardown the latch asks for is happening right here; left armed, the next tick
         * would try its own release, and that one tunes back to a control channel -- against a
         * caller that is already moving the tuner. */
        state->trunk_sm_force_release = 0;
    }
    sm_log(opts, reason);
    /* Where an ordinary release would return survives the teardown (issue #637): a call a Capacity Plus status followed
     * returns to the rest channel announced last, which a voice LC may be the only one to have named, so the caller's
     * fallback park and the target's revisit land there rather than on a rest channel the system has left. */
    if (state && ctx->vc_status_follow) {
        const long destination = dmr_sm_release_destination(ctx, state);
        if (destination > 0) {
            state->p25_cc_freq = 0;
            state->trunk_cc_freq = destination;
        }
    }
    /* ON_CC, not HUNTING: nothing about this release says the control channel was lost, and a
     * revisit expects to find the target resting exactly where an ordinary release leaves it. */
    dmr_sm_clear_released_carrier(ctx, opts, state, 0, had_carrier, DMR_SM_ON_CC, reason ? reason : "abandon-carrier");
}

typedef enum {
    DMR_SM_RESOLVE_REFUSED = 0,
    DMR_SM_RESOLVE_TUNABLE = 1,
    DMR_SM_RESOLVE_SAME_FREQ = 2, // the machine already follows that carrier
} dmr_sm_resolve_e;

static dmr_sm_resolve_e
dmr_sm_resolve_tunable_grant(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev,
                             long* out_freq) {
    if (opts->trunk_enable != 1 || state->trunk_cc_freq == 0 || ctx->cc_tune_request_id != 0U) {
        return DMR_SM_RESOLVE_REFUSED;
    }

    if (!dsd_receive_grant_allows(opts, DSD_SYNC_DMR_BS_VOICE_POS, ev->slot)) {
        return DMR_SM_RESOLVE_REFUSED;
    }
    long freq = resolve_freq(state, ev->freq_hz, ev->lpcn);
    if (freq <= 0) {
        sm_log(opts, "grant-no-freq");
        return DMR_SM_RESOLVE_REFUSED;
    }
    if (dsd_receive_carriers_rejects(&ctx->receive_carriers, opts, freq)) {
        /* The channel's colour code was verified before and the receive policy rejects it: following the grant would
           only hold the receiver for the acquisition window again (issue #637). */
        sm_log(opts, "grant-receive-rejected");
        return DMR_SM_RESOLVE_REFUSED;
    }

    if (ev->freq_hz <= 0 && ev->lpcn > 0 && !lpcn_is_trusted(opts, state, ev->lpcn)) {
        return DMR_SM_RESOLVE_REFUSED;
    }

    *out_freq = freq;
    if (ctx->state == DMR_SM_TUNED && ctx->vc_freq_hz == freq) {
        sm_log(opts, "grant-same-freq");
        return DMR_SM_RESOLVE_SAME_FREQ;
    }
    return DMR_SM_RESOLVE_TUNABLE;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

static void
update_voice_sync_state(dmr_sm_ctx_t* ctx, dsd_state* state, int slot, double now_m) {
    if (!state) {
        return;
    }

    state->last_vc_sync_time_m = now_m;
    if (ctx->state == DMR_SM_TUNED && ctx->vc_slot < 0) {
        ctx->vc_slot = slot;
        state->dmr_mono_slot = slot;
        /* A slotless grant's call is the one its first accepted voice is on: its identity, followed since the tune. */
        dmr_sm_slot_ctx_t* rec = &ctx->slots[slot];
        if (ctx->vc_identity_known && !dmr_sm_slot_identity_known(rec)) {
            const dsd_call_kind kind = ctx->vc_data_call  ? DSD_CALL_KIND_DATA
                                       : ctx->vc_is_group ? DSD_CALL_KIND_GROUP_VOICE
                                                          : DSD_CALL_KIND_PRIVATE_VOICE;
            dmr_sm_slot_identity_set(rec, kind, (uint32_t)(ctx->vc_is_group ? ctx->vc_tg : ctx->vc_dst),
                                     (uint32_t)ctx->vc_src, ctx->vc_is_group, ctx->vc_data_call, DMR_SM_ID_GRANT,
                                     ctx->vc_lpcn, ctx->vc_freq_hz, ctx->t_tune_m);
        }
    }
}

/* ============================================================================
 * Capacity Plus channel-status identity
 * ============================================================================ */

void
dmr_sm_capplus_seed_arm(dmr_sm_ctx_t* ctx, int slot, uint32_t tg, int lsn, long freq_hz, uint32_t receiver_hz,
                        uint8_t crc_flags, uint64_t generation_before_tune) {
    if (!ctx || !ctx->initialized || slot < 0 || slot > 1) {
        return;
    }
    dmr_sm_capplus_seed_t* seed = &ctx->capplus_seed[slot];
    DSD_MEMSET(seed, 0, sizeof(*seed));
    if (tg == 0U || freq_hz <= 0) {
        return;
    }
    seed->armed = 1;
    seed->tg = tg;
    seed->lsn = lsn;
    seed->freq_hz = freq_hz;
    seed->receiver_hz = receiver_hz;
    seed->crc_flags = crc_flags;
    seed->armed_m = dsd_decode_now_mono_s();
    // A tune accepted as PENDING completes later, so the generation read after the request may or may not include it
    // yet. Read before, the completion this seed belongs to is exactly the next one.
    seed->generation = generation_before_tune + 1U;
    /* The slot's identity on the followed carrier, unless a checked LC already named the call there: what preemption
       compares a candidate against before any voice LC decodes. */
    dmr_sm_slot_ctx_t* rec = &ctx->slots[slot];
    if (!(rec->prov == (uint8_t)DMR_SM_ID_LC && rec->carrier_hz == freq_hz)) {
        dmr_sm_slot_identity_set(rec, DSD_CALL_KIND_GROUP_VOICE, tg, 0U, 1, 0, DMR_SM_ID_SEED, lsn, freq_hz,
                                 seed->armed_m);
    }
}

void
dmr_sm_status_identity_reconcile(dmr_sm_ctx_t* ctx, int slot, int lsn, uint32_t tg) {
    if (!ctx || slot < 0 || slot > 1 || tg == 0U || ctx->state != DMR_SM_TUNED || !ctx->vc_status_follow) {
        return;
    }
    dmr_sm_slot_ctx_t* rec = &ctx->slots[slot];
    if (rec->prov != (uint8_t)DMR_SM_ID_SEED || rec->channel != lsn || rec->carrier_hz != ctx->vc_freq_hz) {
        return;
    }
    dmr_sm_slot_identity_set(rec, DSD_CALL_KIND_GROUP_VOICE, tg, 0U, 1, 0, DMR_SM_ID_SEED, lsn, ctx->vc_freq_hz,
                             dsd_decode_now_mono_s());
}

void
dmr_sm_capplus_seed_clear(dmr_sm_ctx_t* ctx, int slot) {
    if (!ctx) {
        return;
    }
    for (int s = 0; s < 2; s++) {
        if (slot < 0 || slot == s) {
            DSD_MEMSET(&ctx->capplus_seed[s], 0, sizeof(ctx->capplus_seed[s]));
        }
    }
}

void
dmr_sm_capplus_seed_reconcile(dmr_sm_ctx_t* ctx, int slot, int lsn, uint32_t tg) {
    if (!ctx || slot < 0 || slot > 1) {
        return;
    }
    const dmr_sm_capplus_seed_t* seed = &ctx->capplus_seed[slot];
    if (seed->armed && seed->lsn == lsn && seed->tg != tg) {
        dmr_sm_capplus_seed_clear(ctx, slot);
    }
}

/* Whether the seed still describes the carrier the decoder is on. Neither check alone suffices: a manual retune moves
 * the receiver without a tuning generation (request_manual_tune() goes straight to io_control), and a trunk-scan
 * rotation can come back to the same frequency for another target. -1: never will, drop it; 0: not yet (the tune is
 * still completing); 1: current. */
static int
dmr_sm_capplus_seed_current(const dmr_sm_capplus_seed_t* seed, const dsd_opts* opts, double now_m) {
    if (now_m - seed->armed_m > DMR_CAPPLUS_SEED_WINDOW_S || now_m < seed->armed_m) {
        return -1;
    }
    const uint64_t generation = dsd_trunk_tuning_generation();
    if (generation < seed->generation) {
        return 0;
    }
    if (!dsd_trunk_tuning_frame_is_current(seed->generation)) {
        return -1;
    }
    return (opts->rtlsdr_center_freq == seed->receiver_hz) ? 1 : -1;
}

// The vocoder's identity-less epoch for this transmission: DMR, opened after the status (an older one would be a call
// from before the follow tune), and named by nothing yet.
static int
dmr_sm_capplus_call_is_unnamed_since(const dsd_call_snapshot* call, double armed_m) {
    return DSD_SYNC_IS_DMR(call->protocol) && call->started_m >= armed_m && call->ota_target_id == 0U
           && call->policy_target_id == 0U && call->ota_source_id == 0U && call->target_text[0] == '\0'
           && call->source_text[0] == '\0';
}

/* Name the slot's call from the channel status that sent Cap+ here, only while no voice LC has. The vocoder opens an
 * identity-less epoch on the first voice frame; this specializes that epoch and nothing else. It never opens one (a
 * superframe later the slot either carries such an epoch or an LC has named the call), never contradicts an LC, and
 * never reopens an ended call, which the canonical layer may still heal. One use, whatever the outcome: once the slot
 * has a named or a seeded call, the status has said all it can. */
static void
dmr_sm_capplus_seed_apply(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, double now_m) {
    const dmr_sm_capplus_seed_t* seed = &ctx->capplus_seed[slot];
    if (!seed->armed || !opts || !state) {
        return;
    }
    // Single-slot decoding folds both slots' LCs onto slot 0, so a per-slot status cannot be matched to it.
    const int single_slot = state->dmr_ms_mode == 1 || (opts->dmr_mono == 1 && state->dmr_stereo == 0);
    const int current = single_slot ? -1 : dmr_sm_capplus_seed_current(seed, opts, now_m);
    if (current < 0) {
        dmr_sm_capplus_seed_clear(ctx, slot);
        return;
    }
    if (current == 0) {
        return;
    }
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return;
    }
    const dmr_sm_capplus_seed_t used = *seed;
    dmr_sm_capplus_seed_clear(ctx, slot);
    if (!dmr_sm_capplus_call_is_unnamed_since(&call, used.armed_m)) {
        return;
    }
    const dsd_call_observation observation = {
        .protocol = call.protocol,
        .slot = (uint8_t)slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = used.tg,
        .policy_target_id = used.tg,
        .channel = (uint32_t)used.lsn,
        .frequency_hz = used.freq_hz,
        .observed_m = now_m,
    };
    const uint8_t saved_crc_invalid = state->event_crc_invalid[slot];
    state->event_crc_invalid[slot] = (uint8_t)(saved_crc_invalid | used.crc_flags);
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_event_sync_slot(opts, state, (uint8_t)slot);
    state->event_crc_invalid[slot] = saved_crc_invalid;
}

/* The time of the slot's last accepted voice when it may hold a followed call: the slot's voice gate is open, and its
   own voice passed the colour-code gate since the follow tune and within DMR_VOICE_EVIDENCE_S. 0 otherwise: a voice
   sync alone (a burst whose EMB the gate rejected, or carried over from the carrier the call was granted on) never
   holds a call. */
static double
dmr_sm_voice_evidence_m(const dmr_sm_ctx_t* ctx, const dsd_state* state, int slot, double now_m) {
    if (!state || slot < 0 || slot > 1 || !state->dmr_confidence_voice_open[slot]) {
        return 0.0;
    }
    const double evidence_m = state->dmr_confidence_voice_m[slot];
    if (evidence_m <= 0.0 || evidence_m < ctx->t_tune_m || (now_m - evidence_m) > DMR_VOICE_EVIDENCE_S) {
        return 0.0;
    }
    return evidence_m;
}

/* The call timers run from the slot's accepted voice and only ever move forward within a call. */
static void
dmr_sm_note_voice_evidence(dmr_sm_ctx_t* ctx, const dsd_state* state, int slot, double now_m) {
    const double evidence_m = dmr_sm_voice_evidence_m(ctx, state, slot, now_m);
    if (evidence_m <= 0.0) {
        return;
    }
    ctx->slots[slot].voice_active = 1;
    if (evidence_m > ctx->slots[slot].last_active_m) {
        ctx->slots[slot].last_active_m = evidence_m;
    }
    if (evidence_m > ctx->t_voice_m) {
        ctx->t_voice_m = evidence_m;
    }
}

static void
handle_voice_sync(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx) {
        return;
    }

    double now_m = dsd_decode_now_mono_s();
    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    if (!dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, s)) {
        return;
    }
    dmr_sm_note_voice_evidence(ctx, state, s, now_m);
    update_voice_sync_state(ctx, state, s, now_m);

    if (state && ctx->state == DMR_SM_TUNED && ctx->vc_identity_published == 0
        && (ctx->vc_slot < 0 || ctx->vc_slot == s)) {
        int protocol = DSD_SYNC_IS_DMR(state->synctype) ? state->synctype : state->lastsynctype;
        if (!DSD_SYNC_IS_DMR(protocol)) {
            protocol = DSD_SYNC_DMR_BS_VOICE_POS;
        }
        const uint64_t target = (uint64_t)(ctx->vc_is_group ? ctx->vc_tg : ctx->vc_dst);
        const dsd_call_observation observation = {
            .protocol = protocol,
            .slot = (uint8_t)s,
            .kind = ctx->vc_is_group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE,
            .ota_target_id = target,
            .policy_target_id = target,
            .ota_source_id = (uint64_t)ctx->vc_src,
            .channel = (uint32_t)ctx->vc_lpcn,
            .frequency_hz = ctx->vc_freq_hz,
            .observed_m = now_m,
        };
        const uint8_t saved_crc_invalid = state->event_crc_invalid[s];
        state->event_crc_invalid[s] = (uint8_t)(saved_crc_invalid | ctx->vc_crc_flags);
        const int observed = dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
        state->event_crc_invalid[s] = saved_crc_invalid;
        if (observed >= 0) {
            ctx->vc_identity_published = 1;
        }
    }
    dmr_sm_capplus_seed_apply(ctx, opts, state, s, now_m);

    sm_log(opts, "voice-sync");
}

static void
handle_data_sync(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx || !opts) {
        return;
    }

    if (opts->trunk_tune_data_calls != 1) {
        return;
    }

    double now_m = dsd_decode_now_mono_s();
    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    if (!dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_DATA_POS, s)) {
        return;
    }
    // Accepted data a followed call carries is activity, as accepted voice is: a zero hangtime holds while it flows.
    ctx->slots[s].voice_active = 1;
    ctx->slots[s].last_active_m = now_m;
    ctx->t_voice_m = now_m;

    if (state) {
        state->last_vc_sync_time_m = now_m;
    }

    sm_log(opts, "data-sync");
}

static void
handle_release(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx) {
        return;
    }

    if (ctx->state != DMR_SM_TUNED) {
        return;
    }

    if (state && state->trunk_sm_force_release != 0) {
        do_release(ctx, opts, state, "release-forced");
        return;
    }

    if (slot < 0 || slot > 1) {
        // Clear both slots for channel-wide release
        ctx->slots[0].voice_active = 0;
        ctx->slots[1].voice_active = 0;
    } else {
        ctx->slots[slot].voice_active = 0;
    }

    sm_log(opts, "release-requested");
}

static void
tick_on_cc(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double now_m) {
    if (!ctx->cc_acquiring && ctx->t_cc_sync_m <= 0.0) {
        ctx->t_cc_sync_m = now_m; // a carrier left without one (an abandoned call) still gets a window
        return;
    }
    if (dmr_cc_window_open(ctx, state, now_m)) {
        return;
    }
    if (ctx->site_without_cc && ctx->cc_rx_freq_hz > 0 && ctx->cc_rx_freq_hz == dmr_cc_anchor(ctx, state)
        && dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation)) {
        ctx->t_cc_sync_m = now_m; // XPT, already listening where it parks: nothing to hunt for
        ctx->cc_acquiring = 0;
        return;
    }
    set_state(ctx, opts, DMR_SM_HUNTING, "cc-lost");
    ctx->cc_window_lost = 1;
}

/* Whether the slot's voice is live: active, and its last accepted voice no older than DMR_VOICE_STALE_S. */
static int
dmr_sm_slot_voice_live(const dmr_sm_ctx_t* ctx, int slot, double now_m) {
    const dmr_sm_slot_ctx_t* s = &ctx->slots[slot];
    return s->voice_active && (s->last_active_m <= 0.0 || (now_m - s->last_active_m) <= DMR_VOICE_STALE_S);
}

static void
clear_stale_voice_slots(dmr_sm_ctx_t* ctx, double now_m) {
    for (int s = 0; s < 2; s++) {
        if (ctx->slots[s].voice_active && !dmr_sm_slot_voice_live(ctx, s, now_m)) {
            ctx->slots[s].voice_active = 0;
        }
    }
}

static int
has_voice_activity(const dmr_sm_ctx_t* ctx) {
    return ctx->slots[0].voice_active || ctx->slots[1].voice_active;
}

static int
hangtime_expired(const dmr_sm_ctx_t* ctx, double now_m, double hangtime) {
    if (ctx->t_voice_m <= 0.0) {
        return 0;
    }
    return (now_m - ctx->t_voice_m) >= hangtime;
}

static int
grant_timeout_expired(const dmr_sm_ctx_t* ctx, double now_m, double grant_timeout) {
    if (ctx->t_tune_m <= 0.0) {
        return 0;
    }
    return (now_m - ctx->t_tune_m) >= grant_timeout;
}

/* Receive admission on the voice channel at @p freq the receiver is on (issue #637), whether a followed grant or a
   CSBK path (Cap+, Connect Plus) tuned it: the colour code verified there is remembered, and a carrier no slot of which
   the policy admits is released once its acquisition window closes. A new channel starts a window of its own; repeated
   grants to the same one cannot extend it. Returns 1 while nothing on the carrier is admitted. */
static int
dmr_sm_receive_check(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, long freq, double now_m,
                     double grant_timeout) {
    if (freq != ctx->receive_check_freq_hz) {
        ctx->receive_check_freq_hz = freq;
        ctx->receive_check_started_m = 0.0;
        /* Its codes are its own only when its tune forgot the control channel's (dsd_engine_update_vc_tune_state()
           does): decided at the tune, not now. */
        ctx->receive_codes_fresh = state && freq != 0 && state->receive_codes_fresh_freq == freq;
    }
    if (ctx->receive_codes_fresh) {
        dsd_receive_carriers_note(&ctx->receive_carriers, state, DSD_SYNC_DMR_BS_VOICE_POS, freq);
    }
    int admitted = 0;
    for (int slot = 0; slot < 2; slot++) {
        if (dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, slot)) {
            admitted = 1;
        } else {
            ctx->slots[slot].voice_active = 0;
        }
    }
    if (admitted) {
        ctx->receive_check_started_m = 0.0;
        return 0;
    }
    if (ctx->receive_check_started_m <= 0.0) {
        ctx->receive_check_started_m = now_m;
    }
    if (now_m - ctx->receive_check_started_m >= grant_timeout) {
        ctx->receive_check_started_m = 0.0;
        do_release(ctx, opts, state, "receive-filter");
    }
    return 1;
}

static int
dmr_sm_any_slot_admitted(const dsd_opts* opts, const dsd_state* state) {
    for (int slot = 0; slot < 2; slot++) {
        if (dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, slot)) {
            return 1;
        }
    }
    return 0;
}

/* Why a TUNED machine keeps its carrier at @p now_m, without side effects: the rule tick_tuned() applies, shared with
   the busy view (dmr_sm_describe_held_calls()). Live voice on an admitted slot holds whatever the hangtime; then the
   hangtime tail from the last activity; before any voice, acquisition for @p acquire_s from the tune. A carrier no slot
   of which the receive policy admits is held while its receive check runs (the colour code still unknown), but not
   once the code verified there is rejected. */
typedef enum {
    DMR_SM_HOLD_NONE = 0,
    DMR_SM_HOLD_LIVE,
    DMR_SM_HOLD_TAIL,
    DMR_SM_HOLD_ACQUIRING,
} dmr_sm_hold_e;

static dmr_sm_hold_e
dmr_sm_tuned_retention(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double now_m,
                       double hangtime, double grant_timeout, double acquire_s) {
    if (state && state->trunk_sm_force_release != 0) {
        return DMR_SM_HOLD_NONE;
    }
    if (!dmr_sm_any_slot_admitted(opts, state)) {
        if (dsd_receive_carriers_rejects(&ctx->receive_carriers, opts, ctx->vc_freq_hz)) {
            return DMR_SM_HOLD_NONE;
        }
        if (ctx->receive_check_freq_hz == ctx->vc_freq_hz && ctx->receive_check_started_m > 0.0
            && now_m - ctx->receive_check_started_m >= grant_timeout) {
            return DMR_SM_HOLD_NONE;
        }
        return DMR_SM_HOLD_ACQUIRING;
    }
    for (int s = 0; s < 2; s++) {
        if (dmr_sm_slot_voice_live(ctx, s, now_m) && dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, s)) {
            return DMR_SM_HOLD_LIVE;
        }
    }
    if (ctx->t_voice_m > 0.0) {
        return hangtime_expired(ctx, now_m, hangtime) ? DMR_SM_HOLD_NONE : DMR_SM_HOLD_TAIL;
    }
    return grant_timeout_expired(ctx, now_m, acquire_s) ? DMR_SM_HOLD_NONE : DMR_SM_HOLD_ACQUIRING;
}

static void
tick_tuned(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double hangtime, double grant_timeout) {
    if (state && state->trunk_sm_force_release != 0) {
        do_release(ctx, opts, state, "release-forced");
        return;
    }
    if (dmr_sm_receive_check(ctx, opts, state, ctx->vc_freq_hz, now_m, grant_timeout)) {
        return;
    }
    clear_stale_voice_slots(ctx, now_m);

    if (has_voice_activity(ctx)) {
        // Each active slot's activity is the time of its last accepted voice (handle_voice_sync()).
        for (int s = 0; s < 2; s++) {
            if (ctx->slots[s].voice_active && ctx->slots[s].last_active_m > ctx->t_voice_m) {
                ctx->t_voice_m = ctx->slots[s].last_active_m;
            }
        }
        return;
    }

    if (dmr_sm_tuned_retention(ctx, opts, state, now_m, hangtime, grant_timeout, grant_timeout) == DMR_SM_HOLD_NONE) {
        do_release(ctx, opts, state, ctx->t_voice_m > 0.0 ? "hangtime-expired" : "grant-timeout");
    }
}

/* ============================================================================
 * Busy view, hold-or-preempt gate and grant submission
 *
 * Decoder thread, under the P25 SM tick guard (frame dispatch and the DMR tick hold it): the followed-call ledger these
 * write is the talkgroup policy's, whose contract requires that guard.
 * ============================================================================ */

/* How long a followed carrier's acquisition before its first voice holds the receiver against other candidates: the
   grant timeout, but a carrier a channel status selected (Capacity Plus, XPT) keeps today's status window. */
static double
dmr_sm_acquire_window(const dmr_sm_ctx_t* ctx) {
    if ((ctx->vc_status_follow || ctx->vc_status_grant) && ctx->grant_timeout_s > DMR_CAPPLUS_SEED_WINDOW_S) {
        return DMR_CAPPLUS_SEED_WINDOW_S;
    }
    return ctx->grant_timeout_s;
}

/* The grant's own identity, for a held slot whose record names nothing yet (a slotless grant before its voice). */
static int
dmr_sm_grant_identity(const dmr_sm_ctx_t* ctx, dmr_sm_slot_ctx_t* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!ctx->vc_identity_known || ctx->vc_status_follow) {
        return 0;
    }
    const uint32_t target = (uint32_t)(ctx->vc_is_group ? ctx->vc_tg : ctx->vc_dst);
    if (target == 0U) {
        return 0;
    }
    const dsd_call_kind kind = ctx->vc_data_call  ? DSD_CALL_KIND_DATA
                               : ctx->vc_is_group ? DSD_CALL_KIND_GROUP_VOICE
                                                  : DSD_CALL_KIND_PRIVATE_VOICE;
    dmr_sm_slot_identity_set(out, kind, target, (uint32_t)ctx->vc_src, ctx->vc_is_group, ctx->vc_data_call,
                             DMR_SM_ID_GRANT, ctx->vc_lpcn, ctx->vc_freq_hz, ctx->t_tune_m);
    return 1;
}

/* Describe one held call into the ledger (slot -1: both). Returns its fresh decision's Hold match. */
static int
dmr_sm_describe_identity(const dsd_opts* opts, dsd_state* state, const dmr_sm_slot_ctx_t* id, long carrier_hz, int slot,
                         double now_m) {
    dsd_tg_policy_decision decision;
    if (!dmr_sm_slot_identity_decision(opts, state, id, &decision)) {
        (void)dsd_tg_policy_clear_active_call(state, slot);
        return 0;
    }
    const dsd_tg_policy_call_route route = {
        .target_id = id->target,
        .source_id = id->source,
        .freq_hz = carrier_hz,
        .channel = id->channel,
        .slot = slot,
        .requires_tuner_retune = 0,
    };
    if (dsd_tg_policy_describe_followed_call(state, &route, &decision, id->follow_m, now_m) != 0) {
        (void)dsd_tg_policy_clear_active_call(state, slot);
    }
    return decision.tg_hold_match;
}

/* TUNED: which slots hold a call. Live voice or the slot's own hangtime tail; before any voice, the grant's slot (a
   slotless grant: each slot a status seeded, else none, the grant's identity then standing for the carrier). */
static int
dmr_sm_tuned_members(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double now_m,
                     dmr_sm_hold_e hold) {
    int members = 0;
    for (int s = 0; s < 2; s++) {
        const dmr_sm_slot_ctx_t* slot = &ctx->slots[s];
        const int live =
            dmr_sm_slot_voice_live(ctx, s, now_m) && dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, s);
        const int tail = slot->last_active_m > 0.0 && (now_m - slot->last_active_m) < ctx->hangtime_s;
        if (live || tail) {
            members |= 1 << s;
        }
    }
    if (hold == DMR_SM_HOLD_ACQUIRING) {
        if (ctx->vc_slot >= 0 && ctx->vc_slot <= 1) {
            members |= 1 << ctx->vc_slot;
        } else {
            for (int s = 0; s < 2; s++) {
                if (dmr_sm_slot_identity_known(&ctx->slots[s]) && ctx->slots[s].carrier_hz == ctx->vc_freq_hz) {
                    members |= 1 << s;
                }
            }
        }
    }
    return members;
}

/* ON_CC: the slots of the carrier the receiver listens on whose calls (named there by a checked LC or a seed) are live
   or in their hangtime tail and admitted by the policy. An unknown or blocked call never makes it busy. */
static int
dmr_sm_resident_members(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, long carrier_hz,
                        double now_m) {
    int members = 0;
    for (int s = 0; s < 2; s++) {
        const dmr_sm_slot_ctx_t* slot = &ctx->slots[s];
        if (!dmr_sm_slot_identity_known(slot) || slot->carrier_hz != carrier_hz) {
            continue;
        }
        const int live = dmr_sm_voice_evidence_m(ctx, state, s, now_m) > 0.0;
        const int tail = slot->last_active_m > 0.0 && (now_m - slot->last_active_m) < ctx->hangtime_s;
        dsd_tg_policy_decision decision;
        if ((live || tail) && dmr_sm_slot_identity_decision(opts, state, slot, &decision) && decision.tune_allowed) {
            members |= 1 << s;
        }
    }
    return members;
}

int
dmr_sm_describe_held_calls(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, double now_m,
                           dmr_sm_busy_view_t* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!ctx || !opts || !state) {
        return 0;
    }
    int members = 0;
    if (ctx->state == DMR_SM_TUNED) {
        out->carrier_hz = ctx->vc_freq_hz;
        const dmr_sm_hold_e hold = dmr_sm_tuned_retention(ctx, opts, state, now_m, ctx->hangtime_s,
                                                          ctx->grant_timeout_s, dmr_sm_acquire_window(ctx));
        out->held = hold != DMR_SM_HOLD_NONE;
        if (out->held) {
            members = dmr_sm_tuned_members(ctx, opts, state, now_m, hold);
        }
    } else if (ctx->state == DMR_SM_ON_CC) {
        out->carrier_hz = dmr_cc_current_frequency(opts, ctx);
        if (out->carrier_hz > 0) {
            members = dmr_sm_resident_members(ctx, opts, state, out->carrier_hz, now_m);
            out->held = members != 0;
            out->resident = out->held;
        }
    }
    if (!out->held) {
        (void)dsd_tg_policy_clear_active_call(state, -1);
        return 0;
    }
    out->members = members;
    if (members == 0) {
        // Held before any slot carries a call (a slotless grant's acquisition, a pending receive check): the grant's
        // own identity stands for the carrier.
        dmr_sm_slot_ctx_t grant;
        if (dmr_sm_grant_identity(ctx, &grant)) {
            out->members = 0x3;
            out->holds_hold_target = dmr_sm_describe_identity(opts, state, &grant, out->carrier_hz, -1, now_m);
        } else {
            out->unknown_identity = 1;
            (void)dsd_tg_policy_clear_active_call(state, -1);
        }
        return 1;
    }
    for (int s = 0; s < 2; s++) {
        if (!(members & (1 << s))) {
            (void)dsd_tg_policy_clear_active_call(state, s);
            continue;
        }
        dmr_sm_slot_ctx_t id = ctx->slots[s];
        const int known = (dmr_sm_slot_identity_known(&id) && id.carrier_hz == out->carrier_hz)
                          || (s == ctx->vc_slot && ctx->state == DMR_SM_TUNED && dmr_sm_grant_identity(ctx, &id));
        if (!known) {
            out->unknown_identity = 1;
            (void)dsd_tg_policy_clear_active_call(state, s);
            continue;
        }
        if (dmr_sm_describe_identity(opts, state, &id, out->carrier_hz, s, now_m)) {
            out->holds_hold_target = 1;
        }
    }
    return 1;
}

dmr_sm_displace_e
dmr_sm_may_displace(const dmr_sm_busy_view_t* view, const dsd_opts* opts, const dsd_state* state,
                    const dsd_tg_policy_call_route* candidate, const dsd_tg_policy_decision* decision, int policy_valid,
                    int identity_known, double now_m) {
    if (!view || !candidate || !decision || !policy_valid || !decision->tune_allowed) {
        return DMR_SM_DISPLACE_NONE;
    }
    if (decision->tg_hold_match && !view->holds_hold_target) {
        return DMR_SM_DISPLACE_HOLD;
    }
    if (identity_known && !view->unknown_identity
        && dsd_tg_policy_should_preempt(opts, state, candidate, decision, now_m)) {
        return DMR_SM_DISPLACE_PREEMPT;
    }
    return DMR_SM_DISPLACE_NONE;
}

void
dmr_sm_policy_note_follow(dsd_state* state, const dsd_tg_policy_call_route* route,
                          const dsd_tg_policy_decision* decision, int preempted, double now_m) {
    if (!state) {
        return;
    }
    (void)dsd_tg_policy_clear_active_call(state, -1);
    if (route && decision) {
        (void)dsd_tg_policy_note_active_call(state, route, decision, now_m);
    }
    if (preempted) {
        (void)dsd_tg_policy_commit_preemption(state, now_m);
    }
}

/* Whether the grant names its call. An event built without the CSBK classification (kind UNKNOWN, zero-initialized)
   names it as before; only a status placeholder (XPT private) says it does not. */
static int
dmr_sm_event_identity_known(const dmr_sm_event_t* ev) {
    return ev->identity_known || ev->kind == DSD_CALL_KIND_UNKNOWN;
}

static dsd_call_kind
dmr_sm_event_kind(const dmr_sm_event_t* ev) {
    if (ev->kind != DSD_CALL_KIND_UNKNOWN) {
        return ev->kind;
    }
    return ev->is_group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
}

/* Tune to an accepted grant's carrier and make its call the machine's. */
static dmr_sm_grant_result_e
dmr_sm_follow_grant(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev, long freq,
                    dmr_sm_displace_e displace) {
    /* A validated grant on a completed probe proves where to return, even
     * before the next ALOHA/system-parameters message arrives. */
    if (ctx->cc_acquiring && opts->trunk_is_tuned == 0 && dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation)) {
        dmr_cc_note_activity_ctx(ctx, opts, state, ctx->cc_probe_freq_hz);
    }
    double now_m = dsd_decode_now_mono_s();

    dsd_trunk_tune_result tune_result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, 0, NULL); // DMR: no TED SPS override
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        sm_log(opts, "grant-tune-deferred");
        return DMR_SM_GRANT_DEFERRED;
    }

    const dsd_call_kind kind = dmr_sm_event_kind(ev);
    const int identity_known = dmr_sm_event_identity_known(ev);
    ctx->vc_freq_hz = freq;
    ctx->vc_lpcn = ev->lpcn;
    ctx->vc_tg = ev->tg;
    ctx->vc_dst = ev->dst;
    ctx->vc_src = ev->src;
    ctx->vc_slot = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : -1;
    ctx->vc_is_group = ev->is_group != 0;
    ctx->vc_identity_known = identity_known;
    ctx->vc_data_call = kind == DSD_CALL_KIND_DATA;
    // A placeholder names no call: the voice LC will, through the canonical layer.
    ctx->vc_identity_published = identity_known ? 0 : 1;
    ctx->vc_status_follow = 0;
    ctx->vc_status_grant = ev->origin == DMR_SM_ORIGIN_STATUS;
    // Grants are dispatched synchronously from inside the CSBK/MBC burst's CRC scope.
    ctx->vc_crc_flags = state->event_crc_invalid[state->currentslot & 1];
    ctx->t_tune_m = now_m;
    ctx->t_voice_m = 0.0;
    ctx->cc_acquiring = 0;
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_superseded = 0; // a call followed since an operator move ends that move's listening window
    state->dmr_mono_slot = ctx->vc_slot;

    dmr_sm_slots_reset(ctx);
    const uint32_t target = (uint32_t)(ev->is_group ? ev->tg : ev->dst);
    if (ctx->vc_slot >= 0 && identity_known && target != 0U) {
        dmr_sm_slot_identity_set(&ctx->slots[ctx->vc_slot], kind, target, (uint32_t)ev->src, ev->is_group,
                                 ctx->vc_data_call, DMR_SM_ID_GRANT, ev->lpcn, freq, now_m);
    }
    if (ev->policy_valid && identity_known) {
        const dsd_tg_policy_call_route route = {
            .target_id = target,
            .source_id = (uint32_t)ev->src,
            .freq_hz = freq,
            .channel = ev->lpcn,
            .slot = ctx->vc_slot,
            .requires_tuner_retune = 0,
        };
        dmr_sm_policy_note_follow(state, &route, &ev->policy, displace == DMR_SM_DISPLACE_PREEMPT, now_m);
    } else {
        dmr_sm_policy_note_follow(state, NULL, NULL, 0, now_m);
    }

    dmr_reset_blocks(opts, state);

    state->last_t3_tune_time_m = now_m;
    state->p25_sm_tune_count++;

    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n  DMR SM: Tune VC freq=%.6lf MHz\n", (double)freq / 1000000.0);
    }

    set_state(ctx, opts, DMR_SM_TUNED, displace == DMR_SM_DISPLACE_NONE ? "grant" : "grant-takeover");
    return DMR_SM_GRANT_FOLLOWED;
}

/* The identity of the call the machine follows, judged as it was followed: the followed slot's record, else the
   grant's. */
static int
dmr_sm_followed_identity(const dmr_sm_ctx_t* ctx, dmr_sm_slot_ctx_t* out) {
    if (ctx->vc_slot >= 0 && ctx->vc_slot <= 1) {
        const dmr_sm_slot_ctx_t* rec = &ctx->slots[ctx->vc_slot];
        if (dmr_sm_slot_identity_known(rec) && rec->carrier_hz == ctx->vc_freq_hz) {
            *out = *rec;
            return 1;
        }
    }
    return dmr_sm_grant_identity(ctx, out);
}

/* A C_MOVE of the call the machine follows, whichever slot it names: its target is the followed grant's (the talkgroup,
   or either party of a private call) or the followed slot's known identity. A move of the companion slot's call is not
   the followed call's. */
static int
dmr_sm_move_matches(const dmr_sm_ctx_t* ctx, const dmr_sm_event_t* ev) {
    if (ctx->state != DMR_SM_TUNED) {
        return 0;
    }
    const uint32_t target = (uint32_t)(ev->is_group ? ev->tg : ev->dst);
    if (target == 0U) {
        return 0;
    }
    if (ctx->vc_identity_known && !ctx->vc_status_follow) {
        if (ctx->vc_is_group ? target == (uint32_t)ctx->vc_tg
                             : (target == (uint32_t)ctx->vc_dst || target == (uint32_t)ctx->vc_src)) {
            return 1;
        }
    }
    dmr_sm_slot_ctx_t followed;
    return dmr_sm_followed_identity(ctx, &followed) && followed.target == target;
}

/* A matched C_MOVE: the followed call goes where the system moved it unless its own classification is now denied
   tuning for a reason other than a Hold mismatch (a lockout, skip, avoid or mode change since it started); media-only
   restrictions never stop following it. The move keeps the call's follow time, so a later candidate's dwell runs from
   the conversation's first follow. */
static dmr_sm_grant_result_e
dmr_sm_follow_move(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    dmr_sm_slot_ctx_t moved;
    if (!dmr_sm_followed_identity(ctx, &moved)) {
        return DMR_SM_GRANT_REFUSED;
    }
    dsd_tg_policy_decision decision;
    if (!dmr_sm_slot_identity_decision(opts, state, &moved, &decision)) {
        return DMR_SM_GRANT_REFUSED;
    }
    const uint32_t non_tuning = (uint32_t)DSD_TG_POLICY_BLOCK_HOLD | (uint32_t)DSD_TG_POLICY_BLOCK_AUDIO
                                | (uint32_t)DSD_TG_POLICY_BLOCK_RECORD | (uint32_t)DSD_TG_POLICY_BLOCK_STREAM;
    if (!decision.tune_allowed && (decision.block_reasons & ~non_tuning) != 0U) {
        sm_log(opts, "move-refused-policy");
        return DMR_SM_GRANT_REFUSED;
    }
    long freq = 0;
    const dmr_sm_resolve_e resolved = dmr_sm_resolve_tunable_grant(ctx, opts, state, ev, &freq);
    if (resolved == DMR_SM_RESOLVE_REFUSED) {
        return DMR_SM_GRANT_REFUSED;
    }
    const int new_slot = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : ctx->vc_slot;
    const double now_m = dsd_decode_now_mono_s();
    if (resolved == DMR_SM_RESOLVE_SAME_FREQ) {
        if (new_slot >= 0 && new_slot != ctx->vc_slot) {
            if (ctx->vc_slot >= 0) {
                dmr_sm_slot_identity_clear(&ctx->slots[ctx->vc_slot]);
            }
            dmr_sm_slot_identity_set(&ctx->slots[new_slot], (dsd_call_kind)moved.kind, moved.target, moved.source,
                                     moved.judged_group, moved.data_call, (dmr_sm_identity_prov_e)moved.prov,
                                     moved.channel, moved.carrier_hz, moved.follow_m);
            ctx->slots[new_slot].follow_m = moved.follow_m;
            ctx->vc_slot = new_slot;
            state->dmr_mono_slot = new_slot;
        }
        sm_log(opts, "move-in-place");
        return DMR_SM_GRANT_IN_PLACE;
    }

    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, 0, NULL);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        sm_log(opts, "move-tune-deferred");
        return DMR_SM_GRANT_DEFERRED;
    }
    ctx->vc_freq_hz = freq;
    ctx->vc_lpcn = ev->lpcn;
    ctx->vc_slot = new_slot;
    ctx->vc_identity_published = ctx->vc_identity_known ? 0 : 1;
    ctx->vc_crc_flags = state->event_crc_invalid[state->currentslot & 1];
    ctx->t_tune_m = now_m; // a new acquisition on the new carrier
    ctx->t_voice_m = 0.0;
    ctx->cc_acquiring = 0;
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_superseded = 0;
    state->dmr_mono_slot = new_slot;
    dmr_sm_slots_reset(ctx);
    if (new_slot >= 0) {
        dmr_sm_slot_identity_set(&ctx->slots[new_slot], (dsd_call_kind)moved.kind, moved.target, moved.source,
                                 moved.judged_group, moved.data_call, (dmr_sm_identity_prov_e)moved.prov, ev->lpcn,
                                 freq, moved.follow_m);
        ctx->slots[new_slot].follow_m = moved.follow_m;
    }
    const dsd_tg_policy_call_route route = {
        .target_id = moved.target,
        .source_id = moved.source,
        .freq_hz = freq,
        .channel = ev->lpcn,
        .slot = new_slot,
        .requires_tuner_retune = 0,
    };
    if (new_slot >= 0) {
        (void)dsd_tg_policy_clear_active_call(state, 1 - new_slot);
    }
    (void)dsd_tg_policy_describe_followed_call(state, &route, &decision, moved.follow_m, now_m);

    dmr_reset_blocks(opts, state);
    state->last_t3_tune_time_m = now_m;
    state->p25_sm_tune_count++;
    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n  DMR SM: Move VC freq=%.6lf MHz\n", (double)freq / 1000000.0);
    }
    set_state(ctx, opts, DMR_SM_TUNED, "move");
    return DMR_SM_GRANT_FOLLOWED;
}

static dmr_sm_grant_result_e
dmr_sm_submit_grant_ctx(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    if (!ctx || !ev || !opts || !state) {
        return DMR_SM_GRANT_REFUSED;
    }
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_DMR);

    if (ev->origin == DMR_SM_ORIGIN_MOVE && dmr_sm_move_matches(ctx, ev)) {
        return dmr_sm_follow_move(ctx, opts, state, ev);
    }
    if (ev->policy_valid && !ev->policy.tune_allowed) {
        sm_log(opts, "grant-policy-blocked");
        return DMR_SM_GRANT_REFUSED;
    }

    long freq = 0;
    const dmr_sm_resolve_e resolved = dmr_sm_resolve_tunable_grant(ctx, opts, state, ev, &freq);
    if (resolved == DMR_SM_RESOLVE_REFUSED) {
        return DMR_SM_GRANT_REFUSED;
    }
    if (resolved == DMR_SM_RESOLVE_SAME_FREQ) {
        return DMR_SM_GRANT_IN_PLACE;
    }

    /* The receiver holds a call (or the caller's own window says so): a candidate on another carrier takes it only by
       Hold over a non-held call or by preemption. One on the held carrier is followed as before. */
    const double now_m = dsd_decode_now_mono_s();
    dmr_sm_busy_view_t view;
    (void)dmr_sm_describe_held_calls(ctx, opts, state, now_m, &view);
    dmr_sm_displace_e displace = DMR_SM_DISPLACE_NONE;
    if (freq != view.carrier_hz && (view.held || ev->caller_busy)) {
        const dsd_tg_policy_call_route candidate = {
            .target_id = (uint32_t)(ev->is_group ? ev->tg : ev->dst),
            .source_id = (uint32_t)ev->src,
            .freq_hz = freq,
            .channel = ev->lpcn,
            .slot = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : -1,
            .requires_tuner_retune = 1,
        };
        displace = dmr_sm_may_displace(&view, opts, state, &candidate, &ev->policy, ev->policy_valid,
                                       dmr_sm_event_identity_known(ev), now_m);
        if (displace == DMR_SM_DISPLACE_NONE) {
            sm_log(opts, "grant-busy");
            return DMR_SM_GRANT_BUSY;
        }
    }
    return dmr_sm_follow_grant(ctx, opts, state, ev, freq, displace);
}

static void
handle_grant(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    (void)dmr_sm_submit_grant_ctx(ctx, opts, state, ev);
}

dmr_sm_grant_result_e
dmr_sm_submit_grant(dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    if (!ctx->initialized) {
        dmr_sm_init_ctx(ctx, opts, state);
    }
    return dmr_sm_submit_grant_ctx(ctx, opts, state, ev);
}

void
dmr_sm_note_lc_identity(const dsd_opts* opts, dsd_state* state, int slot, dsd_call_kind kind, uint32_t target,
                        uint32_t source) {
    if (!opts || !state || opts->trunk_enable != 1 || slot < 0 || slot > 1 || target == 0U) {
        return;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    if (!ctx->initialized) {
        return;
    }
    long carrier_hz = 0;
    if (ctx->state == DMR_SM_TUNED) {
        carrier_hz = ctx->vc_freq_hz;
    } else if (ctx->state == DMR_SM_ON_CC) {
        carrier_hz = dmr_cc_current_frequency(opts, ctx);
    }
    if (carrier_hz <= 0) {
        return;
    }
    dmr_sm_slot_ctx_t* rec = &ctx->slots[slot];
    const int channel = rec->carrier_hz == carrier_hz ? rec->channel : (ctx->state == DMR_SM_TUNED ? ctx->vc_lpcn : 0);
    const int group = kind != DSD_CALL_KIND_PRIVATE_VOICE;
    dmr_sm_slot_identity_set(rec, group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE, target, source,
                             group, 0, DMR_SM_ID_LC, channel, carrier_hz, dsd_decode_now_mono_s());
}

/* ============================================================================
 * Public API - Core State Machine
 * ============================================================================ */

const char*
dmr_sm_state_name(dmr_sm_state_e state) {
    switch (state) {
        case DMR_SM_IDLE: return "IDLE";
        case DMR_SM_ON_CC: return "ON_CC";
        case DMR_SM_TUNED: return "TUNED";
        case DMR_SM_HUNTING: return "HUNT";
        default: return "?";
    }
}

void
dmr_sm_init_ctx(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (!ctx) {
        return;
    }

    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->vc_slot = -1;

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();

    ctx->hangtime_s = 2.0;
    ctx->grant_timeout_s = 4.0;
    ctx->cc_grace_s = 6.0;

    // Honor user hangtime setting, including zero for immediate release
    if (opts && opts->trunk_hangtime >= 0.0) {
        ctx->hangtime_s = opts->trunk_hangtime;
    }

    if (cfg && cfg->dmr_hangtime_is_set) {
        ctx->hangtime_s = cfg->dmr_hangtime_s;
    }
    if (cfg && cfg->dmr_grant_timeout_is_set) {
        ctx->grant_timeout_s = cfg->dmr_grant_timeout_s;
    }

    if (state && state->trunk_cc_freq != 0) {
        ctx->state = DMR_SM_ON_CC;
        ctx->t_cc_sync_m = dsd_decode_now_mono_s();
        ctx->cc_freq_hz = state->trunk_cc_freq;
        ctx->cc_probe_freq_hz = state->trunk_cc_freq;
    } else {
        ctx->state = DMR_SM_IDLE;
    }

    ctx->initialized = 1;
}

void
dmr_sm_event(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    if (!ctx || !ev) {
        return;
    }

    if (!ctx->initialized) {
        dmr_sm_init_ctx(ctx, opts, state);
    }

    switch (ev->type) {
        case DMR_SM_EV_GRANT: handle_grant(ctx, opts, state, ev); break;
        case DMR_SM_EV_VOICE_SYNC: handle_voice_sync(ctx, opts, state, ev->slot); break;
        case DMR_SM_EV_DATA_SYNC: handle_data_sync(ctx, opts, state, ev->slot); break;
        case DMR_SM_EV_RELEASE: handle_release(ctx, opts, state, ev->slot); break;
        case DMR_SM_EV_CC_SYNC: dmr_cc_note_heartbeat_ctx(ctx, opts, state); break;
        case DMR_SM_EV_SYNC_LOST: break;
    }
}

/* A machine the ownership gate does not admit: a startup hunt it may no longer run settles and parks here, where the
   decoder still ticks it, and an armed window is dropped (issue #703). */
static void
dmr_sm_tick_not_admitted(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx->initialized) {
        return;
    }
    if (opts && state && ctx->cold_start_hunt) {
        (void)dmr_sm_cold_start_gate(ctx, opts, state, dsd_decode_now_mono_s(), 0);
    } else {
        ctx->cold_start_armed_m = 0.0;
    }
}

/* Whether the tick's per-state work may run: a startup hunt's gate, an operator move's settle and a tracked tune's
   resolution come first. */
static int
dmr_sm_tick_ready(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (ctx->cold_start_hunt && !dmr_sm_cold_start_gate(ctx, opts, state, now_m, 0)) {
        return 0;
    }
    if (dmr_sm_settle_after_move(ctx, opts, state, now_m)) {
        return 0;
    }
    return dmr_cc_resolve_pending(ctx, opts, now_m);
}

void
dmr_sm_tick_ctx(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }
    if (!dsd_trunk_dmr_recovery_allowed(opts, state)) {
        dmr_sm_tick_not_admitted(ctx, opts, state);
        return;
    }

    if (!ctx->initialized) {
        dmr_sm_init_ctx(ctx, opts, state);
    }

    double now_m = dsd_decode_now_mono_s();
    double hangtime = ctx->hangtime_s;
    double grant_timeout = ctx->grant_timeout_s;
    if (!dmr_sm_tick_ready(ctx, opts, state, now_m)) {
        return;
    }

    switch (ctx->state) {
        case DMR_SM_IDLE: dmr_sm_tick_idle_cold_start(ctx, opts, state, now_m); break;

        case DMR_SM_ON_CC:
            if (!opts || opts->trunk_is_tuned != 1) {
                tick_on_cc(ctx, opts, state, now_m);
            } else if (state) {
                /* A voice channel a CSBK path tuned itself (Connect Plus, Hytera XPT) is judged as a followed grant's. */
                (void)dmr_sm_receive_check(ctx, opts, state, state->trunk_vc_freq[0], now_m, grant_timeout);
            }
            break;

        case DMR_SM_TUNED: tick_tuned(ctx, opts, state, now_m, hangtime, grant_timeout); break;

        case DMR_SM_HUNTING: dmr_cc_hunt(ctx, opts, state, now_m); break;
    }
}

void
dmr_sm_cold_start_note_retune(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx || !ctx->initialized) {
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    // Wherever the receiver went, it is not where the machine last saw it.
    ctx->rx_seen_hz = 0;
    ctx->rx_seen_generation = 0U;
    ctx->rx_query_retry_m = 0.0;
    if (ctx->cold_start_hunt) {
        // Parks keeping a window that runs from the move, once settled; the probe is not where the receiver is.
        ctx->cold_start_armed_m = now_m;
        ctx->cold_start_park_pending = 1;
        ctx->cc_probe_freq_hz = 0;
        ctx->cc_rx_freq_hz = 0;
        return;
    }
    if (ctx->state == DMR_SM_IDLE) {
        if (ctx->cold_start_armed_m > 0.0) {
            ctx->cold_start_armed_m = now_m;
        }
        return;
    }
    if (opts && opts->trunk_scan_enabled == 1) {
        return; // trunk scan moves the receiver itself and restarts each target it parks
    }
    if (ctx->state == DMR_SM_TUNED && opts && state) {
        dmr_sm_abandon_carrier(ctx, opts, state, "operator-move"); // the call stays on the frequency left
    }
    dmr_sm_slots_reset(ctx); // calls heard where the receiver was are not on the new frequency
    /* Listen on the new frequency for a full window: no receiver attribution (a heartbeat or a status there names it
       anew), no probe, and a tune still in flight or failed settles first (dmr_sm_settle_after_move()). The confirmed
       control channel stays the anchor the window's expiry returns to. */
    ctx->cc_rx_freq_hz = 0;
    ctx->cc_rx_generation = 0U;
    ctx->cc_probe_freq_hz = 0;
    ctx->cc_acquiring = 0;
    ctx->cc_acquire_start_m = 0.0;
    ctx->cc_retry_after_m = 0.0;
    ctx->cc_moved_m = now_m;
    ctx->t_cc_sync_m = now_m;
    ctx->cc_tune_superseded = 1;
    ctx->cc_settle_waited = 0;
    set_state(ctx, opts, DMR_SM_ON_CC, "operator-move");
}

void
dmr_sm_note_xpt_site(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    ctx->site_without_cc = 1;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_DMR);
    /* The site parks where it identified itself (a probe's completed frequency included), not on a frequency recovery
       left: anchor-only recovery would otherwise return there and never look again. A followed call is no parking. */
    if (opts && opts->trunk_is_tuned != 1 && ctx->state != DMR_SM_TUNED) {
        const long freq = dmr_cc_current_frequency(opts, ctx);
        if (freq > 0) {
            ctx->cc_freq_hz = freq;
            state->trunk_cc_freq = freq; // the return a trunk-scan revisit and a call's release go to
        }
    }
}

void
dmr_sm_note_system_replaced(dmr_sm_ctx_t* ctx) {
    if (ctx) {
        ctx->site_without_cc = 0;
    }
}

void
dmr_sm_note_hangtime(dmr_sm_ctx_t* ctx, const dsd_opts* opts) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!ctx || !opts || (cfg && cfg->dmr_hangtime_is_set) || opts->trunk_hangtime < 0.0) {
        return;
    }
    ctx->hangtime_s = opts->trunk_hangtime;
}

void
dmr_sm_note_receiver_unknown(dmr_sm_ctx_t* ctx) {
    if (ctx) {
        ctx->cc_rx_freq_hz = 0;
        ctx->cc_rx_generation = 0U;
        ctx->rx_seen_hz = 0;
        ctx->rx_seen_generation = 0U;
        ctx->rx_query_retry_m = 0.0;
    }
}

int
dmr_sm_receiver_on(const dsd_opts* opts, long freq_hz) {
    if (!opts || freq_hz <= 0) {
        return 0;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    if (ctx->cc_tune_request_id != 0U && ctx->cc_probe_freq_hz == freq_hz) {
        return 1;
    }
    return dmr_cc_current_frequency(opts, ctx) == freq_hz;
}

int
dmr_sm_follows(const dmr_sm_ctx_t* ctx, long freq_hz) {
    return ctx && freq_hz > 0 && ctx->state == DMR_SM_TUNED && ctx->vc_freq_hz == freq_hz;
}

void
dmr_sm_note_status_follow(const dsd_opts* opts, const dsd_state* state, long freq_hz) {
    if (!opts || !state || freq_hz <= 0) {
        return;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    /* As handle_grant() after its tune: the call's voice deadlines and its return are the machine's. Its identity is
       the channel status's, which the Capacity Plus seed publishes. */
    ctx->vc_freq_hz = freq_hz;
    ctx->vc_lpcn = 0;
    ctx->vc_tg = 0;
    ctx->vc_dst = 0;
    ctx->vc_src = 0;
    ctx->vc_slot = -1;
    ctx->vc_is_group = 1;
    ctx->vc_identity_published = 1;
    ctx->vc_status_follow = 1;
    ctx->vc_crc_flags = 0;
    ctx->t_tune_m = dsd_decode_now_mono_s();
    ctx->t_voice_m = 0.0;
    ctx->cc_acquiring = 0;
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_superseded = 0; // a call followed since an operator move ends that move's listening window
    ctx->vc_status_grant = 0;
    ctx->vc_identity_known = 0;
    ctx->vc_data_call = 0;
    dmr_sm_slots_reset(ctx); // the slots' identities are the status's seeds (dmr_sm_capplus_seed_arm())
    set_state(ctx, opts, DMR_SM_TUNED, "status-follow");
}

int
dmr_sm_cold_start_active(const dmr_sm_ctx_t* ctx) {
    return ctx && (ctx->cold_start_hunt || ctx->cold_start_armed_m > 0.0);
}

int
dmr_sm_cold_start_due(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (dmr_sm_cold_start_active(ctx)) {
        return 1;
    }
    return ctx && opts && state && ctx->state == DMR_SM_IDLE && dsd_trunk_dmr_recovery_allowed(opts, state)
           && dmr_sm_hunt_eligible(ctx, opts, state);
}

/* ============================================================================
 * Global Singleton
 * ============================================================================ */

static dmr_sm_ctx_t g_dmr_sm_ctx;
static int g_dmr_sm_initialized = 0;

dmr_sm_ctx_t*
dmr_sm_get_ctx(void) {
    dmr_sm_ctx_t* scan_ctx = (dmr_sm_ctx_t*)dsd_trunk_scan_hook_dmr_ctx();
    if (scan_ctx) {
        return scan_ctx;
    }
    if (!g_dmr_sm_initialized) {
        dmr_sm_init_ctx(&g_dmr_sm_ctx, NULL, NULL);
        g_dmr_sm_initialized = 1;
    }
    return &g_dmr_sm_ctx;
}

/* ============================================================================
 * Convenience Emit Functions
 * ============================================================================ */

void
dmr_sm_emit_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    dmr_sm_event_t ev = dmr_sm_ev_voice_sync(slot);
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &ev);
}

void
dmr_sm_emit_data_sync(dsd_opts* opts, dsd_state* state, int slot) {
    dmr_sm_event_t ev = dmr_sm_ev_data_sync(slot);
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &ev);
}

void
dmr_sm_emit_release(dsd_opts* opts, dsd_state* state, int slot) {
    dmr_sm_event_t ev = dmr_sm_ev_release(slot);
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &ev);
}

void
dmr_sm_emit_group_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int tg, int src) {
    dmr_sm_emit_group_grant_slot(opts, state, freq_hz, lpcn, -1, tg, src);
}

void
dmr_sm_emit_group_grant_slot(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int slot, int tg, int src) {
    dmr_sm_event_t ev = dmr_sm_ev_group_grant(freq_hz, lpcn, tg, src);
    ev.slot = (slot >= 0 && slot <= 1) ? slot : -1;
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &ev);
}

void
dmr_sm_emit_indiv_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int dst, int src) {
    dmr_sm_emit_indiv_grant_slot(opts, state, freq_hz, lpcn, -1, dst, src);
}

void
dmr_sm_emit_indiv_grant_slot(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int slot, int dst, int src) {
    dmr_sm_event_t ev = dmr_sm_ev_indiv_grant(freq_hz, lpcn, dst, src);
    ev.slot = (slot >= 0 && slot <= 1) ? slot : -1;
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &ev);
}

void
dmr_sm_init(const dsd_opts* opts, const dsd_state* state) {
    // Reset global flag to allow re-initialization with real opts/state.
    // This ensures user configuration (e.g., trunk_hangtime) is applied
    // even if the singleton was previously auto-initialized with NULLs.
    g_dmr_sm_initialized = 0;
    dmr_sm_init_ctx(dmr_sm_get_ctx(), opts, state);
}

/* ============================================================================
 * Neighbor/CC Candidate Management
 * ============================================================================ */

void
dmr_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    if (!state || !freqs || count <= 0) {
        return;
    }
    UNUSED(opts);

    for (int i = 0; i < count; i++) {
        long f = freqs[i];
        if (f == 0 || f == state->trunk_cc_freq) {
            continue;
        }
        (void)dsd_trunk_cc_candidates_add(state, f, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    }
}
