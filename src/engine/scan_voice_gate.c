// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/scan_voice_gate.h>

#include <math.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* Voice must have run this long before it counts: D-STAR and ProVoice run the
 * vocoder before their confirm, DMR/NXDN/dPMR are already confirm-gated. */
#define DSD_SCAN_VOICE_MIN_SPAN_S         0.10

#define DSD_SCAN_VOICE_DEFAULT_QUALIFY_MS 1000
#define DSD_SCAN_VOICE_DEFAULT_HOLD_MS    2000

static int
scan_voice_gate_enabled(const dsd_opts* opts) {
    return opts && opts->scan_voice_only == 1;
}

static int
scan_voice_resolve_ms(int configured, int fallback) {
    if (configured <= 0) {
        return fallback;
    }
    return configured;
}

static int
scan_voice_snapshot_is_voice(const dsd_call_snapshot* call) {
    if (!call || (call->phase != DSD_CALL_PHASE_ACTIVE && call->phase != DSD_CALL_PHASE_ENDED)) {
        return 0;
    }
    if (call->kind == DSD_CALL_KIND_DATA) {
        return 0;
    }
    if (call->media_started_m <= 0.0 || call->media_updated_m <= 0.0) {
        return 0;
    }
    double span_started_m = call->media_started_m;
    /* A recoverable reopen retains the logical transmission's media clocks but
     * starts a new segment. Qualify from the later boundary so one post-gap
     * vocoder frame cannot borrow the pre-gap media span. */
    if (call->started_m > span_started_m) {
        span_started_m = call->started_m;
    }
    if ((call->media_updated_m - span_started_m) < DSD_SCAN_VOICE_MIN_SPAN_S) {
        return 0;
    }
    return 1;
}

static int
scan_voice_snapshot_policy_allows(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call) {
    const uint64_t target = call->policy_target_id != 0U ? call->policy_target_id : call->ota_target_id;
    if (target == 0U) {
        /* Unknown identity (LC never decoded) counts as voice. */
        return 1;
    }
    const int encrypted =
        call->crypto == DSD_CALL_CRYPTO_ENCRYPTED || call->crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING;
    dsd_tg_policy_decision decision;
    int rc = 0;
    if (call->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        rc = dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)call->ota_source_id, (uint32_t)target,
                                                 encrypted, 0, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)target, (uint32_t)call->ota_source_id, encrypted,
                                               0, &decision);
    }
    return rc == 0 && decision.tune_allowed;
}

int
dsd_scan_voice_probe(const dsd_opts* opts, const dsd_state* state, dsd_scan_voice_probe_result* out) {
    if (!out) {
        return -1;
    }
    out->active_media_m = -1.0;
    out->retained_media_m = -1.0;
    if (!opts || !state) {
        return -1;
    }
    for (int slot_i = 0; slot_i < DSD_CALL_STATE_SLOT_COUNT; slot_i++) {
        dsd_call_snapshot call;
        /* dsd_call_state_get() fills the whole snapshot on success; a failed get skips the slot. */
        if (dsd_call_state_get(state, (uint8_t)slot_i, &call) <= 0) {
            continue;
        }
        if (!scan_voice_snapshot_is_voice(&call)) {
            continue;
        }
        if (!scan_voice_snapshot_policy_allows(opts, state, &call)) {
            continue;
        }
        if (call.media_updated_m > out->retained_media_m) {
            out->retained_media_m = call.media_updated_m;
        }
        if (call.phase == DSD_CALL_PHASE_ACTIVE && call.media_active && call.media_updated_m > out->active_media_m) {
            out->active_media_m = call.media_updated_m;
        }
    }
    return out->retained_media_m >= 0.0 ? 1 : 0;
}

void
dsd_scan_voice_gate_note_retune(dsd_state* state, double now_m) {
    if (!state) {
        return;
    }
    state->scan_voice_gate_arrive_m = now_m;
    state->scan_voice_gate_sync_m = -1.0;
    state->scan_voice_gate_voice_m = -1.0;
    state->scan_voice_gate_roll_seen = state->lcn_freq_roll;
    state->scan_voice_gate_hold_seen = state->lcn_scan_hold ? 1U : 0U;
    state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY;
}

/* Re-open the per-visit window when the row changed underneath the tick or the
 * operator released a hold. */
