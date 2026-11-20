// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Audio gating helpers used by mixers and tests.
 *
 * The helpers here centralize per-slot gating decisions so that
 * dsd_audio2.c only needs to invoke them rather than duplicate the
 * whitelist/TG-hold logic.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/key_material.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_patch_policy.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd_audio_gate_internal.h"

static int
dsd_audio_state_is_p25(const dsd_state* state) {
    return (state && (DSD_SYNC_IS_P25(state->synctype) || DSD_SYNC_IS_P25(state->lastsynctype))) ? 1 : 0;
}

// What a call is judged on: the talkgroup, and the decision when the P25 patch resolver already made it (evaluated for
// `source`, unencrypted, not a data call: as the gates evaluate a call), so the gates need not evaluate it again.
typedef struct {
    uint32_t target;
    uint32_t source;
    int decided;
    dsd_tg_policy_decision decision;
} dsd_audio_judged_call;

static void
dsd_audio_judged_init(dsd_audio_judged_call* judged, uint32_t target) {
    judged->target = target;
    judged->source = 0U;
    judged->decided = 0;
}

// How an active P25 group call on @p ota_target is judged, from one snapshot of the call: its stored policy target while
// that may play (the supergroup on its own decision, or a current patch member), else the patch resolver's answer for
// media (dsd_p25_patch_media_resolve()). @p ota_target, undecided, when it is no current patch's supergroup.
static void
dsd_audio_p25_judge_call(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                         uint32_t ota_target, dsd_audio_judged_call* judged) {
    dsd_audio_judged_init(judged, ota_target);
    if (call->phase != DSD_CALL_PHASE_ACTIVE || !DSD_SYNC_IS_P25(call->protocol)
        || call->kind == DSD_CALL_KIND_PRIVATE_VOICE || call->ota_target_id != ota_target) {
        return;
    }
    const uint32_t stored = call->policy_target_id <= UINT32_MAX ? (uint32_t)call->policy_target_id : 0U;
    const uint32_t source = call->ota_source_id <= UINT32_MAX ? (uint32_t)call->ota_source_id : 0U;
    dsd_p25_patch_resolution resolution;
    if (dsd_p25_patch_media_resolve(opts, state, ota_target, stored, source, &resolution) == 1) {
        judged->target = resolution.policy_target;
        judged->source = source;
        judged->decided = 1;
        judged->decision = resolution.decision;
    }
}

static int
dsd_audio_call_for_target(const dsd_state* state, int slot, uint32_t ota_target, uint32_t policy_tg,
                          dsd_call_snapshot* call) {
    // Mono callers prefer the current slot; an explicit slot never borrows its companion's call kind or source.
    const int first = slot >= 0 ? slot : (state->currentslot == 1 ? 1 : 0);
    const int count = slot >= 0 ? 1 : DSD_CALL_STATE_SLOT_COUNT;
    for (int i = 0; i < count; ++i) {
        if (dsd_call_state_get(state, (uint8_t)(first ^ i), call) > 0 && call->phase == DSD_CALL_PHASE_ACTIVE
            && call->ota_source_id <= UINT32_MAX && call->ota_target_id <= UINT32_MAX
            && (call->ota_target_id == ota_target || call->policy_target_id == policy_tg)) {
            return 1;
        }
    }
    return 0;
}

static void
dsd_audio_p25_judge_slot(const dsd_opts* opts, const dsd_state* state, int slot, uint32_t ota_target,
                         dsd_audio_judged_call* judged) {
    dsd_call_snapshot call;
    if (!state || slot < 0 || slot > 1 || dsd_call_state_get(state, (uint8_t)slot, &call) <= 0) {
        dsd_audio_judged_init(judged, ota_target);
        return;
    }
    dsd_audio_p25_judge_call(opts, state, &call, ota_target, judged);
}

