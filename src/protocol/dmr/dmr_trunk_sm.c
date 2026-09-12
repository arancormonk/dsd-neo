// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * DMR Tier III trunking state machine - event-driven, tick-based.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);

#define DMR_CC_PROBE_GRACE_S  2.0
#define DMR_CC_TUNE_TIMEOUT_S 5.0

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
    dmr_sm_init_ctx(ctx, opts, state);
    ctx->cc_freq_hz = freq_hz;
    ctx->cc_probe_freq_hz = freq_hz;
    ctx->cc_acquiring = 1;
    ctx->cc_acquire_start_m = dsd_time_now_monotonic_s();
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

static long
dmr_cc_current_frequency(const dsd_opts* opts, const dmr_sm_ctx_t* ctx) {
    if (ctx && ctx->cc_rx_freq_hz > 0 && ctx->cc_rx_generation == dsd_trunk_tuning_generation()) {
        return ctx->cc_rx_freq_hz;
    }
    if (opts->use_rigctl == 1) {
        return dsd_rigctl_query_hook_get_current_freq_hz(opts);
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

static dmr_sm_ctx_t*
dmr_cc_activity_context(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable != 1 || opts->trunk_is_tuned == 1) {
        return NULL;
    }
    if (opts->trunk_scan_enabled && !dsd_trunk_scan_hook_dmr_ctx()) {
        return NULL;
    }
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    if (!dmr_cc_resolve_pending(ctx, opts, dsd_time_now_monotonic_s())) {
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
    ctx->cc_confirmed = 1;
    ctx->cc_acquiring = 0;
    ctx->cc_retry_after_m = 0.0;
    ctx->t_cc_sync_m = dsd_time_now_monotonic_s();
    state->trunk_cc_freq = current;
    state->last_cc_sync_time = time(NULL);
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
        || !dmr_cc_resolve_pending(ctx, opts, dsd_time_now_monotonic_s())) {
        return;
    }
    /* A heartbeat is in the decode hot path. Attribute it to the completed
     * tuning boundary, without a blocking rigctl round trip per CSBK. */
    if (ctx->cc_rx_freq_hz == ctx->cc_freq_hz && dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation)) {
        ctx->t_cc_sync_m = dsd_time_now_monotonic_s();
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
    for (int tries = 0; tries < state->lcn_freq_count; ++tries) {
        if (ctx->cc_hunt_index >= state->lcn_freq_count || ctx->cc_hunt_index < 0) {
            ctx->cc_hunt_index = 0;
        }
        const int index = ctx->cc_hunt_index++;
        const long freq = *dsd_state_trunk_lcn_slot_const(state, index);
        if (freq <= 0 || dsd_state_trunk_lcn_avoid_get(state, (size_t)index) || freq == ctx->cc_probe_freq_hz) {
            continue;
        }
        /* Adjacent LSNs often name the same RF channel. */
        while (ctx->cc_hunt_index < state->lcn_freq_count
               && *dsd_state_trunk_lcn_slot_const(state, ctx->cc_hunt_index) == freq) {
            ++ctx->cc_hunt_index;
        }
        return freq;
    }
    return 0; // No eligible alternative: keep listening without a retune.
}

static long
dmr_cc_next_candidate(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    const long anchor = ctx->cc_freq_hz > 0 ? ctx->cc_freq_hz : state->trunk_cc_freq;
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
    if (dsd_trunk_cc_candidates_next(state, dsd_time_now_monotonic_s(), DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE,
                                     &candidate)) {
        return candidate;
    }
    return anchor;
}

static void
dmr_cc_hunt(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!dsd_trunk_dmr_recovery_allowed(opts, state) || opts->trunk_is_tuned == 1 || ctx->cc_retry_after_m > now_m) {
        return;
    }
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
    ctx->cc_acquire_start_m = dsd_time_now_monotonic_s();
    ctx->cc_tune_request_id = request_id;
    ctx->cc_tune_deadline_m = ctx->cc_acquire_start_m + DMR_CC_TUNE_TIMEOUT_S;
    set_state(ctx, opts, DMR_SM_ON_CC, "cc-probe");
    (void)dmr_cc_resolve_pending(ctx, opts, ctx->cc_acquire_start_m);
    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n[DMR SM] CC probe %.6lf MHz; acquisition %.2f s\n", (double)freq / 1000000.0,
                    DMR_CC_PROBE_GRACE_S);
    }
}

/* ============================================================================
 * Release to CC
 * ============================================================================ */

