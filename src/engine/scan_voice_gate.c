// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_patch_policy.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>

#include <math.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "scan_landing.h"
#include "scan_owner.h"
#include "scan_quiet.h"

/* Checked voice must have run this long before it counts: D-STAR and ProVoice run the
 * vocoder before their confirm, DMR/NXDN/dPMR are already confirm-gated. */
#define DSD_SCAN_VOICE_MIN_SPAN_S         0.10

#define DSD_SCAN_VOICE_DEFAULT_QUALIFY_MS 1000
#define DSD_SCAN_VOICE_DEFAULT_HOLD_MS    2000

/* The gate waits for decoded voice, which an analog row never produces: its carrier holds it instead. */
static int
scan_voice_gate_enabled(const dsd_opts* opts) {
    return opts && opts->scan_voice_only == 1 && !dsd_opts_is_analog_family(opts);
}

/* The analog carrier on air, before the tone policy has its say: dsd_scan_analog_carrier_open() without the verdict. */
static int
scan_analog_carrier_heard(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable == 1 || state->carrier != 0 || !dsd_analog_monitor_tap_active(opts)) {
        return 0;
    }
    /* The tap's carrier, and only for the channel the receiver is on now: a read from before a retune says
       nothing about the new row. */
    if (!dsd_analog_rx_carrier_open_now(opts, state)) {
        return 0;
    }
    /* An input that stopped delivering leaves its last word published; past its deadline, on the clock the tap set it
       on, the pause has outlasted the hangover (rx_tone_view.c reads it the same way). */
    return !dsd_analog_rx_stale_now(state);
}

int
dsd_scan_analog_tone_gate(const dsd_opts* opts, const dsd_state* state) {
    if (!scan_analog_carrier_heard(opts, state)) {
        return DSD_ANALOG_TONE_GATE_OFF;
    }
    return dsd_analog_tone_gate_in_force(opts, state);
}

int
dsd_scan_analog_tone_rejection_ended(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->trunk_enable == 1 || state->carrier != 0 || !dsd_analog_monitor_tap_active(opts)
        || scan_analog_carrier_heard(opts, state)) {
        return 0;
    }
    if (dsd_analog_rx_carrier_open_now(opts, state)) {
        /* Published open but no longer heard: an input that stopped delivering, past its deadline. A rejection it left
           published ended with it. */
        return dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_REJECTED;
    }
    return dsd_analog_rx_rejection_ended_now(opts, state);
}

int
dsd_scan_analog_carrier_open(const dsd_opts* opts, const dsd_state* state) {
    /* Only traffic the tone policy passes is activity (issue #527): a rejected carrier holds nothing, and one still
       being checked holds the row only through its verdict (dsd_scan_analog_tone_gate()), leaving no tail. */
    return scan_analog_carrier_heard(opts, state)
           && dsd_analog_tone_gate_passes(dsd_analog_tone_gate_in_force(opts, state));
}

static int
scan_voice_resolve_ms(int configured, int fallback) {
    if (configured <= 0) {
        return fallback;
    }
    return configured;
}

/* Whether @p call carries voice the scan may credit (issue #890): checked media, the frames its decoder output as speech
   successfully and unconcealed (dsd_call_snapshot::checked_media_started_m), never the media mark a frame gets before it
   is decoded, and only media read on the scan visit and carrier in force now (dsd_state_scan_observation_is_current()):
   a call another row's visit or a tune still in flight heard, ended or not, is no voice of this one, whatever its
   decode times say. Media earned on the visit before a sync loss stays the visit's. */
static int
scan_voice_snapshot_is_voice(const dsd_state* state, const dsd_call_snapshot* call) {
    if (!call || (call->phase != DSD_CALL_PHASE_ACTIVE && call->phase != DSD_CALL_PHASE_ENDED)) {
        return 0;
    }
    if (call->kind == DSD_CALL_KIND_DATA) {
        return 0;
    }
    if (call->checked_media_started_m <= 0.0 || call->checked_media_updated_m <= 0.0) {
        return 0;
    }
    const dsd_scan_observation_origin origin = {call->checked_media_visit, call->checked_media_carrier_seq};
    if (!dsd_state_scan_observation_is_current(state, &origin)) {
        return 0;
    }
    double span_started_m = call->checked_media_started_m;
    /* A recoverable reopen retains the logical transmission's media clocks but
     * starts a new segment. Qualify from the later boundary so one post-gap
     * vocoder frame cannot borrow the pre-gap media span. */
    if (call->started_m > span_started_m) {
        span_started_m = call->started_m;
    }
    if ((call->checked_media_updated_m - span_started_m) < DSD_SCAN_VOICE_MIN_SPAN_S) {
        return 0;
    }
    return 1;
}

int
dsd_scan_activity_protocol_is_callsign(int protocol) {
    return DSD_SYNC_IS_YSF(protocol) || DSD_SYNC_IS_DSTAR(protocol) || DSD_SYNC_IS_M17(protocol);
}