// A mono gate's group: the first slot judged on another talkgroup than @p ota_target, else @p ota_target with the first
// decision a slot made for it (the gate reuses it only for a call of that source on @p ota_target).
static void
dsd_audio_p25_judge_group(const dsd_opts* opts, const dsd_state* state, uint32_t ota_target,
                          dsd_audio_judged_call* judged) {
    dsd_audio_judged_init(judged, ota_target);
    if (!dsd_audio_state_is_p25(state)) {
        return;
    }
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        dsd_audio_judged_call slot_judged;
        dsd_audio_p25_judge_slot(opts, state, slot, ota_target, &slot_judged);
        if (slot_judged.target != ota_target) {
            *judged = slot_judged;
            return;
        }
        if (slot_judged.decided && !judged->decided) {
            *judged = slot_judged;
        }
    }
}

dsd_key_material_need
dsd_dmr_alg_key_need(int algid) {
    switch (algid) {
        case 0x02: /* Hytera Enhanced */
        case 0x21: /* DMR RC4 */
        case 0x22: /* DMR DES */
        case 0x81: /* P25 DES */
        case 0x9F: /* P25 DES-XL */
        case 0xAA: /* P25 RC4 */ return DSD_KEY_NEED_SCALAR;
        case 0x24: /* DMR AES-128 */
        case 0x89: /* P25 AES-128 */ return DSD_KEY_NEED_AES_2;
        case 0x83: /* P25 TDEA */ return DSD_KEY_NEED_AES_3;
        case 0x25: /* DMR AES-256 */
        case 0x84: /* P25 AES-256 */ return DSD_KEY_NEED_AES_4;
        case 0x36: /* Kirisun */
        case 0x37: /* Kirisun */ return DSD_KEY_NEED_QUARTET;
        default: return DSD_KEY_NEED_NONE;
    }
}

int
dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded) {
    switch (dsd_dmr_alg_key_need(algid)) {
        case DSD_KEY_NEED_SCALAR: return (r_key != 0ULL) ? 1 : 0;
        case DSD_KEY_NEED_AES_2:
        case DSD_KEY_NEED_AES_3:
        case DSD_KEY_NEED_AES_4: return (aes_loaded == 1) ? 1 : 0;
        // Kirisun decides on the quartet, which this signature cannot see: only
        // dsd_dmr_voice_kid_can_decrypt() and dsd_dmr_voice_slot_can_decrypt() answer that family.
        case DSD_KEY_NEED_QUARTET:
        case DSD_KEY_NEED_NONE: break;
    }
    return 0;
}

static int
dsd_dmr_slot_valid(int slot) {
    return slot == 0 || slot == 1;
}

int
dsd_dmr_kirisun_slot_key_complete(const dsd_state* state, int slot) {
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return 0;
    }

    return state->aes_key_segments[slot] == 4U && state->A1[slot] != 0ULL && state->A2[slot] != 0ULL
           && state->A3[slot] != 0ULL && state->A4[slot] != 0ULL;
}

int
dsd_dmr_missing_alg_key_can_decrypt(const dsd_state* state, int slot) {
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return 0;
    }

    return dsd_key_scalar_present(state, slot) || dsd_key_basic_present(state) || dsd_key_hytera_present(state);
}

int
dsd_dmr_voice_kid_can_decrypt(const dsd_state* state, int slot, int algid, const dsd_dmr_key_material* key) {
    if (!state || !key || !dsd_dmr_slot_valid(slot)) {
        return 0;
    }
    if (algid == 0x36 || algid == 0x37) {
        // Supplied, not read from the slot: keyring_activate_slot_with_kid() overwrites
        // aes_key_segments[] and A1..A4[] for these ALG IDs too, so a prospective key id does
        // change Kirisun completeness and the caller has to say which key it means.
        return key->kirisun_complete ? 1 : 0;
    }
    const dsd_key_material_need need = dsd_dmr_alg_key_need(algid);
    if (need == DSD_KEY_NEED_SCALAR && key->scalar_present) {
        return 1;
    }
    // Activation does not know the ALG, so a loaded AES key narrower than this ALG's width (a lone imported scalar on
    // the key ID's own cell, say) is no key for it.
    const unsigned int width = dsd_key_need_aes_width(need);
    if (width != 0U && key->aes_segments != 0 && key->aes_segments < (int)width) {
        return 0;
    }
    return dsd_dmr_voice_alg_can_decrypt(algid, key->r_key, key->aes_loaded);
}