static void
dmr_sm_release_without_cc(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (state) {
        const double ended_m = dsd_time_now_monotonic_s();
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
dmr_sm_clear_released_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int cc_returned,
                              dmr_sm_state_e next_state, const char* reason) {
    for (int s = 0; s < 2; s++) {
        ctx->slots[s].voice_active = 0;
        ctx->slots[s].last_active_m = 0.0;
        ctx->slots[s].tg = 0;
    }

    ctx->vc_freq_hz = 0;
    ctx->vc_lpcn = 0;
    ctx->vc_tg = 0;
    ctx->vc_dst = 0;
    ctx->vc_src = 0;
    ctx->vc_slot = -1;
    ctx->vc_is_group = 0;
    ctx->vc_identity_published = 0;
    ctx->t_tune_m = 0.0;
    ctx->t_voice_m = 0.0;

    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        state->dmr_mono_slot = 0;
        state->p25_sm_release_count++;
    }

    if (!cc_returned) {
        dmr_sm_release_without_cc(ctx, opts, state);
    }
    set_state(ctx, opts, next_state, reason);
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

    const long cc = state && state->trunk_cc_freq > 0 ? state->trunk_cc_freq : ctx->cc_freq_hz;
    dsd_trunk_tune_result tune_result = cc > 0 ? dmr_sm_return_to_cc(ctx, opts, state, cc) : DSD_TRUNK_TUNE_RESULT_OK;
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        sm_log(opts, "release-tune-deferred");
        if (state && had_force_release) {
            state->trunk_sm_force_release = 1;
        }
        return;
    }

    dmr_sm_clear_released_carrier(ctx, opts, state, cc > 0, cc > 0 ? DMR_SM_ON_CC : DMR_SM_HUNTING, reason);
}

void
dmr_sm_abandon_carrier(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    if (!ctx) {
        return;
    }
    if (state) {
        /* The teardown the latch asks for is happening right here; left armed, the next tick
         * would try its own release, and that one tunes back to a control channel -- against a
         * caller that is already moving the tuner. */
        state->trunk_sm_force_release = 0;
    }
    sm_log(opts, reason);
    /* ON_CC, not HUNTING: nothing about this release says the control channel was lost, and a
     * revisit expects to find the target resting exactly where an ordinary release leaves it. */
    dmr_sm_clear_released_carrier(ctx, opts, state, 0, DMR_SM_ON_CC, reason ? reason : "abandon-carrier");
}

static int
dmr_sm_resolve_tunable_grant(const dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev,
                             long* out_freq) {
    if (opts->trunk_enable != 1 || state->trunk_cc_freq == 0 || ctx->cc_tune_request_id != 0U) {
        return 0;
    }

    long freq = resolve_freq(state, ev->freq_hz, ev->lpcn);
    if (freq <= 0) {
        sm_log(opts, "grant-no-freq");
        return 0;
    }

    if (ev->freq_hz <= 0 && ev->lpcn > 0 && !lpcn_is_trusted(opts, state, ev->lpcn)) {
        return 0;
    }

    if (ctx->state == DMR_SM_TUNED && ctx->vc_freq_hz == freq) {
        sm_log(opts, "grant-same-freq");
        return 0;
    }

    *out_freq = freq;
    return 1;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

static void
handle_grant(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    if (!ctx || !ev || !opts || !state) {
        return;
    }
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_DMR);

    long freq = 0;
    if (!dmr_sm_resolve_tunable_grant(ctx, opts, state, ev, &freq)) {
        return;
    }

    /* A validated grant on a completed probe proves where to return, even
     * before the next ALOHA/system-parameters message arrives. */
    if (ctx->cc_acquiring && opts->trunk_is_tuned == 0 && dsd_trunk_tuning_frame_is_current(ctx->cc_rx_generation)) {
        dmr_cc_note_activity_ctx(ctx, opts, state, ctx->cc_probe_freq_hz);
    }
    double now_m = dsd_time_now_monotonic_s();

    dsd_trunk_tune_result tune_result =
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, 0, NULL); // DMR: no TED SPS override
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        sm_log(opts, "grant-tune-deferred");
        return;
    }

    ctx->vc_freq_hz = freq;
    ctx->vc_lpcn = ev->lpcn;
    ctx->vc_tg = ev->tg;
    ctx->vc_dst = ev->dst;
    ctx->vc_src = ev->src;
    ctx->vc_slot = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : -1;
    ctx->vc_is_group = ev->is_group != 0;
    ctx->vc_identity_published = 0;
    ctx->t_tune_m = now_m;
    ctx->t_voice_m = 0.0;
    ctx->cc_acquiring = 0;
    ctx->cc_tune_request_id = 0U;
    state->dmr_mono_slot = ctx->vc_slot;

    for (int s = 0; s < 2; s++) {
        ctx->slots[s].voice_active = 0;
        ctx->slots[s].last_active_m = 0.0;
        ctx->slots[s].tg = 0;
    }

    dmr_reset_blocks(opts, state);

    state->last_t3_tune_time_m = now_m;
    state->p25_sm_tune_count++;

    if (sm_status_log_enabled(opts)) {
        DSD_FPRINTF(stderr, "\n  DMR SM: Tune VC freq=%.6lf MHz\n", (double)freq / 1000000.0);
    }

    set_state(ctx, opts, DMR_SM_TUNED, "grant");
}

