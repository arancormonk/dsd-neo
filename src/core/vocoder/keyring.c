// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/key_material.h>
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

int
keyring_kid_kirisun_complete(const dsd_state* state, int key_id) {
    if (state == NULL) {
        return 0;
    }
    // Deliberately built from the same two primitives keyring_activate_slot_with_kid() uses, in
    // the same order, rather than from an independently derived rule: this predicts what
    // dsd_dmr_kirisun_slot_key_complete() will report *after* that activation runs, and the two
    // can only stay in agreement if they read the same functions.
    //
    // keyring_aes_segments_complete(state, key_id, 4) is NOT this predicate -- it accepts an index
    // that is rkey_array_loaded but zero-valued, which activates as A_i == 0 and is incomplete.
    if (keyring_aes_segment_count(state, key_id) != 4U) {
        return 0;
    }
    for (size_t i = 0; i < 4U; i++) {
        if (keyring_rkey_value(state, key_id + k_aes_segment_offsets[i]) == 0ULL) {
            return 0;
        }
    }
    return 1;
}

void
keyring_activate_slot_with_kid(dsd_state* state, int slot, int key_id) {
    if (state == NULL || slot < 0 || slot > 1) {
        return;
    }
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

int
keyring_kid_material(const dsd_state* state, int key_id, unsigned long long* out_rkey, int* out_aes_loaded) {
    unsigned long long rkey = 0ULL;
    int aes_loaded = 0;

    if (state != NULL) {
        rkey = keyring_rkey_value(state, key_id);
        for (size_t i = 0; i < 4U; i++) {
            if (keyring_rkey_value(state, key_id + k_aes_segment_offsets[i]) != 0ULL) {
                aes_loaded = 1;
                break;
            }
        }
    }

    if (out_rkey != NULL) {
        *out_rkey = rkey;
    }
    if (out_aes_loaded != NULL) {
        *out_aes_loaded = aes_loaded;
    }
    return (rkey != 0ULL || aes_loaded != 0) ? 1 : 0;
}

// All of the first `count` AES segment cells non-zero. Distinct from
// keyring_aes_segments_complete(), which accepts a cell that is rkey_array_loaded but zero-valued:
// such a cell activates as A_i == 0, so accepting it here would let the map install a key the
// decryptability gate then rejects -- exactly the divergence this predicate exists to prevent.
static int
keyring_aes_segments_nonzero(const dsd_state* state, int key_id, unsigned int count) {
    if (state == NULL || count > 4U) {
        return 0;
    }
    for (unsigned int i = 0; i < count; i++) {
        if (keyring_rkey_value(state, key_id + k_aes_segment_offsets[i]) == 0ULL) {
            return 0;
        }
    }
    return 1;
}

int
keyring_kid_satisfies_need(const dsd_state* state, int key_id, dsd_key_material_need need) {
    if (state == NULL) {
        return 0;
    }
    switch (need) {
        case DSD_KEY_NEED_SCALAR: return keyring_rkey_value(state, key_id) != 0ULL ? 1 : 0;
        case DSD_KEY_NEED_AES_2: return keyring_aes_segments_nonzero(state, key_id, 2U);
        case DSD_KEY_NEED_AES_3: return keyring_aes_segments_nonzero(state, key_id, 3U);
        case DSD_KEY_NEED_AES_4: return keyring_aes_segments_nonzero(state, key_id, 4U);
        case DSD_KEY_NEED_QUARTET: return keyring_kid_kirisun_complete(state, key_id);
        case DSD_KEY_NEED_NONE: break;
    }
    return 0;
}

uint8_t
keyring_dmr_effective_kid(const dsd_state* state, uint32_t target, int target_is_group, uint8_t signaled_kid,
                          int* out_mapped) {
    if (out_mapped != NULL) {
        *out_mapped = 0;
    }
    if (state == NULL || !target_is_group || state->keyloader != 1 || state->dmr_tg_key_map_count <= 0) {
        return signaled_kid;
    }

    uint8_t kid = 0U;
    if (!keyring_dmr_tg_map_kid(state, target, &kid)) {
        return signaled_kid;
    }
    if (!keyring_kid_material(state, (int)kid, NULL, NULL)) {
        // Explicit intent still loses to reality here: see the header comment.
        return signaled_kid;
    }

    if (out_mapped != NULL) {
        *out_mapped = 1;
    }
    return kid;
}

// True when the slot's snapshot describes a live DMR group call whose talkgroup may be looked up.
// Three of these are load-bearing, not defensive: dsd_call_state_get() reports success for any
// non-zero epoch and dsd_call_state_end_ex() clears only the phase, so an ENDED epoch still carries
// the previous transmission's talkgroup; a private call puts the destination RADIO ID in
// ota_target_id, and DMR radio ids share the talkgroup's 24-bit space, so an unchecked match would
// key a unit call off a colliding row; and the protocol check keeps a resident non-DMR call from
// steering this DMR-only map when lastsynctype is the only thing still reading DMR.
//
// DSD_SYNC_NONE is admitted rather than rejected because it reads "protocol not observed yet",
// not "some other protocol". The voice path cannot arrive here with it: dsd_mbe.c's
// mark_vocoder_call_media_protocol_compatible() rejects DSD_SYNC_NONE before the snapshot is
// taken. The PI path is DMR-only by construction. So on both callers an unobserved protocol is
// a DMR call, and rejecting it would only drop the map on the first burst of one.
static int
keyring_dmr_tg_map_call_is_mappable(const dsd_call_snapshot* call) {
    return call->phase == DSD_CALL_PHASE_ACTIVE && call->kind == DSD_CALL_KIND_GROUP_VOICE
           && (call->protocol == DSD_SYNC_NONE || DSD_SYNC_IS_DMR(call->protocol)) && call->ota_target_id != 0U
           && call->ota_target_id <= UINT32_MAX;
}

uint8_t
keyring_dmr_kid_for_call(const dsd_state* state, const dsd_call_snapshot* call, uint8_t signaled_kid, int* out_mapped) {
    if (out_mapped != NULL) {
        *out_mapped = 0;
    }
    if (call == NULL || !keyring_dmr_tg_map_call_is_mappable(call)) {
        return signaled_kid;
    }
    return keyring_dmr_effective_kid(state, (uint32_t)call->ota_target_id, 1, signaled_kid, out_mapped);
}

// One notice per call epoch, not one per voice frame. dsd_call_state_get() only reports a hit
// for a non-zero epoch, so epoch 0 is the "never announced" sentinel and needs no valid flag.
// Shared by both notice variants below so the latch itself -- the thing that keeps either one
// from flooding the console (and corrupting the ncurses display) once per voice frame -- has a
// single implementation instead of two copies that could drift.
static int
keyring_dmr_tg_map_note_should_print(dsd_state* state, int slot, uint64_t epoch) {
    if (state->dmr_tg_key_note_epoch[slot] == epoch) {
        return 0;
    }
    state->dmr_tg_key_note_epoch[slot] = epoch;
    return 1;
}

static void
keyring_dmr_tg_map_note(dsd_state* state, int slot, uint64_t epoch, uint32_t tg, uint8_t kid) {
    if (!keyring_dmr_tg_map_note_should_print(state, slot, epoch)) {
        return;
    }
    DSD_FPRINTF(stderr, "\n Slot %d DMR TG Key Map: TG %u -> Key ID: %02X;", slot + 1, tg, kid);
}

// Announced when a row matched but resolved to nothing, so a CSV typo is visible rather than
// looking like the map simply did not cover the talkgroup.
static void
keyring_dmr_tg_map_note_skipped(dsd_state* state, int slot, uint64_t epoch, uint32_t tg, uint8_t mapped_kid,
                                uint8_t signaled_kid) {
    if (!keyring_dmr_tg_map_note_should_print(state, slot, epoch)) {
        return;
    }
    DSD_FPRINTF(stderr,
                "\n Slot %d DMR TG Key Map: TG %u -> Key ID: %02X has no imported key; using signaled Key ID: %02X;",
                slot + 1, tg, mapped_kid, signaled_kid);
}

uint8_t
keyring_dmr_slot_kid_for_call(dsd_state* state, int slot, const dsd_call_snapshot* call, uint8_t signaled_kid) {
    // call == NULL is a live input, not a defensive one: mbe_prepare_frame_state() passes NULL
    // whenever dsd_call_state_get() reports no snapshot for the slot. Rejecting it here rather
    // than mid-function is what lets both call-> dereferences below stand unguarded -- the mapped
    // branch could rely on mapped == 1 implying non-NULL, but the unmapped branch cannot.
    if (state == NULL || call == NULL || slot < 0 || slot > 1) {
        return signaled_kid;
    }

    int mapped = 0;
    const uint8_t kid = keyring_dmr_kid_for_call(state, call, signaled_kid, &mapped);
    if (mapped) {
        keyring_dmr_tg_map_note(state, slot, call->epoch, (uint32_t)call->ota_target_id, kid);
        return kid;
    }

    // Distinguish "no row for this talkgroup" from "row present, nothing imported behind it".
    uint8_t row_kid = 0U;
    if (keyring_dmr_tg_map_call_is_mappable(call) && state->keyloader == 1
        && keyring_dmr_tg_map_kid(state, (uint32_t)call->ota_target_id, &row_kid)) {
        keyring_dmr_tg_map_note_skipped(state, slot, call->epoch, (uint32_t)call->ota_target_id, row_kid, signaled_kid);
    }
    return signaled_kid;
}

void
keyring_activate_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (state == NULL || slot < 0 || slot > 1) {
        return;
    }
    keyring_activate_slot_with_kid(state, slot, (slot == 0) ? state->payload_keyid : state->payload_keyidR);
}
