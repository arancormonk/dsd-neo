// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/key_material.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

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

void
keyring_activate_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (state == NULL || slot < 0 || slot > 1) {
        return;
    }
    keyring_activate_slot_with_kid(state, slot, (slot == 0) ? state->payload_keyid : state->payload_keyidR);
}