dsd_dmr_key_material
dsd_dmr_slot_key_material(const dsd_state* state, int slot, int key_id, int mapped) {
    dsd_dmr_key_material material = {0};
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return material;
    }

    material.r_key = (slot == 0) ? state->R : state->RR;
    material.scalar_present = dsd_key_scalar_present(state, slot);
    material.aes_loaded = state->aes_key_loaded[slot];
    material.aes_segments = state->aes_key_segments[slot];
    material.kirisun_complete = dsd_dmr_kirisun_slot_key_complete(state, slot);
    if (mapped) {
        material.scalar_present = keyring_kid_scalar_present(state, key_id);
        (void)keyring_kid_material(state, key_id, &material.r_key, &material.aes_loaded);
        material.aes_segments = keyring_kid_aes_segments(state, key_id);
        material.kirisun_complete = keyring_kid_kirisun_complete(state, key_id);
    }
    return material;
}

int
dsd_dmr_voice_slot_can_decrypt(const dsd_state* state, int slot, int algid, unsigned long long r_key) {
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return 0;
    }
    const dsd_dmr_key_material key = {r_key, state->aes_key_loaded[slot],
                                      dsd_dmr_kirisun_slot_key_complete(state, slot), state->scalar_key_present[slot],
                                      state->aes_key_segments[slot]};
    return dsd_dmr_voice_kid_can_decrypt(state, slot, algid, &key);
}

// Whether the DMR voice hold applies: not on the single-slot decoder without trunking, nor to direct-mode traffic,
// whose one lane carries both slots (issue #834).
int
dsd_dmr_voice_hold_applies(const dsd_opts* opts, const dsd_state* state) {
    if (opts != NULL && opts->dmr_mono == 1 && opts->trunk_enable != 1) {
        return 0;
    }
    if (state != NULL && (DSD_SYNC_IS_DMR_MS(state->synctype) || state->dmr_ms_mode == 1)) {
        return 0;
    }
    return 1;
}

// The --dmr-force-algid value, or 0 when none is in force. state->M doubles as the scrambler
// key for 0/1 and as the 0x16 Hytera marker, neither of which is a forced ALG ID.
int
dsd_dmr_forced_algid(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    if (state->M <= 1 || state->M == 0x16) {
        return 0;
    }
    return state->M & 0xFF;
}

int
dsd_dmr_classify_algid(const dsd_state* state, int slot, int so) {
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return 0;
    }
    const int algid = (slot == 0) ? state->payload_algid : state->payload_algidR;
    if (algid != 0 || (so & 0x40) == 0) {
        return algid;
    }
    return dsd_dmr_forced_algid(state);
}

int
dsd_dmr_apply_forced_algid(dsd_state* state) {
    if (!state) {
        return 0;
    }
    const int forced = dsd_dmr_forced_algid(state);
    if (forced == 0) {
        return 0;
    }

    // Fallback only: OTA identifiers from a verified PI header or LE single burst take
    // precedence, so a slot that already carries an ALG ID is left untouched and a known
    // KEY ID is never replaced by the 0xFF "no key id" sentinel (issue #351).
    if (state->currentslot == 0 && (state->dmr_so & 0x40) != 0 && state->payload_algid == 0) {
        state->payload_algid = forced;
        if (state->payload_keyid == 0) {
            state->payload_keyid = 0xFF;
        }
        return 1;
    }
    if (state->currentslot == 1 && (state->dmr_soR & 0x40) != 0 && state->payload_algidR == 0) {
        state->payload_algidR = forced;
        if (state->payload_keyidR == 0) {
            state->payload_keyidR = 0xFF;
        }
        return 1;
    }

    return 0;
}

