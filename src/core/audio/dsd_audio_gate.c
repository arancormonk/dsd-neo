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
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
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
    if (!dsd_audio_state_is_p25(state) || slot < 0 || slot > 1) {
        return 0;
    }

    int raw_target = (slot == 0) ? state->lasttg : state->lasttgR;
    if (raw_target <= 0 || (uint32_t)raw_target != ota_target) {
        return 0;
    }

    return dsd_audio_p25_patch_member_active(state, ota_target, state->p25_policy_tg[slot]);
}

static uint32_t
dsd_audio_group_source_id(const dsd_state* state, unsigned long tg) {
    uint32_t id = (uint32_t)tg;
    if (!state) {
        return 0;
    }
    if (state->lasttg > 0 && state->p25_policy_tg[0] == id
        && dsd_audio_p25_policy_tg_valid_for_slot(state, 0, (uint32_t)state->lasttg)) {
        return (uint32_t)state->lastsrc;
    }
    if (state->lasttgR > 0 && state->p25_policy_tg[1] == id
        && dsd_audio_p25_policy_tg_valid_for_slot(state, 1, (uint32_t)state->lasttgR)) {
        return (uint32_t)state->lastsrcR;
    }
    if (state->lasttg >= 0 && (uint32_t)state->lasttg == id) {
        return state->lastsrc;
    }
    if (state->lasttgR >= 0 && (uint32_t)state->lasttgR == id) {
        return state->lastsrcR;
    }
    return 0;
}

static uint32_t
dsd_audio_group_source_id_for_slot(const dsd_state* state, int slot, uint32_t ota_target, uint32_t policy_tg) {
    if (!state || slot < 0 || slot > 1) {
        return dsd_audio_group_source_id(state, policy_tg);
    }

    int raw_target = (slot == 0) ? state->lasttg : state->lasttgR;
    int raw_source = (slot == 0) ? state->lastsrc : state->lastsrcR;
    if (raw_source <= 0) {
        return 0;
    }
    if (raw_target >= 0 && (uint32_t)raw_target == ota_target) {
        return (uint32_t)raw_source;
    }
    if (raw_target >= 0 && (uint32_t)raw_target == policy_tg) {
        return (uint32_t)raw_source;
    }
    return dsd_audio_group_source_id(state, policy_tg);
}

static uint32_t
dsd_audio_p25_policy_target_for_slot(const dsd_state* state, int slot, uint32_t ota_target) {
    if (!dsd_audio_p25_policy_tg_valid_for_slot(state, slot, ota_target)) {
        return ota_target;
    }
    return state->p25_policy_tg[slot];
}

static uint32_t
dsd_audio_p25_policy_target_for_group(const dsd_state* state, uint32_t ota_target) {
    if (!dsd_audio_state_is_p25(state)) {
        return ota_target;
    }
    if (state->lasttg > 0 && (uint32_t)state->lasttg == ota_target
        && dsd_audio_p25_policy_tg_valid_for_slot(state, 0, ota_target)) {
        return state->p25_policy_tg[0];
    }
    if (state->lasttgR > 0 && (uint32_t)state->lasttgR == ota_target
        && dsd_audio_p25_policy_tg_valid_for_slot(state, 1, ota_target)) {
        return state->p25_policy_tg[1];
    }
    return ota_target;
}

static int
dsd_alg_list_contains(const uint8_t* algs, size_t count, int algid) {
    size_t i = 0;
    if (!algs) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (algid == (int)algs[i]) {
            return 1;
        }
    }
    return 0;
}

int
dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded) {
    static const uint8_t kRKeyAlgs[] = {
        0x02, // Hytera Enhanced
        0x21, // DMR RC4
        0x22, // DMR DES
        0x81, // P25 DES
        0x9F, // P25 DES-XL
        0xAA  // P25 RC4
    };
    static const uint8_t kAesLoadedAlgs[] = {
        0x24, // DMR AES-128
        0x25, // DMR AES-256
        0x83, // P25 TDEA
        0x84, // P25 AES-256
        0x89  // P25 AES-128
    };

    if (dsd_alg_list_contains(kRKeyAlgs, sizeof(kRKeyAlgs) / sizeof(kRKeyAlgs[0]), algid)) {
        return (r_key != 0ULL) ? 1 : 0;
    }
    if (dsd_alg_list_contains(kAesLoadedAlgs, sizeof(kAesLoadedAlgs) / sizeof(kAesLoadedAlgs[0]), algid)) {
        return (aes_loaded == 1) ? 1 : 0;
    }
    return 0;
}