int
dsd_scan_mode_family_has_protocol(int mode, int protocol) {
    switch ((dsd_scan_mode)mode) {
        case DSD_SCAN_MODE_INHERIT: return 1;
        case DSD_SCAN_MODE_P25: return DSD_SYNC_IS_P25(protocol);
        case DSD_SCAN_MODE_DMR: return DSD_SYNC_IS_DMR(protocol);
        case DSD_SCAN_MODE_NXDN96:
        case DSD_SCAN_MODE_NXDN48: return DSD_SYNC_IS_NXDN(protocol);
        case DSD_SCAN_MODE_DPMR: return DSD_SYNC_IS_DPMR(protocol);
        case DSD_SCAN_MODE_DSTAR: return DSD_SYNC_IS_DSTAR(protocol);
        case DSD_SCAN_MODE_YSF: return DSD_SYNC_IS_YSF(protocol);
        case DSD_SCAN_MODE_M17: return DSD_SYNC_IS_M17(protocol);
        case DSD_SCAN_MODE_TETRA_DMO: return DSD_SYNC_IS_TETRA(protocol);
        case DSD_SCAN_MODE_NFM:
        case DSD_SCAN_MODE_AM:
        default: return 0;
    }
}

/* The restrictions on a call's data, encryption and party that hold whatever its IDs: a field a restriction in force
   depends on and the report does not know holds nothing (issue #890 F9), and a known one is judged by the restriction's
   own option. */
static int
scan_activity_restrictions_allow(const dsd_opts* opts, const dsd_trunk_scan_activity_identity* id) {
    const int data_restricted = opts->trunk_tune_data_calls == 0;
    const int party_restricted = opts->trunk_tune_private_calls == 0 || opts->trunk_tune_group_calls == 0;
    if (((id->known & DSD_SCAN_ACTIVITY_KNOWN_DATA) == 0U && data_restricted)
        || ((id->known & DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED) == 0U && opts->trunk_tune_enc_calls == 0)
        || ((id->known & DSD_SCAN_ACTIVITY_KNOWN_PARTY) == 0U && party_restricted)) {
        return 0;
    }
    if ((id->known & DSD_SCAN_ACTIVITY_KNOWN_DATA) && id->data_call && data_restricted) {
        return 0;
    }
    if ((id->known & DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED) && dsd_tg_policy_encrypted_tune_denied(opts, id->encrypted)) {
        return 0;
    }
    if (id->known & DSD_SCAN_ACTIVITY_KNOWN_PARTY) {
        return id->is_private ? opts->trunk_tune_private_calls != 0 : opts->trunk_tune_group_calls != 0;
    }
    return 1;
}

/* A numeric family's identity through the talkgroup and radio lists (dsd_scan_activity_policy_allows()). */
static int
scan_activity_numeric_allows(const dsd_opts* opts, const dsd_state* state,
                             const dsd_trunk_scan_activity_identity* identity) {
    const uint32_t source = (identity->known & DSD_SCAN_ACTIVITY_KNOWN_SOURCE) ? identity->source : 0U;
    if ((identity->known & DSD_SCAN_ACTIVITY_KNOWN_TARGET) == 0U || identity->target == 0U) {
        /* An anonymous report never passes a talkgroup Hold or the allow list (fail closed); the radio list acts once
           the radio is known. */
        return state->tg_hold == 0U && opts->trunk_use_allow_list != 1 && !dsd_tg_policy_radio_blocked(state, source);
    }
    const int encrypted = (identity->known & DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED) ? identity->encrypted : 0;
    const int data_call = (identity->known & DSD_SCAN_ACTIVITY_KNOWN_DATA) ? identity->data_call : 0;
    dsd_tg_policy_decision decision;
    int rc = 0;
    if ((identity->known & DSD_SCAN_ACTIVITY_KNOWN_PARTY) && identity->is_private) {
        rc =
            dsd_tg_policy_evaluate_private_call(opts, state, source, identity->target, encrypted, data_call, &decision);
    } else {
        /* A group call, or one whose party the protocol does not say (dPMR, TS 102 658 A.1.3.3.5): by its target's
           talkgroup row and its source's radio row. */
        rc = dsd_tg_policy_evaluate_group_call(opts, state, identity->target, source, encrypted, data_call, &decision);
    }
    return rc == 0 && decision.tune_allowed;
}

int
dsd_scan_activity_policy_allows(const dsd_opts* opts, const dsd_state* state, int protocol,
                                const dsd_trunk_scan_activity_identity* identity) {
    if (!opts || !state || !identity || identity->receive_refused != 0U
        || !scan_activity_restrictions_allow(opts, identity)) {
        return 0;
    }
    return dsd_scan_activity_protocol_is_callsign(protocol) || scan_activity_numeric_allows(opts, state, identity);
}

