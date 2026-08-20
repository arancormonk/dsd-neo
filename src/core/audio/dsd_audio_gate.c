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
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <stdint.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#define DSD_AUDIO_P25_PATCH_TTL_SECONDS 20

static int
dsd_audio_state_is_p25(const dsd_state* state) {
    return (state && (DSD_SYNC_IS_P25(state->synctype) || DSD_SYNC_IS_P25(state->lastsynctype))) ? 1 : 0;
}

static int
dsd_audio_p25_policy_pair_valid(uint32_t ota_target, uint32_t policy_tg) {
    return (ota_target != 0U && ota_target <= UINT16_MAX && policy_tg != 0U && policy_tg <= UINT16_MAX
            && policy_tg != ota_target)
               ? 1
               : 0;
}

static int
dsd_audio_p25_patch_entry_current(const dsd_state* state, int idx, uint32_t ota_target, time_t now) {
    if (!state || idx < 0 || idx >= 8 || !state->p25_patch_active[idx]
        || state->p25_patch_sgid[idx] != (uint16_t)ota_target) {
        return 0;
    }
    if (state->p25_patch_last_update[idx] > 0
        && (now - state->p25_patch_last_update[idx]) > DSD_AUDIO_P25_PATCH_TTL_SECONDS) {
        return 0;
    }
    return 1;
}

static int
dsd_audio_p25_patch_entry_has_wgid(const dsd_state* state, int idx, uint32_t policy_tg) {
    uint8_t count = state->p25_patch_wgid_count[idx];
    for (int k = 0; k < count && k < 8; k++) {
        if (state->p25_patch_wgid[idx][k] == (uint16_t)policy_tg) {
            return 1;
        }
    }
    return 0;
}

static int
dsd_audio_p25_patch_member_active(const dsd_state* state, uint32_t ota_target, uint32_t policy_tg) {
    if (!state || !dsd_audio_p25_policy_pair_valid(ota_target, policy_tg)) {
        return 0;
    }

    time_t now = time(NULL);
    for (int i = 0; i < state->p25_patch_count && i < 8; i++) {
        if (dsd_audio_p25_patch_entry_current(state, i, ota_target, now)
            && dsd_audio_p25_patch_entry_has_wgid(state, i, policy_tg)) {
            return 1;
        }
    }
    return 0;
}

static int
dsd_audio_p25_policy_tg_valid_for_slot(const dsd_state* state, int slot, uint32_t ota_target) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_P25(call.protocol) || call.ota_target_id != ota_target || call.policy_target_id > UINT32_MAX) {
        return 0;
    }

    return dsd_audio_p25_patch_member_active(state, ota_target, (uint32_t)call.policy_target_id);
}

static uint32_t
dsd_audio_group_source_id(const dsd_state* state, unsigned long tg) {
    uint32_t id = (uint32_t)tg;
    if (!state) {
        return 0;
    }
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
            || call.ota_source_id > UINT32_MAX) {
            continue;
        }
        if (call.ota_target_id == id || call.policy_target_id == id) {
            return (uint32_t)call.ota_source_id;
        }
    }
    return 0;
}

static uint32_t
dsd_audio_group_source_id_for_slot(const dsd_state* state, int slot, uint32_t ota_target, uint32_t policy_tg) {
    if (!state || slot < 0 || slot > 1) {
        return dsd_audio_group_source_id(state, policy_tg);
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.ota_source_id > UINT32_MAX) {
        return 0;
    }
    if (call.ota_target_id == ota_target || call.policy_target_id == policy_tg) {
        return (uint32_t)call.ota_source_id;
    }
    return dsd_audio_group_source_id(state, policy_tg);
}

static uint32_t
dsd_audio_p25_policy_target_for_slot(const dsd_state* state, int slot, uint32_t ota_target) {
    if (!dsd_audio_p25_policy_tg_valid_for_slot(state, slot, ota_target)) {
        return ota_target;
    }
    dsd_call_snapshot call;
    return dsd_call_state_get(state, (uint8_t)slot, &call) > 0 && call.policy_target_id <= UINT32_MAX
               ? (uint32_t)call.policy_target_id
               : ota_target;
}

static uint32_t
dsd_audio_p25_policy_target_for_group(const dsd_state* state, uint32_t ota_target) {
    if (!dsd_audio_state_is_p25(state)) {
        return ota_target;
    }
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        uint32_t policy_target = dsd_audio_p25_policy_target_for_slot(state, slot, ota_target);
        if (policy_target != ota_target) {
            return policy_target;
        }
    }
    return ota_target;
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

    const unsigned long long r_key = (slot == 0) ? state->R : state->RR;
    return (r_key != 0ULL || state->K != 0ULL || state->K1 != 0ULL) ? 1 : 0;
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
    return dsd_dmr_voice_alg_can_decrypt(algid, key->r_key, key->aes_loaded);
}

dsd_dmr_key_material
dsd_dmr_slot_key_material(const dsd_state* state, int slot, int key_id, int mapped) {
    dsd_dmr_key_material material = {0ULL, 0, 0};
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return material;
    }

    material.r_key = (slot == 0) ? state->R : state->RR;
    material.aes_loaded = state->aes_key_loaded[slot];
    material.kirisun_complete = dsd_dmr_kirisun_slot_key_complete(state, slot);
    if (mapped) {
        (void)keyring_kid_material(state, key_id, &material.r_key, &material.aes_loaded);
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
                                      dsd_dmr_kirisun_slot_key_complete(state, slot)};
    return dsd_dmr_voice_kid_can_decrypt(state, slot, algid, &key);
}