static void
scan_voice_gate_track_visit(dsd_state* state, double now_m) {
    /* An external lcn_freq_roll change (avoid, `L` cycle) restarts the visit so
     * the new row gets a full qualify window. */
    if (state->lcn_freq_roll != state->scan_voice_gate_roll_seen) {
        dsd_scan_voice_gate_note_retune(state, now_m);
    }
    /* An operator hold release grants a fresh qualify window rather than a hop on
     * the very next tick, mirroring the mark_cc_sync() the release gives the
     * legacy hangtime rule. */
    const uint8_t hold_now = state->lcn_scan_hold ? 1U : 0U;
    if (state->scan_voice_gate_hold_seen && !hold_now) {
        dsd_scan_voice_gate_note_retune(state, now_m);
    }
    state->scan_voice_gate_hold_seen = hold_now;
}

/* Voice while parked, tail once the media stops, qualify until either. */
static void
scan_voice_gate_publish_phase(dsd_state* state, int media_in_visit) {
    if (media_in_visit) {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_VOICE;
    } else if (state->scan_voice_gate_voice_m >= 0.0) {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_TAIL;
    } else {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY;
    }
}

void
dsd_scan_voice_gate_tick(const dsd_opts* opts, dsd_state* state, int synced, double now_m) {
    if (!opts || !state) {
        return;
    }
    /* The phase field is owned by whichever scanner is running: under --trunk-scan
     * the coordinator publishes it per target (trunk_scan.c), so the -Y tick must
     * not touch it. */
    if (opts->scanner_mode != 1) {
        return;
    }
    if (!scan_voice_gate_enabled(opts)) {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_OFF;
        return;
    }
    scan_voice_gate_track_visit(state, now_m);
    if (state->scan_voice_gate_arrive_m < 0.0) {
        state->scan_voice_gate_arrive_m = now_m;
    }
    if (state->scan_voice_gate_sync_m < 0.0 && synced) {
        state->scan_voice_gate_sync_m = now_m;
    }
    dsd_scan_voice_probe_result media;
    const int probe_rc = dsd_scan_voice_probe(opts, state, &media);
    const int retained_in_visit = probe_rc > 0 && media.retained_media_m >= state->scan_voice_gate_arrive_m;
    const int active_in_visit = media.active_media_m >= 0.0 && media.active_media_m >= state->scan_voice_gate_arrive_m;
    if (retained_in_visit && media.retained_media_m > state->scan_voice_gate_voice_m) {
        state->scan_voice_gate_voice_m = media.retained_media_m;
    }
    scan_voice_gate_publish_phase(state, active_in_visit);
}

int
dsd_scan_voice_gate_owns_step(const dsd_opts* opts, const dsd_state* state) {
    return scan_voice_gate_enabled(opts) && state
           && (state->scan_voice_gate_voice_m >= 0.0 || state->scan_voice_gate_sync_m >= 0.0);
}

int
dsd_scan_voice_gate_should_step(const dsd_opts* opts, const dsd_state* state, double now_m) {
    if (!dsd_scan_voice_gate_owns_step(opts, state)) {
        return 0;
    }
    if (state->lcn_scan_hold) {
        return 0;
    }
    if (state->scan_voice_gate_voice_m >= 0.0) {
        const int hold_ms = scan_voice_resolve_ms(opts->scan_voice_hold_ms, DSD_SCAN_VOICE_DEFAULT_HOLD_MS);
        return (now_m - state->scan_voice_gate_voice_m) >= ((double)hold_ms / 1000.0);
    }
    if (state->scan_voice_gate_sync_m >= 0.0) {
        const int qualify_ms = scan_voice_resolve_ms(opts->scan_voice_qualify_ms, DSD_SCAN_VOICE_DEFAULT_QUALIFY_MS);
        return (now_m - state->scan_voice_gate_sync_m) >= ((double)qualify_ms / 1000.0);
    }
    /* Never synced this visit: the caller falls back to the legacy hangtime rule. */
    return 0;
}

void
dsd_scan_timing_clear(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(&state->scan_timing, 0, sizeof(state->scan_timing));
    state->scan_timing.started_m = -1.0;
    state->scan_timing.deadline_m = -1.0;
}

void
dsd_scan_timing_publish(dsd_state* state, const dsd_scan_timing_publication* report) {
    if (!state || !report) {
        return;
    }
    state->scan_timing = *report;
}