/* What the policy table reads of @p call (issue #890): the numeric IDs of a family named by them, the party its kind
   says, its crypto classification (a callsign family with no encryption, YSF or D-STAR, is clear), and voice. A call
   whose decoder could decrypt it is judged clear, as its audio is. */
static void
scan_voice_call_identity(const dsd_call_snapshot* call, dsd_trunk_scan_activity_identity* id) {
    DSD_MEMSET(id, 0, sizeof(*id));
    id->known = DSD_SCAN_ACTIVITY_KNOWN_DATA;
    if (!dsd_scan_activity_protocol_is_callsign(call->protocol)) {
        const uint64_t wide_target = call->policy_target_id != 0U ? call->policy_target_id : call->ota_target_id;
        if (wide_target != 0U && wide_target <= UINT32_MAX) {
            id->target = (uint32_t)wide_target;
            id->known |= DSD_SCAN_ACTIVITY_KNOWN_TARGET;
        }
        /* A source that does not fit 32 bits is unknown, not its truncated low half. */
        if (call->ota_source_id != 0U && call->ota_source_id <= UINT32_MAX) {
            id->source = (uint32_t)call->ota_source_id;
            id->known |= DSD_SCAN_ACTIVITY_KNOWN_SOURCE;
        }
    }
    if (call->kind == DSD_CALL_KIND_PRIVATE_VOICE || call->kind == DSD_CALL_KIND_GROUP_VOICE) {
        id->is_private = (uint8_t)(call->kind == DSD_CALL_KIND_PRIVATE_VOICE);
        id->known |= DSD_SCAN_ACTIVITY_KNOWN_PARTY;
    }
    if (DSD_SYNC_IS_YSF(call->protocol) || DSD_SYNC_IS_DSTAR(call->protocol)) {
        id->known |= DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED;
    } else if (call->crypto != DSD_CALL_CRYPTO_UNKNOWN) {
        id->encrypted =
            (uint8_t)(call->crypto == DSD_CALL_CRYPTO_ENCRYPTED || call->crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
        id->known |= DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED;
    }
}

/* Whether @p call is of the family of the scan row on air (issue #890): the row's class (dsd_scan_mode_active(); an
   untyped row takes every protocol), and on a tetra-dmo row only direct mode speech, never a trunked carrier's. */
static int
scan_voice_call_of_row_family(const dsd_state* state, const dsd_call_snapshot* call) {
    const dsd_scan_mode mode = dsd_scan_mode_active(state);
    if (!dsd_scan_mode_family_has_protocol((int)mode, call->protocol)) {
        return 0;
    }
    return mode != DSD_SCAN_MODE_TETRA_DMO || call->checked_media_tetra_direct != 0U;
}

/* The supergroup of a P25 group call on a live patch, judged through the patch resolver; 0 for any other call, which
 * keeps its stored policy target. */
static uint32_t
scan_voice_call_patch_sg(const dsd_state* state, const dsd_call_snapshot* call) {
    if (!DSD_SYNC_IS_P25(call->protocol) || call->kind == DSD_CALL_KIND_PRIVATE_VOICE
        || call->kind == DSD_CALL_KIND_DATA || call->ota_target_id == 0U || call->ota_target_id > UINT32_MAX
        || !dsd_p25_patch_sg_live(state, (uint32_t)call->ota_target_id)) {
        return 0U;
    }
    return (uint32_t)call->ota_target_id;
}

/* The voice check of a patched call, judged on the call the audio follows: the patch's keep-or-resolve media target
 * (dsd_p25_patch_media_resolve(), as the audio gate judges it every frame, so a kept call costs what the gate's does),
 * with that target's own tune verdict and the encrypted-call tuning rule its unencrypted evaluation leaves out. The -Y
 * gate stays for traffic it plays and would follow, as for a plain call: a member that plays but may not be followed
 * (a Hold through a mode block, an encryption lockout of its own) does not hold the channel, even when a member the
 * media ranking put below it could be followed (decided limit: one resolution per frame, the audio's). */
static int
scan_voice_patch_allows(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call, uint32_t sg,
                        uint32_t source, int encrypted) {
    const uint32_t stored = call->policy_target_id <= UINT32_MAX ? (uint32_t)call->policy_target_id : 0U;
    dsd_p25_patch_resolution patch;
    if (dsd_p25_patch_media_resolve(opts, state, sg, stored, source, &patch) != 1) {
        return 0;
    }
    return patch.decision.tune_allowed && !dsd_tg_policy_encrypted_tune_denied(opts, encrypted);
}

static int
scan_voice_snapshot_policy_allows(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call) {
    /* By the identity the call was received with (issue #637): a retained call keeps the hold it earned while a sync
       loss has the live code unknown again. */
    if (!dsd_receive_identity_admitted(opts, &call->receive_identity)) {
        return 0;
    }
    /* The policy table (issue #890), as a protocol's activity report is judged: a callsign family (M17's 48-bit
       values among them) never reaches a talkgroup lookup, and an anonymous call holds nothing under a Hold, the allow
       list or a block on its known source. */
    dsd_trunk_scan_activity_identity identity;
    scan_voice_call_identity(call, &identity);
    /* The data, encryption and party restrictions hold for a patched call too, before the patch resolver judges it. */
    if (!scan_activity_restrictions_allow(opts, &identity)) {
        return 0;
    }
    const uint32_t patch_sg = scan_voice_call_patch_sg(state, call);
    if (patch_sg != 0U) {
        return scan_voice_patch_allows(opts, state, call, patch_sg, identity.source, identity.encrypted);
    }
    return dsd_scan_activity_policy_allows(opts, state, call->protocol, &identity);
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
        if (!scan_voice_snapshot_is_voice(state, &call) || !scan_voice_call_of_row_family(state, &call)) {
            continue;
        }
        if (!scan_voice_snapshot_policy_allows(opts, state, &call)) {
            continue;
        }
        if (call.checked_media_updated_m > out->retained_media_m) {
            out->retained_media_m = call.checked_media_updated_m;
        }
        if (call.phase == DSD_CALL_PHASE_ACTIVE && call.media_active
            && call.checked_media_updated_m > out->active_media_m) {
            out->active_media_m = call.checked_media_updated_m;
        }
    }
    return out->retained_media_m >= 0.0 ? 1 : 0;
}