static int
dsd_p25p2_slot_crypto_permits_audio(const dsd_opts* opts, const dsd_state* state, int slot, int alg) {
    (void)alg;
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return p25_crypto_audio_permitted(opts, state, slot);
}

// A matching Hold overrides the talkgroup part of the policy only: explicit temporary blocks (session avoid, call
// skip) still mute a held talkgroup, and the radio part (the radio list's rows) applies after the override.
static int
dsd_audio_hold_overrides_policy(const dsd_tg_policy_decision* decision) {
    return decision->tg_hold_active && decision->tg_hold_match
           && !(decision->block_reasons & (DSD_TG_POLICY_BLOCK_SESSION_AVOID | DSD_TG_POLICY_BLOCK_CALL_SKIP));
}

int
dsd_audio_policy_tg_allows_audio(const dsd_tg_policy_decision* decision) {
    return decision
           && (dsd_audio_hold_overrides_policy(decision)
               || (decision->tg_audio_allowed && (decision->block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) == 0u));
}

// The media verdicts: (Hold override || talkgroup part) && radio part. The allow list is a talkgroup reason.
static int
dsd_audio_policy_allows_audio(const dsd_tg_policy_decision* decision) {
    return dsd_audio_policy_tg_allows_audio(decision) && decision->radio_audio_allowed;
}

static int
dsd_audio_policy_allows_record(const dsd_tg_policy_decision* decision) {
    const int tg = dsd_audio_hold_overrides_policy(decision)
                   || (decision->tg_record_allowed && (decision->block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) == 0u);
    return tg && decision->radio_record_allowed;
}

// The rdio-scanner export verdict (issue #773 D7): (Hold override ? no explicit stream block : talkgroup part with the
// allow list) && radio part. A Hold lifts what it lifts for recording -- an allow-list miss and a stream flag only
// derived off (a B/DE or audio=off row) -- but never an explicit stream=off on an audible row
// (DSD_TG_POLICY_BLOCK_STREAM; the radio part sets that reason too, and is never lifted).
static int
dsd_audio_policy_allows_stream(const dsd_tg_policy_decision* decision) {
    const int tg =
        dsd_audio_hold_overrides_policy(decision)
            ? (decision->block_reasons & DSD_TG_POLICY_BLOCK_STREAM) == 0u
            : (decision->tg_stream_allowed && (decision->block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) == 0u);
    return tg && decision->radio_stream_allowed;
}

static int
dsd_p25p2_media_decision_allows_audio(const dsd_tg_policy_decision* decision) {
    return decision ? dsd_audio_policy_allows_audio(decision) : 1;
}

// The policy part of a P25 Phase 2 output verdict from one decision (issue #651): what the mixers' talkgroup gate
// (dsd_audio_group_gate_slot()) and the record gate (dsd_audio_record_policy_blocks()) answer for it. For P25 both
// evaluate the call exactly as the decode gate does, so one decision serves all three.
static void
dsd_p25p2_verdict_from_decision(const dsd_tg_policy_decision* decision, dsd_p25p2_playout_verdict* verdict) {
    // A radio the radio list mutes stays muted under a Hold: the hold flag only stands for a Hold that plays the call.
    verdict->hold = (dsd_audio_hold_overrides_policy(decision) && decision->radio_audio_allowed) ? 1U : 0U;
    verdict->blocked = dsd_audio_policy_allows_audio(decision) ? 0U : 1U;
    verdict->recordable = dsd_audio_policy_allows_record(decision) ? 1U : 0U;
}

// The slot's active call, if it has one, in one look.
static int
dsd_p25p2_slot_active_call(const dsd_state* state, int slot, dsd_call_snapshot* call) {
    return dsd_call_state_get(state, (uint8_t)slot, call) > 0 && call->phase == DSD_CALL_PHASE_ACTIVE;
}