static void
update_voice_sync_state(dmr_sm_ctx_t* ctx, dsd_state* state, int slot, double now_m) {
    if (!state) {
        return;
    }

    state->last_vc_sync_time_m = now_m;
    if (ctx->state == DMR_SM_TUNED && ctx->vc_slot < 0) {
        ctx->vc_slot = slot;
        state->dmr_mono_slot = slot;
    }
}

static void
handle_voice_sync(dmr_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx) {
        return;
    }

    double now_m = dsd_time_now_monotonic_s();
    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    ctx->slots[s].voice_active = 1;
    ctx->slots[s].last_active_m = now_m;
    ctx->t_voice_m = now_m;

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
        if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) >= 0) {
            ctx->vc_identity_published = 1;
        }
    }

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

    double now_m = dsd_time_now_monotonic_s();
    int s = (slot >= 0 && slot <= 1) ? slot : 0;

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
tick_on_cc(dmr_sm_ctx_t* ctx, const dsd_opts* opts, double now_m, double cc_grace) {
    const double since = ctx->cc_acquiring ? ctx->cc_acquire_start_m : ctx->t_cc_sync_m;
    if (since <= 0.0) {
        return;
    }
    if ((now_m - since) > cc_grace) {
        set_state(ctx, opts, DMR_SM_HUNTING, "cc-lost");
    }
}

static void
clear_stale_voice_slots(dmr_sm_ctx_t* ctx, double now_m) {
    const double voice_stale_threshold = 0.2; // DMR voice frames are ~60ms; 200ms is conservative.

    for (int s = 0; s < 2; s++) {
        if (ctx->slots[s].voice_active && ctx->slots[s].last_active_m > 0.0) {
            if ((now_m - ctx->slots[s].last_active_m) > voice_stale_threshold) {
                ctx->slots[s].voice_active = 0;
            }
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

static void
tick_tuned(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double hangtime, double grant_timeout) {
    if (state && state->trunk_sm_force_release != 0) {
        do_release(ctx, opts, state, "release-forced");
        return;
    }

    clear_stale_voice_slots(ctx, now_m);

    if (has_voice_activity(ctx)) {
        ctx->t_voice_m = now_m;
        return;
    }

    if (hangtime_expired(ctx, now_m, hangtime)) {
        do_release(ctx, opts, state, "hangtime-expired");
        return;
    }

    if (ctx->t_voice_m <= 0.0 && grant_timeout_expired(ctx, now_m, grant_timeout)) {
        do_release(ctx, opts, state, "grant-timeout");
    }
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
        ctx->t_cc_sync_m = dsd_time_now_monotonic_s();
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

void
dmr_sm_tick_ctx(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx || !dsd_trunk_dmr_recovery_allowed(opts, state)) {
        return;
    }

    if (!ctx->initialized) {
        dmr_sm_init_ctx(ctx, opts, state);
    }

    double now_m = dsd_time_now_monotonic_s();
    double hangtime = ctx->hangtime_s;
    double grant_timeout = ctx->grant_timeout_s;
    double cc_grace = ctx->cc_acquiring ? DMR_CC_PROBE_GRACE_S : ctx->cc_grace_s;
    if (!dmr_cc_resolve_pending(ctx, opts, now_m)) {
        return;
    }

    switch (ctx->state) {
        case DMR_SM_IDLE: break;

        case DMR_SM_ON_CC:
            if (!opts || opts->trunk_is_tuned != 1) {
                tick_on_cc(ctx, opts, now_m, cc_grace);
            }
            break;

        case DMR_SM_TUNED: tick_tuned(ctx, opts, state, now_m, hangtime, grant_timeout); break;

        case DMR_SM_HUNTING: dmr_cc_hunt(ctx, opts, state, now_m); break;
    }
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
