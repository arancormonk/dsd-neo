// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const int k_aes_segment_offsets[4] = {0x000, 0x101, 0x201, 0x301};

static int
keyring_rkey_index_valid(const dsd_state* state, int index) {
    return state != NULL && index >= 0 && (size_t)index < (sizeof(state->rkey_array) / sizeof(state->rkey_array[0]));
}

static uint8_t
keyring_aes_segment_count(const dsd_state* state, int key_id) {
    uint8_t present = 0U;
    uint8_t nonzero = 0U;

    for (size_t i = 0; i < 4U; i++) {
        const int index = key_id + k_aes_segment_offsets[i];
        if (!keyring_rkey_index_valid(state, index)) {
            continue;
        }
        if (state->rkey_array_loaded[index] != 0U) {
            present++;
        }
        if (state->rkey_array[index] != 0ULL) {
            nonzero++;
        }
    }

    return present != 0U ? present : nonzero;
}

int
keyring_aes_segments_complete(const dsd_state* state, int key_id, unsigned int required_segments) {
    if (!state || required_segments > 4U) {
        return 0;
    }
    for (unsigned int i = 0; i < required_segments; i++) {
        const int index = key_id + k_aes_segment_offsets[i];
        if (!keyring_rkey_index_valid(state, index)
            || (state->rkey_array_loaded[index] == 0U && state->rkey_array[index] == 0ULL)) {
            return 0;
        }
    }
    return 1;
}

static unsigned long long int
keyring_rkey_value(const dsd_state* state, int index) {
    return keyring_rkey_index_valid(state, index) ? state->rkey_array[index] : 0ULL;
}

static void keyring_activate_slot_with_kid(dsd_state* state, int slot, int key_id);

int
keyring_dmr_tg_map_kid(const dsd_state* state, uint32_t tg, uint8_t* out_kid) {
    if (!state || !out_kid || tg == 0U) {
        return 0;
    }
    int count = state->dmr_tg_key_map_count;
    if (count > DSD_DMR_TG_KEY_MAP_MAX) {
        count = DSD_DMR_TG_KEY_MAP_MAX;
    }
    for (int i = 0; i < count; i++) {
        if (state->dmr_tg_key_map_tg[i] == tg) {
            *out_kid = state->dmr_tg_key_map_kid[i];
            return 1;
        }
    }
    return 0;
}

static int
keyring_dmr_tg_map_slot_eligible(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1 || state->dmr_tg_key_map_count <= 0 || state->keyloader != 1) {
        return 0;
    }
    if (!DSD_SYNC_IS_DMR(state->synctype) && !DSD_SYNC_IS_DMR(state->lastsynctype)) {
        return 0;
    }
    // Same gate as the signaled-KID activation in mbe_prepare_frame_state: no ALG ID means
    // the basic-privacy TG autoload owns the slot, and 0x80 stays with the scrambler path.
    const int algid = (slot == 0) ? state->payload_algid : state->payload_algidR;
    return algid != 0 && algid != 0x80;
}

// One notice per call epoch, not one per voice frame.
static void
keyring_dmr_tg_map_note(dsd_state* state, int slot, uint64_t epoch, uint32_t tg, uint8_t kid) {
    if (state->dmr_tg_key_note_valid[slot] != 0U && state->dmr_tg_key_note_epoch[slot] == epoch) {
        return;
    }
    state->dmr_tg_key_note_epoch[slot] = epoch;
    state->dmr_tg_key_note_valid[slot] = 1U;
    DSD_FPRINTF(stderr, "\n Slot %d DMR TG Key Map: TG %u -> Key ID: %02X;", slot + 1, tg, kid);
}

int
keyring_dmr_tg_map_activate_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (!keyring_dmr_tg_map_slot_eligible(state, slot)) {
        return 0;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.ota_target_id == 0U
        || call.ota_target_id > UINT32_MAX) {
        return 0;
    }

    uint8_t kid = 0U;
    if (!keyring_dmr_tg_map_kid(state, (uint32_t)call.ota_target_id, &kid)) {
        return 0;
    }

    keyring_activate_slot_with_kid(state, slot, (int)kid);
    keyring_dmr_tg_map_note(state, slot, call.epoch, (uint32_t)call.ota_target_id, kid);
    return 1;
}

void
keyring_activate_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    keyring_activate_slot_with_kid(state, slot, (slot == 0) ? state->payload_keyid : state->payload_keyidR);
}

static void
keyring_activate_slot_with_kid(dsd_state* state, int slot, int key_id) {
    const unsigned long long int scalar_key = keyring_rkey_value(state, key_id);
    if (slot == 0) {
        state->R = scalar_key;
    } else {
        state->RR = scalar_key;
    }

    state->A1[slot] = keyring_rkey_value(state, key_id + k_aes_segment_offsets[0]);
    state->A2[slot] = keyring_rkey_value(state, key_id + k_aes_segment_offsets[1]);
    state->A3[slot] = keyring_rkey_value(state, key_id + k_aes_segment_offsets[2]);
    state->A4[slot] = keyring_rkey_value(state, key_id + k_aes_segment_offsets[3]);
    state->aes_key_segments[slot] = keyring_aes_segment_count(state, key_id);
    state->aes_key_loaded[slot] =
        (state->A1[slot] != 0ULL || state->A2[slot] != 0ULL || state->A3[slot] != 0ULL || state->A4[slot] != 0ULL) ? 1
                                                                                                                   : 0;
}