// What an active call's verdict reads, its identity taken from this snapshot of it: see dsd_p25p2_policy_key. Ids out
// of range read 0.
static void
dsd_p25p2_call_policy_key(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                          dsd_p25p2_policy_key* key, dsd_audio_judged_call* judged) {
    DSD_MEMSET(key, 0, sizeof(*key));
    dsd_tg_policy_table_version(state, &key->table_context, &key->table_generation);
    key->second = (int64_t)dsd_decode_time();
    key->tg_hold = state->tg_hold;
    key->allow_list = (opts && opts->trunk_use_allow_list == 1) ? 1U : 0U;
    key->record_unknown_only = (opts && opts->record_unknown_only != 0) ? 1U : 0U;
    key->radio_store = dsd_source_alias_store_id(state);
    key->ota_target = (call->ota_target_id <= UINT32_MAX) ? (uint32_t)call->ota_target_id : 0U;
    key->source = (call->ota_source_id <= UINT32_MAX) ? (uint32_t)call->ota_source_id : 0U;
    key->kind = (uint8_t)call->kind;
    dsd_audio_judged_init(judged, key->ota_target);
    if (call->kind != DSD_CALL_KIND_PRIVATE_VOICE && call->ota_target_id <= UINT32_MAX) {
        dsd_audio_p25_judge_call(opts, state, call, key->ota_target, judged);
    }
    key->policy_target = judged->target;
}

// The decision on an active call, private or group, as the decode gate evaluates it, all from this one snapshot of the
// call and judged on the policy target @p key holds (taken first, from the same snapshot, with @p judged), so neither a
// call that ends meanwhile nor a patch going stale can leave the decision and its key telling two stories: a decision
// the patch resolver made with the key is used as it is. 0 when it has one.
static int
dsd_p25p2_call_policy_decision(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                               const dsd_p25p2_policy_key* key, const dsd_audio_judged_call* judged,
                               dsd_tg_policy_decision* decision) {
    if (call->ota_target_id > UINT32_MAX || call->ota_source_id > UINT32_MAX) {
        return -1;
    }
    const uint32_t target = (uint32_t)call->ota_target_id;
    const uint32_t source = (uint32_t)call->ota_source_id;
    if (call->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return dsd_tg_policy_evaluate_private_call(opts, state, source, target, 0, 0, decision) == 0 ? 0 : -1;
    }
    if (judged->decided) {
        *decision = judged->decision;
        return 0;
    }
    const uint32_t policy_target = key->policy_target;
    if (dsd_tg_policy_evaluate_group_call(opts, state, policy_target, source, 0, 0, decision) != 0) {
        return -1;
    }
    (void)dsd_tg_policy_apply_ota_final_blocks(opts, state, target, policy_target, source, decision);
    return 0;
}

int
dsd_p25p2_decode_policy_verdict(const dsd_opts* opts, const dsd_state* state, int slot, int alg,
                                dsd_p25p2_burst_decision* burst, int* burst_valid) {
    dsd_tg_policy_decision decision;
    dsd_call_snapshot call;
    dsd_p25p2_policy_key key;

    if (burst_valid) {
        *burst_valid = 0;
    }
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    if (!dsd_p25p2_slot_crypto_permits_audio(opts, state, slot, alg)) {
        return 0;
    }
    if (!dsd_p25p2_slot_active_call(state, slot, &call)) {
        return 0;
    }
    dsd_audio_judged_call judged;
    dsd_p25p2_call_policy_key(opts, state, &call, &key, &judged);
    if (dsd_p25p2_call_policy_decision(opts, state, &call, &key, &judged, &decision) != 0) {
        return 0;
    }
    if (burst) {
        dsd_p25p2_verdict_from_decision(&decision, &burst->verdict);
        burst->verdict.crypto_ok = 0U;
        burst->key = key;
        burst->epoch = call.epoch;
        if (burst_valid) {
            *burst_valid = 1;
        }
    }
    return dsd_p25p2_media_decision_allows_audio(&decision);
}

