// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
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

void
keyring_activate_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    if (!state || slot < 0 || slot > 1) {
        return;
    }

    const int key_id = (slot == 0) ? state->payload_keyid : state->payload_keyidR;
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
