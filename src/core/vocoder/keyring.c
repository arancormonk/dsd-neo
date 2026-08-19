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

void
keyring_dmr_tg_map_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(state->dmr_tg_key_map_tg, 0, sizeof(state->dmr_tg_key_map_tg));
    DSD_MEMSET(state->dmr_tg_key_map_kid, 0, sizeof(state->dmr_tg_key_map_kid));
    state->dmr_tg_key_map_count = 0;
    state->dmr_tg_key_note_epoch[0] = state->dmr_tg_key_note_epoch[1] = 0U;
}

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

// True when the slot's snapshot describes a live DMR group call whose talkgroup may be looked up.
// Three of these are load-bearing, not defensive: dsd_call_state_get() reports success for any
// non-zero epoch and dsd_call_state_end_ex() clears only the phase, so an ENDED epoch still carries
// the previous transmission's talkgroup; a private call puts the destination RADIO ID in
// ota_target_id, and DMR radio ids share the talkgroup's 24-bit space, so an unchecked match would
// key a unit call off a colliding row; and the protocol check keeps a resident non-DMR call from
// steering this DMR-only map when lastsynctype is the only thing still reading DMR.
static int
keyring_dmr_tg_map_call_is_mappable(const dsd_call_snapshot* call) {
    return call->phase == DSD_CALL_PHASE_ACTIVE && call->kind == DSD_CALL_KIND_GROUP_VOICE
           && (call->protocol == DSD_SYNC_NONE || DSD_SYNC_IS_DMR(call->protocol)) && call->ota_target_id != 0U
           && call->ota_target_id <= UINT32_MAX;
}

// One notice per call epoch, not one per voice frame. dsd_call_state_get() only reports a hit for a
// non-zero epoch, so epoch 0 is the "never announced" sentinel and needs no companion valid flag.
static void
keyring_dmr_tg_map_note(dsd_state* state, int slot, uint64_t epoch, uint32_t tg, uint8_t kid, int has_key) {
    if (state->dmr_tg_key_note_epoch[slot] == epoch) {
        return;
    }
    state->dmr_tg_key_note_epoch[slot] = epoch;
    DSD_FPRINTF(stderr, "\n Slot %d DMR TG Key Map: TG %u -> Key ID: %02X;", slot + 1, tg, kid);
    if (!has_key) {
        // The override is explicit, so it still stands -- but without this the mapped-but-unimported
        // key id reads as a success while it silently zeroes the slot key and shadows the signaled one.
        DSD_FPRINTF(stderr, " no key imported for this key id;");
    }
}

int
keyring_dmr_tg_map_activate_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (!keyring_dmr_tg_map_slot_eligible(state, slot)) {
        return 0;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || !keyring_dmr_tg_map_call_is_mappable(&call)) {
        return 0;
    }

    uint8_t kid = 0U;
    if (!keyring_dmr_tg_map_kid(state, (uint32_t)call.ota_target_id, &kid)) {
        return 0;
    }

    keyring_activate_slot_with_kid(state, slot, (int)kid);
    const int has_key = ((slot == 0) ? state->R : state->RR) != 0ULL || state->aes_key_loaded[slot] != 0;
    keyring_dmr_tg_map_note(state, slot, call.epoch, (uint32_t)call.ota_target_id, kid, has_key);
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