int
dsd_p25p2_decode_audio_allowed_verdict(const dsd_opts* opts, const dsd_state* state, int slot, int alg,
                                       dsd_p25p2_burst_decision* burst, int* burst_valid) {
    if (burst_valid) {
        *burst_valid = 0;
    }
    if (!state || slot < 0 || slot > 1 || !dsd_receive_allows(opts, state, DSD_SYNC_P25P2_POS, slot)) {
        return 0;
    }
    return dsd_p25p2_decode_policy_verdict(opts, state, slot, alg, burst, burst_valid);
}

int
dsd_p25p2_decode_audio_allowed(const dsd_opts* opts, const dsd_state* state, int slot, int alg) {
    return dsd_p25p2_decode_audio_allowed_verdict(opts, state, slot, alg, NULL, NULL);
}

int
dsd_p25p2_playout_policy_verdict(const dsd_opts* opts, const dsd_state* state, int slot, dsd_p25p2_playout_verdict* out,
                                 uint64_t* call_epoch, dsd_p25p2_policy_key* key) {
    dsd_tg_policy_decision decision;
    dsd_call_snapshot call;
    dsd_p25p2_policy_key judged;
    if (!out) {
        return 0;
    }
    out->blocked = 0U;
    out->hold = 0U;
    out->crypto_ok = 0U;
    out->recordable = 1U;
    if (!opts || !state || slot < 0 || slot > 1 || !dsd_p25p2_slot_active_call(state, slot, &call)) {
        return 0;
    }
    if (call_epoch) {
        *call_epoch = call.epoch;
    }
    dsd_audio_judged_call judged_call;
    dsd_p25p2_call_policy_key(opts, state, &call, &judged, &judged_call);
    if (key) {
        *key = judged;
    }
    if (dsd_p25p2_call_policy_decision(opts, state, &call, &judged, &judged_call, &decision) == 0) {
        dsd_p25p2_verdict_from_decision(&decision, out);
        if (!dsd_receive_allows(opts, state, DSD_SYNC_P25P2_POS, slot)) {
            /* A hard gate (issue #637): no talkgroup hold plays traffic the receive policy rejects or is checking. */
            out->blocked = 1;
            out->hold = 0;
            out->recordable = 0;
        }
    }
    return 1;
}

int
dsd_p25p2_playout_policy_key(const dsd_opts* opts, const dsd_state* state, int slot, uint64_t* call_epoch,
                             dsd_p25p2_policy_key* key) {
    dsd_call_snapshot call;
    if (!state || slot < 0 || slot > 1 || !dsd_p25p2_slot_active_call(state, slot, &call)) {
        return 0;
    }
    if (call_epoch) {
        *call_epoch = call.epoch;
    }
    if (key) {
        dsd_audio_judged_call judged;
        dsd_p25p2_call_policy_key(opts, state, &call, key, &judged);
    }
    return 1;
}

// A private call is judged on its endpoints; a group call on its policy target
// plus the over-the-air supergroup's final blocks. @p judged (may be NULL) is how the gate chose the policy target: its
// decision stands when it was made for this call's over-the-air target and source.
static int
dsd_audio_group_gate_decision(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                              uint32_t ota_tg, uint32_t policy_tg, const dsd_audio_judged_call* judged,
                              dsd_tg_policy_decision* decision) {
    const uint32_t source_id = call ? (uint32_t)call->ota_source_id : 0;
    if (call && call->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return dsd_tg_policy_evaluate_private_call(opts, state, source_id, (uint32_t)call->ota_target_id, 0, 0,
                                                   decision);
    }
    if (judged && judged->decided && judged->target == policy_tg && judged->source == source_id && call
        && call->ota_target_id == ota_tg) {
        *decision = judged->decision;
        return 0;
    }
    const int rc = dsd_tg_policy_evaluate_group_call(opts, state, policy_tg, source_id, 0, 0, decision);
    if (rc == 0) {
        (void)dsd_tg_policy_apply_ota_final_blocks(opts, state, call ? (uint32_t)call->ota_target_id : ota_tg,
                                                   policy_tg, source_id, decision);
    }
    return rc;
}

