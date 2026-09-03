// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/scan_voice_gate.h>

#include <stddef.h>
#include <stdint.h>

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
    if (!call || call->phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    if (!call->media_active) {
        return 0;
    }
    if (call->kind == DSD_CALL_KIND_DATA) {
        return 0;
    }
    if ((call->updated_m - call->started_m) < DSD_SCAN_VOICE_MIN_SPAN_S) {
        return 0;
    }
    return 1;
}

double
dsd_scan_voice_probe(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return -1.0;
    }
    double newest = -1.0;
    for (int slot_i = 0; slot_i < DSD_CALL_STATE_SLOT_COUNT; slot_i++) {
        dsd_call_snapshot call;
        /* dsd_call_state_get() fills the whole snapshot on success; a failed get skips the slot. */
        if (dsd_call_state_get(state, (uint8_t)slot_i, &call) <= 0) {
            continue;
        }
        if (!scan_voice_snapshot_is_voice(&call)) {
            continue;
        }
        uint64_t target = call.policy_target_id != 0U ? call.policy_target_id : call.ota_target_id;
        if (target != 0U) {
            const int encrypted =
                (call.crypto == DSD_CALL_CRYPTO_ENCRYPTED || call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
            dsd_tg_policy_decision decision;
            int rc = 0;
            if (call.kind == DSD_CALL_KIND_PRIVATE_VOICE) {
                rc = dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)call.ota_source_id, (uint32_t)target,
                                                         encrypted, 0, &decision);
            } else {
                rc = dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)target, (uint32_t)call.ota_source_id,
                                                       encrypted, 0, &decision);
            }
            if (rc != 0 || !decision.tune_allowed) {
                continue;
            }
        }
        /* Unknown identity (LC never decoded) counts as voice. */
        if (call.updated_m > newest) {
            newest = call.updated_m;
        }
    }
    return newest;
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
        state->scan_voice_gate_sync_m = -1.0;
        state->scan_voice_gate_voice_m = -1.0;
    }
    state->scan_voice_gate_hold_seen = hold_now;
    if (state->scan_voice_gate_arrive_m < 0.0) {
        state->scan_voice_gate_arrive_m = now_m;
    }
    if (state->scan_voice_gate_sync_m < 0.0 && synced) {
        state->scan_voice_gate_sync_m = now_m;
    }
    const double media_m = dsd_scan_voice_probe(opts, state);
    const int media_in_visit = media_m >= 0.0 && media_m >= state->scan_voice_gate_arrive_m;
    if (media_in_visit && media_m > state->scan_voice_gate_voice_m) {
        state->scan_voice_gate_voice_m = media_m;
    }
    if (media_in_visit) {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_VOICE;
    } else if (state->scan_voice_gate_voice_m >= 0.0) {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_TAIL;
    } else {
        state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY;
    }
}

int
dsd_scan_voice_gate_should_step(const dsd_opts* opts, const dsd_state* state, double now_m) {
    if (!scan_voice_gate_enabled(opts) || !state) {
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