/* Whether call @p call is the held talkgroup's (@p hold, the state's Hold). A P25 group call on a live patch's
 * supergroup is the held one when the patch resolver's admission picks the held talkgroup
 * (dsd_p25_patch_admits_hold(): no evaluation for a Hold the patch does not carry, at most two otherwise, since the
 * check runs every frame). Any other call is held by its remapped policy target, the OTA target standing in when no
 * remap applies. */
static int
scan_tg_hold_matches_call(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call, uint64_t hold) {
    const uint32_t sg = scan_voice_call_patch_sg(state, call);
    if (sg != 0U) {
        const uint32_t source = call->ota_source_id > UINT32_MAX ? 0U : (uint32_t)call->ota_source_id;
        return dsd_p25_patch_admits_hold(opts, state, sg, source);
    }
    const uint64_t target = call->policy_target_id != 0U ? call->policy_target_id : call->ota_target_id;
    return hold == target;
}

/* Whether call @p call may stand for a talkgroup Hold on the scan row on air (issue #890 F7): a call of the row's family
   (scan_voice_call_of_row_family()), and on a typed TETRA DMO row only a direct mode talkspurt the policy table admits
   whose checked speech was heard on this visit: a trunked call, or a DMO call of another visit, never suspends that
   row's visit cap. */
static int
scan_tg_hold_call_on_row(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call) {
    if (dsd_scan_mode_active(state) != DSD_SCAN_MODE_TETRA_DMO) {
        return dsd_scan_mode_family_has_protocol((int)dsd_scan_mode_active(state), call->protocol);
    }
    return scan_voice_call_of_row_family(state, call) && scan_voice_snapshot_is_voice(state, call)
           && scan_voice_snapshot_policy_allows(opts, state, call);
}

int
dsd_scan_tg_hold_call_active(const dsd_opts* opts, const dsd_state* state) {
    if (!state || state->tg_hold == 0U) {
        return 0;
    }
    const uint64_t hold = (uint64_t)state->tg_hold;
    for (int slot_i = 0; slot_i < DSD_CALL_STATE_SLOT_COUNT; slot_i++) {
        dsd_call_snapshot call;
        /* dsd_call_state_get() fills the whole snapshot on success; a failed get skips the slot. */
        if (dsd_call_state_get(state, (uint8_t)slot_i, &call) <= 0) {
            continue;
        }
        /* A talkgroup is a number: a callsign family (YSF, D-STAR, M17) names its stations by callsign, whose base-40
           M17 code can equal a held talkgroup number by chance, so none of its calls stands for a Hold, as the policy
           table never looks them up (dsd_scan_activity_policy_allows()). */
        if (call.phase != DSD_CALL_PHASE_ACTIVE || call.kind == DSD_CALL_KIND_DATA
            || call.receive_verdict == DSD_RECEIVE_CHECKING || call.receive_verdict == DSD_RECEIVE_REJECTED
            || dsd_scan_activity_protocol_is_callsign(call.protocol) || !scan_tg_hold_call_on_row(opts, state, &call)) {
            continue;
        }
        if (scan_tg_hold_matches_call(opts, state, &call, hold)) {
            return 1;
        }
        /* A private call is held by either end, exactly as the policy layer holds it. */
        if (call.kind == DSD_CALL_KIND_PRIVATE_VOICE && hold == call.ota_source_id) {
            return 1;
        }
    }
    return 0;
}