// The gate's verdict on @p call (NULL: a group call on @p policy_tg with no known source): 1 when the talkgroup policy
// mutes it. A hold decides the talkgroup policy, never encryption or the slot switches a caller folds into its own mute
// (issue #655): a matching hold passes the policy.
static int
dsd_audio_group_gate_mutes(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call, uint32_t ota_tg,
                           uint32_t policy_tg, const dsd_audio_judged_call* judged) {
    dsd_tg_policy_decision decision;
    if (dsd_audio_group_gate_decision(opts, state, call, ota_tg, policy_tg, judged, &decision) != 0) {
        return 0;
    }
    return !dsd_audio_policy_allows_audio(&decision);
}

static int
dsd_audio_group_gate_slot(const dsd_opts* opts, const dsd_state* state, int slot, unsigned long tg, int enc_in,
                          int* enc_out) {
    uint32_t ota_tg = (uint32_t)tg;

    if (!opts || !state || !enc_out) {
        return -1;
    }

    // The slot keeps the mute @p enc_in brought in; the policy only adds one.
    int enc = (enc_in != 0 || !dsd_receive_current_allows(opts, state, slot < 0 ? state->currentslot : slot)) ? 1 : 0;
    dsd_audio_judged_call judged;
    if (slot >= 0 && slot <= 1) {
        dsd_audio_p25_judge_slot(opts, state, slot, ota_tg, &judged);
    } else {
        dsd_audio_p25_judge_group(opts, state, ota_tg, &judged);
    }
    const uint32_t policy_tg = judged.target;

    dsd_call_snapshot call;
    const int have_call = dsd_audio_call_for_target(state, slot, ota_tg, policy_tg, &call);
    if (dsd_audio_group_gate_mutes(opts, state, have_call ? &call : NULL, ota_tg, policy_tg, &judged)) {
        enc = 1;
    }

    *enc_out = enc;
    return 0;
}

int
dsd_audio_group_gate_call(const dsd_opts* opts, const dsd_state* state, int private_call, uint32_t target,
                          uint32_t source, int enc_in, int* enc_out) {
    if (!opts || !state || !enc_out) {
        return -1;
    }
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    call.phase = DSD_CALL_PHASE_ACTIVE;
    call.kind = private_call ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = target;
    call.policy_target_id = target;
    call.ota_source_id = source;
    *enc_out = (enc_in != 0 || dsd_audio_group_gate_mutes(opts, state, &call, target, target, NULL)) ? 1 : 0;
    return 0;
}

int
dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out) {
    return dsd_audio_group_gate_slot(opts, state, -1, tg, enc_in, enc_out);
}

int
dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                          int encL_in, int encR_in, int* encL_out, int* encR_out) {
    if (!encL_out || !encR_out) {
        return -1;
    }
    int rc = 0;
    rc |= dsd_audio_group_gate_slot(opts, state, 0, tgL, encL_in, encL_out);
    rc |= dsd_audio_group_gate_slot(opts, state, 1, tgR, encR_in, encR_out);
    return rc;
}

// The per-call WAV records what the speakers play: the vocoder's verdict on the slot's frame, judged by the protocol
// that owns the call, the user's encrypted-audio mute and reverse mute (-q) included (issue #655).
static int
dsd_audio_record_slot_allows_audio(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (!opts || !state || slot < 0 || slot > 1) {
        return 0;
    }
    return state->mbe_slot_muted[slot] == 0 && dsd_receive_current_allows(opts, state, slot);
}

// @p judged (may be NULL): how the policy target was chosen; a decision the patch resolver made stands.
static int
dsd_audio_record_policy_evaluate(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                                 uint32_t ota_target, uint32_t policy_target, uint32_t source_id,
                                 const dsd_audio_judged_call* judged, dsd_tg_policy_decision* decision) {
    if (call->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return dsd_tg_policy_evaluate_private_call(opts, state, source_id, ota_target, 0, 0, decision);
    }
    if (judged && judged->decided && judged->target == policy_target && judged->source == source_id) {
        *decision = judged->decision;
        return 0;
    }
    const int rc = dsd_tg_policy_evaluate_group_call(opts, state, policy_target, source_id, 0, 0, decision);
    if (rc == 0) {
        (void)dsd_tg_policy_apply_ota_final_blocks(opts, state, ota_target, policy_target, source_id, decision);
    }
    return rc;
}