/* Seed the -Y report. The effective windows are the row's, not the live timer's: they
 * stay visible while a hold pauses the countdown, and are 0 when the voice gate is off
 * because the legacy hangtime rule has neither a qualify nor a hold window. */
static void
scan_y_timing_seed(const dsd_opts* opts, dsd_scan_timing_publication* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->started_m = -1.0;
    out->deadline_m = -1.0;
    out->conventional = 1U;
    if (!scan_voice_gate_enabled(opts)) {
        return;
    }
    out->dwell_ms = (uint32_t)scan_voice_resolve_ms(opts->scan_voice_qualify_ms, DSD_SCAN_VOICE_DEFAULT_QUALIFY_MS);
    out->hold_ms = (uint32_t)scan_voice_resolve_ms(opts->scan_voice_hold_ms, DSD_SCAN_VOICE_DEFAULT_HOLD_MS);
}

/* Anti-drift: dsd_scan_voice_gate_should_step() flips at anchor + span_ms / 1000.0, so
 * the published deadline is that same expression rather than a second approximation. */
static void
scan_y_timing_arm(dsd_scan_timing_publication* out, double started_m, uint32_t span_ms) {
    out->started_m = started_m;
    out->deadline_m = started_m + ((double)span_ms / 1000.0);
    out->span_ms = span_ms;
}

/* The gate owns the step: voice (or its tail) counts down the hold window from the last
 * media frame, and a synced-but-silent visit counts down the qualify window. */
static void
scan_y_timing_fill_gate(const dsd_state* state, dsd_scan_timing_publication* out) {
    if (state->scan_voice_gate_voice_m >= 0.0) {
        out->reason = state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_VOICE
                          ? (uint8_t)DSD_SCAN_STAY_VOICE
                          : (uint8_t)DSD_SCAN_STAY_ACTIVITY_HOLD;
        scan_y_timing_arm(out, state->scan_voice_gate_voice_m, out->hold_ms);
        return;
    }
    /* dsd_scan_voice_gate_owns_step() is true and no voice anchor exists, so the sync
     * anchor is the one that is set. */
    out->reason = (uint8_t)DSD_SCAN_STAY_IDLE_DWELL;
    scan_y_timing_arm(out, state->scan_voice_gate_sync_m, out->dwell_ms);
}

/* -t parses any non-negative double, so the second-to-millisecond conversion has to
 * saturate rather than wrap on its way into the publication's uint32_t. */
static uint32_t
scan_y_timing_span_ms(double seconds) {
    const double span_ms = seconds * 1000.0;
    if (span_ms >= (double)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)lround(span_ms);
}

/* The legacy rule waits out -t since the last sync and knows nothing else, so it reports
 * no dwell and no hold. Without an anchor or with -t 0 there is nothing to count down. */
static void
scan_y_timing_fill_hangtime(const dsd_opts* opts, const dsd_state* state, dsd_scan_timing_publication* out) {
    out->reason = (uint8_t)DSD_SCAN_STAY_HANGTIME;
    out->dwell_ms = 0U;
    out->hold_ms = 0U;
    if (state->last_cc_sync_time_m <= 0.0 || opts->trunk_hangtime <= 0.0f) {
        return;
    }
    out->started_m = state->last_cc_sync_time_m;
    out->deadline_m = out->started_m + (double)opts->trunk_hangtime;
    out->span_ms = scan_y_timing_span_ms((double)opts->trunk_hangtime);
}

void
dsd_engine_scan_y_timing_tick(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    /* Under --trunk-scan the coordinator publishes for its parked target; the two must
     * never both write the field. With no scanner running there is nothing to report. */
    if (opts->scanner_mode != 1 || opts->trunk_scan_enabled == 1) {
        return;
    }
    dsd_scan_timing_publication report;
    scan_y_timing_seed(opts, &report);
    if (state->lcn_scan_hold) {
        /* The rotation is parked by the operator: the window is paused, not expired.
         * dsd_scan_voice_gate_should_step() returns 0 here and the no-carrier step
         * returns early, so publishing a deadline would count down to a hop that the
         * release, not the clock, actually causes. */
        report.reason = (uint8_t)DSD_SCAN_STAY_MANUAL_HOLD;
    } else if (dsd_scan_voice_gate_owns_step(opts, state)) {
        scan_y_timing_fill_gate(state, &report);
    } else {
        scan_y_timing_fill_hangtime(opts, state, &report);
    }
    dsd_scan_timing_publish(state, &report);
}