void
dsd_scan_voice_gate_note_retune(dsd_state* state, double now_m) {
    if (!state) {
        return;
    }
    /* Successful parking opens the per-visit cap too. Suspension handling can restart it,
     * but sync and voice activity must never extend a visit. */
    state->scan_visit_since_m = now_m;
    state->scan_visit_roll_seen = state->lcn_freq_roll;
    state->scan_visit_rearm_pending = 0U;
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
    /* A P25p1 frame dropped behind a tolerant sync word synced nothing: the first sync waits for a real one. */
    if (state->scan_voice_gate_sync_m < 0.0 && synced && !dsd_state_p25p1_frame_dropped(state)) {
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
    /* Channels learned over the air are no list to step through, so with no list of the user's the gate never ends
       a reception it could only leave for one of them. */
    return scan_voice_gate_enabled(opts) && state
           && (state->scan_voice_gate_voice_m >= 0.0 || state->scan_voice_gate_sync_m >= 0.0)
           && dsd_state_trunk_lcn_user_list_present(opts, state);
}

int
dsd_scan_voice_gate_qualifying(const dsd_opts* opts, const dsd_state* state) {
    return dsd_scan_voice_gate_owns_step(opts, state) && state->scan_voice_gate_voice_m < 0.0;
}

int
dsd_scan_voice_gate_should_step(const dsd_opts* opts, const dsd_state* state, double now_m) {
    if (!dsd_scan_voice_gate_owns_step(opts, state)) {
        return 0;
    }
    if (dsd_engine_channel_scan_hold_keeps_row(state)) {
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

/* ---- Per-visit cap for the -Y row (issue #507) ------------------------------ */

/* The cap the row on air is subject to, in ms, with anything below 1000 meaning off. Deliberately
 * not scan_voice_resolve_ms(): that maps 0 to a default window, and here 0 is the default and it
 * means disabled. Config loading is range-free by design, so a hand-written 1..999 reaches the
 * engine and must read as off rather than as a millisecond-long visit. */
static int
scan_visit_cap_enabled(const dsd_opts* opts) {
    return opts->scan_max_visit_ms >= 1000;
}

/* The -Y scanner owns this anchor only while it is the scanner that is running: under
 * --trunk-scan the coordinator keeps its own per-target anchor (trunk_scan.c), and with no
 * scanner there is no visit to measure. */
static int
scan_visit_scanner_owns(const dsd_opts* opts) {
    return opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1;
}

/* While this holds the cap neither counts down nor expires: the operator's row hold, and a
 * talkgroup hold on the very call being followed -- cutting that call short is exactly what the
 * operator asked the hold to prevent. A row hold over a row the operator avoided keeps nothing
 * (dsd_engine_channel_scan_hold_keeps_row(), issue #644). */
static int
scan_visit_suspended(const dsd_opts* opts, const dsd_state* state) {
    return dsd_engine_channel_scan_hold_keeps_row(state) || dsd_scan_tg_hold_call_active(opts, state);
}

/*
 * The visit cannot end right now, and it must not be allowed to age in the meantime either: the
 * tick re-arms it and the deadline hides it, which is the one question both have to ask the same
 * way (requirement 5, and the shape trunk_scan.c already uses).
 *
 * Either the visit is suspended, or there is nowhere to go (dsd_engine_channel_scan_can_rotate()):
 * a rotation with fewer than two rows a step can land on would only hop back onto the row it is
 * already on and interrupt its audio. A row skipped at every visit for its width is not one, nor
 * is a channel learned over the air while no list of the user's is loaded. Freezing the anchor
 * instead of re-arming would let it age for as long as that lasts, and then fire the instant the
 * operator clears an avoid, a placeholder gets a frequency or a skipped row's width fits again.
 */
static int
scan_visit_rearms(const dsd_opts* opts, const dsd_state* state) {
    return scan_visit_suspended(opts, state) || !dsd_engine_channel_scan_can_rotate(opts, state);
}

void
dsd_engine_scan_visit_tick(const dsd_opts* opts, dsd_state* state, double now_m) {
    if (!opts || !state) {
        return;
    }
    if (!scan_visit_scanner_owns(opts)) {
        /* Not this scanner's rotation: drop the anchor rather than leave one behind. A runtime
         * switch into --trunk-scan hands the rotation to the coordinator, and an anchor from the
         * visit before it would otherwise be minutes stale by the time -Y takes it back -- and
         * fire the instant it did. The first owning tick anchors a fresh full limit. */
        state->scan_visit_since_m = -1.0;
        state->scan_visit_roll_seen = state->lcn_freq_roll;
        state->scan_visit_rearm_pending = 0U;
        return;
    }
    if (!scan_visit_cap_enabled(opts)) {
        /* Disabled leaves no anchor behind, so enabling the cap later grants a full fresh limit
         * instead of expiring against a park from minutes ago. */
        state->scan_visit_since_m = -1.0;
        state->scan_visit_roll_seen = state->lcn_freq_roll;
        state->scan_visit_rearm_pending = 0U;
        return;
    }
    if (state->lcn_freq_roll != state->scan_visit_roll_seen) {
        /* The untyped `L` cycle and an avoid move the roll without a retune note, so the row under
         * the anchor changed and the new one is owed its own full limit. */
        state->scan_visit_roll_seen = state->lcn_freq_roll;
        state->scan_visit_since_m = now_m;
        state->scan_visit_rearm_pending = 0U;
    }
    if (state->scan_visit_since_m < 0.0) {
        state->scan_visit_since_m = now_m;
    }
    const int rearm = scan_visit_rearms(opts, state);
    if (rearm || state->scan_visit_rearm_pending) {
        /* Remember the suspension across input stalls: time since the last suspended tick
         * cannot consume the fresh interval owed when a hold or an avoid stops applying. */
        state->scan_visit_since_m = now_m;
    }
    state->scan_visit_rearm_pending = rearm ? 1U : 0U;
}

double
dsd_engine_scan_visit_deadline_m(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return -1.0;
    }
    if (!scan_visit_scanner_owns(opts) || !scan_visit_cap_enabled(opts)) {
        return -1.0;
    }
    if (state->scan_visit_since_m < 0.0 || state->scan_visit_rearm_pending) {
        /* No park to measure from. */
        return -1.0;
    }
    if (state->lcn_freq_roll != state->scan_visit_roll_seen) {
        /* An untyped `L` or avoid moved the row and no tick has reconciled it yet, so the anchor
         * still describes the row before it. The pump can do that between the tick and the step
         * predicate; firing on it would cut the new visit to zero. The next tick re-anchors. */
        return -1.0;
    }
    if (dsd_engine_channel_scan_waiting(state)) {
        /* A row transaction owns the receiver: the cap must not fire into a tune already on its
         * way, and the commit re-opens the visit itself. Unlike the cases below this one is not a
         * re-arm -- the commit stamps its own anchor, so there is nothing to slide. */
        return -1.0;
    }
    if (scan_visit_rearms(opts, state)) {
        return -1.0;
    }
    return state->scan_visit_since_m + ((double)opts->scan_max_visit_ms / 1000.0);
}

int
dsd_engine_scan_visit_expired(const dsd_opts* opts, const dsd_state* state, double now_m) {
    const double deadline_m = dsd_engine_scan_visit_deadline_m(opts, state);
    return deadline_m >= 0.0 && now_m >= deadline_m;
}

void
dsd_scan_timing_clear(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(&state->scan_timing, 0, sizeof(state->scan_timing));
    state->scan_timing.started_m = -1.0;
    state->scan_timing.deadline_m = -1.0;
    state->scan_timing.visit_deadline_m = -1.0;
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
    /* Above the early return: the per-visit cap is independent of the voice gate, and a
     * zeroed double would read as a deadline at monotonic 0. */
    out->visit_deadline_m = -1.0;
    out->conventional = 1U;
    /* Also above the early return: the cap is independent of the voice gate, so it stays visible
     * in legacy hangtime mode, where there is neither a qualify nor a hold window to report. */
    out->visit_limit_ms = scan_visit_cap_enabled(opts) ? (uint32_t)opts->scan_max_visit_ms : 0U;
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
    return (uint32_t)llround(span_ms);
}

/* The legacy rule waits out -t since the last sync and knows nothing else, so it reports
 * no dwell and no hold. Without an anchor there is nothing to count down.
 *
 * The anchor is the wall-clock last_cc_sync_time the step rule itself compares
 * (engine.c no_carrier_scanner_step_is_due), not its monotonic twin: NXDN stamps the
 * wall clock two seconds ahead after a confirmed frame without touching the monotonic
 * field, and a countdown read from the twin would reach zero two seconds early. The
 * window is re-expressed on the monotonic clock through the two clocks sampled together,
 * so it can start in the future and stays put from tick to tick. */
static void
scan_y_timing_fill_hangtime(const dsd_opts* opts, const dsd_state* state, double now_m, double now_wall_s,
                            dsd_scan_timing_publication* out) {
    /* An analog row's carrier keeps restarting the same window (issue #526): say what holds it. */
    out->reason = (uint8_t)(dsd_scan_analog_carrier_open(opts, state) ? DSD_SCAN_STAY_CARRIER : DSD_SCAN_STAY_HANGTIME);
    out->dwell_ms = 0U;
    out->hold_ms = 0U;
    if (state->last_cc_sync_time == 0 || !(opts->trunk_hangtime >= 0.0f)) {
        return;
    }
    /* The step rule's deadline (no_carrier_hangtime_ran_out() in engine.c): no window while a retune's mute still
       holds the input, and -t from the input's return when that came after the anchor and ends later. */
    const double listening_m = dsd_engine_scan_input_listening_since_m(opts, state);
    if (listening_m < 0.0) {
        return;
    }
    out->started_m = now_m + ((double)state->last_cc_sync_time - now_wall_s);
    /* noCarrier compares whole seconds with strict >. The next integer after
     * -t is the first wall-clock tick that permits a hop, including for -t 0. */
    double span_s = floor((double)opts->trunk_hangtime) + 1.0;
    /* Checked content, an unchecked protocol's frames or an open verdict hold the row past that (issue #834): the
       countdown runs to where the step lets it go, read on the decode-mono clock the step reads. */
    const double frames_until_m = dsd_engine_scan_y_frames_hold_until_m(state, (double)opts->trunk_hangtime);
    if (frames_until_m > out->started_m + span_s) {
        span_s = frames_until_m - out->started_m;
    }
    out->deadline_m = out->started_m + span_s;
    out->span_ms = scan_y_timing_span_ms(span_s);
    const double listened_until_m = listening_m + (double)opts->trunk_hangtime;
    if (listening_m > state->last_cc_sync_time_m && listened_until_m > out->deadline_m) {
        out->started_m = listening_m;
        out->deadline_m = listened_until_m;
        out->span_ms = scan_y_timing_span_ms((double)opts->trunk_hangtime);
    }
}

/* The silent-channel skip's status on the -Y row on air (issue #834 R6): shown on its idle dwell (the voice gate's
   qualify window) and its hangtime, the windows a skip ends early. */
static void
scan_y_timing_fill_quiet(const dsd_opts* opts, dsd_state* state, dsd_scan_timing_publication* report) {
    scan_quiet_visit_ref ref;
    DSD_MEMSET(&ref, 0, sizeof ref);
    ref.owner = SCAN_QUIET_OWNER_Y;
    ref.index = dsd_engine_channel_scan_on_air_row(state);
    ref.session = state->lcn_scan_session;
    ref.list_id = state->lcn_list_rev;
    ref.shows_status = scan_quiet_stay_shows_status(report->reason);
    ref.conventional = 1;
    scan_quiet_timing_fill(opts, state, &ref, report);
}

void
dsd_engine_scan_y_timing_tick(const dsd_opts* opts, dsd_state* state, double now_m, double now_wall_s) {
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
    if (dsd_engine_channel_scan_waiting(state)) {
        report.reason = (uint8_t)DSD_SCAN_STAY_RETUNE_PENDING;
    } else if (!dsd_state_trunk_lcn_user_list_present(opts, state)) {
        /* No channel map of the user's (issue #697): nothing steps, so nothing counts down; a channel learned over the
           air is no row to step to. */
        report.reason = (uint8_t)DSD_SCAN_STAY_NO_LIST;
    } else if (dsd_engine_channel_scan_hold_keeps_row(state)) {
        /* The rotation is parked by the operator: the window is paused, not expired.
         * dsd_scan_voice_gate_should_step() returns 0 here and the no-carrier step
         * returns early, so publishing a deadline would count down to a hop that the
         * release, not the clock, actually causes. A hold over a row the operator avoided
         * keeps nothing (issue #644): the row reports the window it leaves by, as an
         * unheld one does. */
        report.reason = (uint8_t)DSD_SCAN_STAY_MANUAL_HOLD;
    } else if (dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_REJECTED) {
        /* Traffic the tone policy rejected (issue #527) holds the row only while the list has nowhere else to go,
           muted, for as long as its carrier lasts; with somewhere to go the next pass steps. Either way no window
           runs (engine.c no_carrier_scanner_step_is_due()), and the hangtime window the carrier no longer restarts
           would count down to a step that never comes. */
        report.reason = (uint8_t)DSD_SCAN_STAY_CARRIER;
    } else if (dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_PENDING) {
        /* A carrier the tone policy is still checking (issue #527) holds the row until its verdict, whatever -t says
           (engine.c no_carrier_scanner_step_is_due()), for no longer than the policy's window. It is no activity and
           restarts no window: the hangtime runs on beneath it from the last carrier the policy passed, or the row's
           landing, so nothing counts down to a step the check itself decides. */
        report.reason = (uint8_t)DSD_SCAN_STAY_TONE_PENDING;
    } else if (dsd_scan_voice_gate_owns_step(opts, state)) {
        scan_y_timing_fill_gate(state, &report);
    } else {
        scan_y_timing_fill_hangtime(opts, state, now_m, now_wall_s, &report);
    }
    /* The cap rides beside the step timer instead of folding into deadline_m/reason: both can be
     * counting down at once, and the reason names the rule the hop would be blamed on. Under a
     * hold the deadline function returns < 0, so MANUAL_HOLD reports the cap as paused. */
    report.visit_deadline_m = dsd_engine_scan_visit_deadline_m(opts, state);
    if (opts->scan_skip_silent) {
        scan_y_timing_fill_quiet(opts, state, &report);
    }
    dsd_scan_timing_publish(state, &report);
}

/* An admitted call of any kind heard on this visit within the hold (issue #637): a call whose terminator was missed
   stays ACTIVE, so only its latest observation counts, never its phase alone. */
static int
scan_admitted_call_recent(const dsd_opts* opts, const dsd_state* state, double arrived_m, double now_m, double hold_s) {
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, (uint8_t)slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
            && call.updated_m >= arrived_m && now_m - call.updated_m < hold_s
            && dsd_receive_allows(opts, state, call.protocol, slot)) {
            return 1;
        }
    }
    return 0;
}

