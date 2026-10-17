// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
p25_crypto_reset_slot(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }

    if (slot == 0) {
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->payload_miP = 0ULL;
        state->p25_p1_identity_epoch_started = 0;
        state->p25_p1_hdu_crypto_fresh = 0;
        p25_crypto_clear_phase1_lockout_epoch(state);
        DSD_MEMSET(&state->p25_p1_crypto_conflict, 0, sizeof(state->p25_p1_crypto_conflict));
    } else {
        state->payload_algidR = 0;
        state->payload_keyidR = 0;
        state->payload_miN = 0ULL;
    }
    state->p25_crypto_inherited[slot] = 0U;
    state->p25_crypto_state[slot] = DSD_P25_CRYPTO_UNKNOWN;
    DSD_MEMSET(&state->p25_p2_rekey[slot], 0, sizeof(state->p25_p2_rekey[slot]));
    DSD_MEMSET(&state->p25_p2_crypto_conflict[slot], 0, sizeof(state->p25_p2_crypto_conflict[slot]));
    state->p25_p2_audio_allowed[slot] = 0;

    if (slot == 0) {
        state->DMRvcL = 0;
        state->bit_counterL = 0;
        state->dropL = 256;
        state->p25vc = 0;
        state->octet_counter = 0;
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    } else {
        state->DMRvcR = 0;
        state->bit_counterR = 0;
        state->dropR = 256;
        DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
        DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    }

    if (state->keyloader == 1) {
        state->scalar_key_present[slot] = 0;
        if (slot == 0) {
            state->R = 0ULL;
        } else {
            state->RR = 0ULL;
        }
        state->A1[slot] = 0ULL;
        state->A2[slot] = 0ULL;
        state->A3[slot] = 0ULL;
        state->A4[slot] = 0ULL;
        state->aes_key_loaded[slot] = 0;
        state->aes_key_segments[slot] = 0U;
    }
}

static int
p25_crypto_inherited_slot_valid(const dsd_state* state, int slot) {
    return state && slot >= 0 && slot <= 1;
}

void
p25_crypto_note_inherited(dsd_state* state, int slot) {
    if (!p25_crypto_inherited_slot_valid(state, slot)) {
        return;
    }
    state->p25_crypto_inherited[slot] = 1U;
    state->p25_crypto_inherited_algid[slot] = slot == 0 ? state->payload_algid : state->payload_algidR;
    state->p25_crypto_inherited_keyid[slot] = slot == 0 ? state->payload_keyid : state->payload_keyidR;
}

int
p25_crypto_live_is_inherited(dsd_state* state, int slot) {
    if (!p25_crypto_inherited_slot_valid(state, slot) || !state->p25_crypto_inherited[slot]) {
        return 0;
    }
    const int algid = slot == 0 ? state->payload_algid : state->payload_algidR;
    const int keyid = slot == 0 ? state->payload_keyid : state->payload_keyidR;
    if (algid == state->p25_crypto_inherited_algid[slot] && keyid == state->p25_crypto_inherited_keyid[slot]) {
        return 1;
    }
    state->p25_crypto_inherited[slot] = 0U;
    return 0;
}

dsd_call_crypto_state
p25_crypto_carried_classification(const dsd_state* state, int slot) {
    dsd_call_snapshot call;
    if (!state || slot < 0 || slot > 1 || dsd_call_state_get(state, (uint8_t)slot, &call) <= 0
        || !call.has_service_metadata) {
        return DSD_CALL_CRYPTO_UNKNOWN;
    }
    return (call.service_options & 0x40U) != 0U ? DSD_CALL_CRYPTO_ENCRYPTED_PENDING : DSD_CALL_CRYPTO_CLEAR;
}
