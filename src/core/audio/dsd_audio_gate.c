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

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static uint32_t
dsd_audio_group_source_id(const dsd_state* state, unsigned long tg) {
    uint32_t id = (uint32_t)tg;
    if (!state) {
        return 0;
    }
    if (state->lasttg >= 0 && (uint32_t)state->lasttg == id) {
        return state->lastsrc;
    }
    if (state->lasttgR >= 0 && (uint32_t)state->lasttgR == id) {
        return state->lastsrcR;
    }
    return 0;
}

int
dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded) {
    switch (algid) {
        // RC4/DES-style families keyed from 40/56-bit key material.
        case 0x02: // Hytera Enhanced
        case 0x21: // DMR RC4
        case 0x22: // DMR DES
        case 0x81: // P25 DES
        case 0x9F: // P25 DES-XL
        case 0xAA: // P25 RC4
            return (r_key != 0ULL) ? 1 : 0;

        // AES/TDEA-style families keyed from loaded AES key segments.
        case 0x24: // DMR AES-128
        case 0x25: // DMR AES-256
        case 0x36: // Kirisun Advanced
        case 0x37: // Kirisun Universal
        case 0x83: // P25 TDEA
        case 0x84: // P25 AES-256
        case 0x89: // P25 AES-128
            return (aes_loaded == 1) ? 1 : 0;

        default: return 0;
    }
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
        if (dsd_tg_policy_evaluate_group_call(opts, state, target, source, 0, 0, DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY,
                                              &decision)
            == 0) {
            return dsd_p25p2_media_decision_allows_audio(&decision);
        }
    }

    return 0;
}

int
dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out) {
    dsd_tg_policy_decision decision;
    uint32_t source_id = 0;

    if (!opts || !state || !enc_out) {
        return -1;
    }

    int enc = (enc_in != 0) ? 1 : 0;
    source_id = dsd_audio_group_source_id(state, tg);

    if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)tg, source_id, 0, 0,
                                          DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY, &decision)
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
dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                          int encL_in, int encR_in, int* encL_out, int* encR_out) {
    if (!encL_out || !encR_out) {
        return -1;
    }
    int rc = 0;
    rc |= dsd_audio_group_gate_mono(opts, state, tgL, encL_in, encL_out);
    rc |= dsd_audio_group_gate_mono(opts, state, tgR, encR_in, encR_out);
    return rc;
}

int
dsd_audio_record_gate_mono(const dsd_opts* opts, const dsd_state* state, int* allow_out) {
    dsd_tg_policy_decision decision;

    if (!opts || !state || !allow_out) {
        return -1;
    }

    const int slot = (state->currentslot == 1) ? 1 : 0;
    int allow = 0;
    if (DSD_SYNC_IS_P25P2(state->synctype)) {
        allow = (state->p25_p2_audio_allowed[slot] != 0) ? 1 : 0;
    } else {
        const int enc = (slot == 1) ? state->dmr_encR : state->dmr_encL;
        const int dmr_unmute_slot = (slot == 1) ? (opts->dmr_mute_encR == 0) : (opts->dmr_mute_encL == 0);
        allow = (opts->unmute_encrypted_p25 == 1 || enc == 0 || dmr_unmute_slot) ? 1 : 0;
    }

    if (allow) {
        const unsigned long tg = (slot == 1) ? (unsigned long)state->lasttgR : (unsigned long)state->lasttg;
        const uint32_t source_id = (slot == 1) ? state->lastsrcR : state->lastsrc;
        if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)tg, source_id, 0, 0,
                                              DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY, &decision)
            == 0) {
            int blocked = (!decision.record_allowed || (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0u);
            if (decision.tg_hold_active && decision.tg_hold_match) {
                blocked = 0;
            }
            if (blocked) {
                allow = 0;
            }
        }
    }

    *allow_out = allow;
    return 0;
}