int
dsd_scan_receive_should_leave(const dsd_opts* opts, const dsd_state* state, double arrived_m, double now_m) {
    if (!opts || !state || arrived_m < 0.0 || !dsd_receive_filters_enabled(opts)) {
        return 0;
    }
    /* A two-slot carrier is judged on both lanes, not the one the decoder happens to be on: an idle lane the policy
       admits does not keep a channel whose traffic it rejects or is still checking (issue #637). Then, as for any
       carrier the policy does not wholly admit, only admitted media holds it past the acquisition window. */
    if (dsd_receive_carrier_allows(opts, state)) {
        return 0;
    }
    const double hold_s =
        (double)scan_voice_resolve_ms(opts->scan_voice_hold_ms, DSD_SCAN_VOICE_DEFAULT_HOLD_MS) / 1000.0;
    /* With voice-only scanning off, an admitted call of any kind in progress (a data transfer, which the voice probe
       below never sees) keeps the channel as it would without a filter, while it is still heard. */
    if (opts->scan_voice_only != 1 && scan_admitted_call_recent(opts, state, arrived_m, now_m, hold_s)) {
        return 0;
    }
    dsd_scan_voice_probe_result media;
    (void)dsd_scan_voice_probe(opts, state, &media);
    /* Admitted media heard on this visit holds through its voice hold, after its call ended too (the reply a hold
       waits for), as the voice gate's own hold does. */
    if (media.retained_media_m >= arrived_m && now_m - media.retained_media_m < hold_s) {
        return 0;
    }
    const double acquire_s =
        (double)scan_voice_resolve_ms(opts->scan_voice_qualify_ms, DSD_SCAN_VOICE_DEFAULT_QUALIFY_MS) / 1000.0;
    return now_m - arrived_m >= acquire_s;
}