// The --dmr-force-algid value, or 0 when none is in force. state->M doubles as the scrambler
// key for 0/1 and as the 0x16 Hytera marker, neither of which is a forced ALG ID.
static int
dsd_dmr_forced_algid(const dsd_state* state) {
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

static int
dsd_p25p2_media_decision_allows_audio(const dsd_tg_policy_decision* decision) {
    if (!decision) {
        return 1;
    }
    if (decision->tg_hold_active && decision->tg_hold_match) {
        return 1;
    }
    if (!decision->audio_allowed || (decision->block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0u) {
        return 0;
    }
    return 1;
}

int
dsd_p25p2_decode_audio_allowed(const dsd_opts* opts, const dsd_state* state, int slot, int alg) {
    dsd_tg_policy_decision decision;
    uint32_t target = 0;
    uint32_t source = 0;

    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    if (!dsd_p25p2_slot_crypto_permits_audio(opts, state, slot, alg)) {
        return 0;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.ota_target_id > UINT32_MAX || call.ota_source_id > UINT32_MAX) {
        return 0;
    }
    target = (uint32_t)call.ota_target_id;
    source = (uint32_t)call.ota_source_id;
    if (call.kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        if (dsd_tg_policy_evaluate_private_call(opts, state, source, target, 0, 0, &decision) == 0) {
            return dsd_p25p2_media_decision_allows_audio(&decision);
        }
    } else {
        uint32_t policy_target = dsd_audio_p25_policy_target_for_slot(state, slot, target);
        if (dsd_tg_policy_evaluate_group_call(opts, state, policy_target, source, 0, 0, &decision) == 0) {
            return dsd_p25p2_media_decision_allows_audio(&decision);
        }
    }

    return 0;
}

static int
dsd_audio_group_gate_slot(const dsd_opts* opts, const dsd_state* state, int slot, unsigned long tg, int enc_in,
                          int* enc_out) {
    dsd_tg_policy_decision decision;
    uint32_t ota_tg = (uint32_t)tg;
    uint32_t policy_tg = 0;
    uint32_t source_id = 0;

    if (!opts || !state || !enc_out) {
        return -1;
    }

    int enc = (enc_in != 0) ? 1 : 0;
    if (slot >= 0 && slot <= 1) {
        policy_tg = dsd_audio_p25_policy_target_for_slot(state, slot, ota_tg);
        source_id = dsd_audio_group_source_id_for_slot(state, slot, ota_tg, policy_tg);
    } else {
        policy_tg = dsd_audio_p25_policy_target_for_group(state, ota_tg);
        source_id = dsd_audio_group_source_id(state, policy_tg);
    }

    if (dsd_tg_policy_evaluate_group_call(opts, state, policy_tg, source_id, 0, 0, &decision) == 0) {
        if (decision.tg_hold_active && decision.tg_hold_match) {
            enc = 0;
        } else if (!decision.audio_allowed || (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0u) {
            enc = 1;
        }
    }

    *enc_out = enc;
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

static int
dsd_audio_record_slot_allows_audio(const dsd_opts* opts, const dsd_state* state, int slot) {
    int enc = 0;
    int dmr_unmute_slot = 0;

    if (!opts || !state) {
        return 0;
    }

    if (DSD_SYNC_IS_P25P2(state->synctype)) {
        return (state->p25_p2_audio_allowed[slot] != 0) ? 1 : 0;
    }

    enc = (slot == 1) ? state->dmr_encR : state->dmr_encL;
    dmr_unmute_slot = (slot == 1) ? (opts->dmr_mute_encR == 0) : (opts->dmr_mute_encL == 0);
    if (opts->unmute_encrypted_p25 == 1 || enc == 0 || dmr_unmute_slot) {
        return 1;
    }
    return 0;
}

static int
dsd_audio_record_policy_evaluate(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                                 uint32_t ota_target, uint32_t policy_target, uint32_t source_id,
                                 dsd_tg_policy_decision* decision) {
    if (call->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return dsd_tg_policy_evaluate_private_call(opts, state, source_id, ota_target, 0, 0, decision);
    }
    return dsd_tg_policy_evaluate_group_call(opts, state, policy_target, source_id, 0, 0, decision);
}

static uint32_t
dsd_audio_record_policy_target(const dsd_state* state, const dsd_call_snapshot* call, int slot, uint32_t ota_target) {
    if (DSD_SYNC_IS_P25(call->protocol)) {
        return dsd_audio_p25_policy_target_for_slot(state, slot, ota_target);
    }
    if (call->policy_target_id != 0U && call->policy_target_id <= UINT32_MAX) {
        return (uint32_t)call->policy_target_id;
    }
    return ota_target;
}

static int
dsd_audio_record_policy_blocks(const dsd_opts* opts, const dsd_state* state, int slot) {
    dsd_tg_policy_decision decision;

    if (!opts || !state) {
        return 1;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.ota_target_id > UINT32_MAX || call.ota_source_id > UINT32_MAX) {
        return 0;
    }
    const uint32_t ota_target = (uint32_t)call.ota_target_id;
    const uint32_t policy_target = dsd_audio_record_policy_target(state, &call, slot, ota_target);
    const uint32_t source_id = (uint32_t)call.ota_source_id;
    int rc = dsd_audio_record_policy_evaluate(opts, state, &call, ota_target, policy_target, source_id, &decision);
    if (rc != 0) {
        return 0;
    }

    if (decision.tg_hold_active && decision.tg_hold_match) {
        return 0;
    }
    if (!decision.record_allowed || (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0u) {
        return 1;
    }
    return 0;
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