// How the record gate and the export verdict judge a group call, from this snapshot of it: a P25 call's keep-or-resolve
// (dsd_audio_p25_judge_call()), else on its policy target, else on @p ota_target.
static void
dsd_audio_record_policy_target(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                               uint32_t ota_target, dsd_audio_judged_call* judged) {
    if (DSD_SYNC_IS_P25(call->protocol)) {
        dsd_audio_p25_judge_call(opts, state, call, ota_target, judged);
        return;
    }
    dsd_audio_judged_init(judged, (call->policy_target_id != 0U && call->policy_target_id <= UINT32_MAX)
                                      ? (uint32_t)call->policy_target_id
                                      : ota_target);
}

static int
dsd_audio_record_policy_blocks(const dsd_opts* opts, const dsd_state* state, int slot) {
    dsd_tg_policy_decision decision;

    if (!opts || !state || !dsd_receive_current_allows(opts, state, slot)) {
        return 1;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.ota_target_id > UINT32_MAX || call.ota_source_id > UINT32_MAX) {
        return 0;
    }
    const uint32_t ota_target = (uint32_t)call.ota_target_id;
    dsd_audio_judged_call judged;
    dsd_audio_record_policy_target(opts, state, &call, ota_target, &judged);
    const uint32_t source_id = (uint32_t)call.ota_source_id;
    int rc =
        dsd_audio_record_policy_evaluate(opts, state, &call, ota_target, judged.target, source_id, &judged, &decision);
    if (rc != 0) {
        return 0;
    }

    return !dsd_audio_policy_allows_record(&decision);
}

int
dsd_audio_stream_export_allowed(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                                uint32_t policy_target, uint32_t* judged_target) {
    if (judged_target) {
        *judged_target = 0U;
    }
    if (!state || !call || call->epoch == 0U || call->protocol == DSD_SYNC_ANALOG || DSD_SYNC_IS_M17(call->protocol)
        || call->ota_target_id > UINT32_MAX || call->ota_source_id > UINT32_MAX) {
        return 1;
    }
    const uint32_t ota_target = (uint32_t)call->ota_target_id;
    const uint32_t source_id = (uint32_t)call->ota_source_id;
    dsd_audio_judged_call judged;
    dsd_audio_judged_init(&judged, ota_target);
    if (call->kind != DSD_CALL_KIND_PRIVATE_VOICE) {
        if (policy_target != 0U) {
            judged.target = policy_target;
        } else {
            dsd_audio_record_policy_target(opts, state, call, ota_target, &judged);
        }
    }
    const uint32_t target = judged.target;
    dsd_tg_policy_decision decision;
    if (dsd_audio_record_policy_evaluate(opts, state, call, ota_target, target, source_id, &judged, &decision) != 0) {
        return 1;
    }
    if (judged_target) {
        *judged_target = target;
    }
    return dsd_audio_policy_allows_stream(&decision);
}

int
dsd_audio_record_gate_mono(const dsd_opts* opts, const dsd_state* state, int* allow_out) {
    if (!opts || !state || !allow_out) {
        return -1;
    }

    const int slot = (state->currentslot == 1) ? 1 : 0;
    int allow = dsd_audio_record_slot_allows_audio(opts, state, slot);

    if (allow && dsd_audio_record_policy_blocks(opts, state, slot)) {
        allow = 0;
    }

    *allow_out = allow;
    return 0;
}

int
dsd_audio_record_policy_gate_slot(const dsd_opts* opts, const dsd_state* state, int slot, int* allow_out) {
    if (!opts || !state || !allow_out || slot < 0 || slot > 1) {
        return -1;
    }
    *allow_out = !dsd_audio_record_policy_blocks(opts, state, slot);
    return 0;
}