static int
dsd_dmr_slot_valid(int slot) {
    return slot == 0 || slot == 1;
}

static int
dsd_dmr_kirisun_key_complete(const dsd_state* state, int slot) {
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
dsd_dmr_voice_slot_can_decrypt(const dsd_state* state, int slot, int algid, unsigned long long r_key) {
    if (!state || !dsd_dmr_slot_valid(slot)) {
        return 0;
    }
    if (algid == 0x36 || algid == 0x37) {
        return dsd_dmr_kirisun_key_complete(state, slot);
    }
    return dsd_dmr_voice_alg_can_decrypt(algid, r_key, state->aes_key_loaded[slot]);
}

int
dsd_dmr_apply_forced_algid(dsd_state* state) {
    if (!state || state->M <= 1 || state->M == 0x16) {
        return 0;
    }

    if (state->currentslot == 0 && (state->dmr_so & 0x40) != 0) {
        state->payload_algid = state->M & 0xFF;
        state->payload_keyid = 0xFF;
        return 1;
    }
    if (state->currentslot == 1 && (state->dmr_soR & 0x40) != 0) {
        state->payload_algidR = state->M & 0xFF;
        state->payload_keyidR = 0xFF;
        return 1;
    }

    return 0;
}

int
dsd_p25p2_mixer_gate(const dsd_state* state, int* encL, int* encR) {
    if (!state || !encL || !encR) {
        return -1;
    }
    *encL = state->p25_p2_audio_allowed[0] ? 0 : 1;
    *encR = state->p25_p2_audio_allowed[1] ? 0 : 1;
    return 0;
}

static int
dsd_p25p2_slot_can_decrypt(const dsd_state* state, int slot, int alg) {
    unsigned long long key = 0;
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    if (alg == 0 || alg == 0x80) {
        return 1;
    }
    key = (slot == 0) ? state->R : state->RR;
    if ((alg == 0xAA || alg == 0x81 || alg == 0x9F) && key != 0ULL) {
        return 1;
    }
    if ((alg == 0x84 || alg == 0x89) && state->aes_key_loaded[slot] == 1) {
        return 1;
    }
    return 0;
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
    int raw_target = 0;
    int raw_source = 0;
    uint32_t target = 0;
    uint32_t source = 0;

    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    if (!dsd_p25p2_slot_can_decrypt(state, slot, alg)) {
        return 0;
    }

    raw_target = (slot == 0) ? state->lasttg : state->lasttgR;
    raw_source = (slot == 0) ? state->lastsrc : state->lastsrcR;
    target = (raw_target > 0) ? (uint32_t)raw_target : 0u;
    source = (raw_source > 0) ? (uint32_t)raw_source : 0u;
    if (state->gi[slot] == 1) {
        if (dsd_tg_policy_evaluate_private_call(opts, state, source, target, 0, 0,
                                                DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY, &decision)
            == 0) {
            return dsd_p25p2_media_decision_allows_audio(&decision);
        }
    } else {
        uint32_t policy_target = dsd_audio_p25_policy_target_for_slot(state, slot, target);
        if (dsd_tg_policy_evaluate_group_call(opts, state, policy_target, source, 0, 0,
                                              DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY, &decision)
            == 0) {
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

    if (dsd_tg_policy_evaluate_group_call(opts, state, policy_tg, source_id, 0, 0, DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY,
                                          &decision)
        == 0) {
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
dsd_audio_record_policy_blocks(const dsd_opts* opts, const dsd_state* state, int slot) {
    dsd_tg_policy_decision decision;
    unsigned long tg = 0;
    uint32_t source_id = 0;

    if (!opts || !state) {
        return 1;
    }

    tg = (slot == 1) ? (unsigned long)state->lasttgR : (unsigned long)state->lasttg;
    tg = (unsigned long)dsd_audio_p25_policy_target_for_slot(state, slot, (uint32_t)tg);
    source_id = (slot == 1) ? state->lastsrcR : state->lastsrc;
    if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)tg, source_id, 0, 0,
                                          DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY, &decision)
        != 0) {
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