int
dsd_engine_scan_y_row_holds_on_admitted(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    switch (dsd_scan_mode_active(state)) {
        case DSD_SCAN_MODE_DPMR:
        case DSD_SCAN_MODE_DSTAR:
        case DSD_SCAN_MODE_YSF:
        case DSD_SCAN_MODE_M17:
        case DSD_SCAN_MODE_TETRA_DMO: return 1;
        default: return 0;
    }
}

/* Whether the report being made is an observation of the -Y row landed now (issue #890): a row landing is in force and
   the report's frame was read on it, on the current carrier, so a frame of the row before, or one a carrier boundary
   cut, holds nothing. */
static int
scan_y_observation_on_row(const dsd_state* state) {
    const dsd_engine_scan_landing* landing = dsd_engine_scan_landing_get(state);
    if (state->scan_landing_visit == 0U || !landing || landing->by != DSD_ENGINE_SCAN_LANDING_ROW) {
        return 0;
    }
    dsd_scan_observation_origin origin;
    return dsd_state_scan_observation_origin(state, &origin) && dsd_state_scan_observation_is_current(state, &origin);
}

void
dsd_engine_scan_y_typed_activity(const dsd_opts* opts, const dsd_state* state, int protocol, int slot,
                                 const dsd_trunk_scan_activity_identity* identity) {
    if (!opts || !state || !identity || opts->scanner_mode != 1 || opts->trunk_scan_enabled == 1
        || !dsd_engine_scan_y_row_holds_on_admitted(state)
        || !dsd_scan_mode_family_has_protocol((int)dsd_scan_mode_active(state), protocol)
        || !scan_y_observation_on_row(state)) {
        return;
    }
    /* The owner is the engine's own extension, apart from the state the report only reads, as the trunk scan's
       coordinator is for its targets' holds. */
    dsd_engine_scan_owner* owner =
        (dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    if (!owner) {
        return;
    }
    const int admitted = dsd_receive_allows(opts, state, protocol, slot)
                         && dsd_scan_activity_policy_allows(opts, state, protocol, identity);
    /* A refusal is the refused transmission's: bound to the call active on its slot at the next frame boundary
       (live_scanner_y_refusal_stands() in engine.c), which sees the call this report's frame observed. */
    owner->y_refused = (uint8_t)(admitted ? 0U : 1U);
    owner->y_refused_slot = (uint8_t)(slot == 1 ? 1 : 0);
    owner->y_refused_epoch = 0U;
    if (admitted) {
        owner->y_admitted_seq++;
    }
}
