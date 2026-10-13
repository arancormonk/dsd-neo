// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/protocol/tetra/tetra_air.h>

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <string.h>

#define TETRA_AIR_SLOTS 4

static int
tetra_air_slot(const uint8_t* valid, const uint16_t* mcc, const uint16_t* mnc, uint16_t want_mcc, uint16_t want_mnc) {
    int free_slot = -1;
    for (int i = 0; i < TETRA_AIR_SLOTS; i++) {
        if (!valid[i]) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (mcc[i] == want_mcc && mnc[i] == want_mnc) {
            return i;
        }
    }
    return free_slot >= 0 ? free_slot : 0;
}

int
tetra_air_ksg_remember(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg) {
    int slot;
    if (!state || ksg > 3u) {
        return 0;
    }
    slot = tetra_air_slot(state->tetra_air_ksg_valid, state->tetra_air_ksg_mcc, state->tetra_air_ksg_mnc, mcc, mnc);
    state->tetra_air_ksg_valid[slot] = 1;
    state->tetra_air_ksg_mcc[slot] = mcc;
    state->tetra_air_ksg_mnc[slot] = mnc;
    state->tetra_air_ksg[slot] = ksg;
    return 1;
}

int
tetra_air_ksg_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    if (!state) {
        return -1;
    }
    for (int i = 0; i < TETRA_AIR_SLOTS; i++) {
        if (state->tetra_air_ksg_valid[i] && state->tetra_air_ksg_mcc[i] == mcc && state->tetra_air_ksg_mnc[i] == mnc) {
            return state->tetra_air_ksg[i];
        }
    }
    return -1;
}

const char*
tetra_air_ksg_name(int ksg) {
    switch (ksg) {
        case 0: return "TEA1";
        case 1: return "TEA2";
        case 2: return "TEA3";
        case 3: return "TEA4";
        default: return NULL;
    }
}

int
tetra_air_key_set(dsd_state* state, uint16_t mcc, uint16_t mnc, const uint8_t key[10]) {
    int slot;
    if (!state || !key) {
        return 0;
    }
    slot = tetra_air_slot(state->tetra_air_key_valid, state->tetra_air_key_mcc, state->tetra_air_key_mnc, mcc, mnc);
    state->tetra_air_key_valid[slot] = 1;
    state->tetra_air_key_mcc[slot] = mcc;
    state->tetra_air_key_mnc[slot] = mnc;
    memcpy(state->tetra_air_key[slot], key, 10);
    return 1;
}

uint32_t
tetra_air_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir) {
    /* Packing is build_iv() in TETRA_crypto. Reject illegal fields first so
     * that helper's asserts are not reached. */
    return tetra_tea_build_iv(tn, fn, mn, hn, dir);
}

static const uint8_t*
tetra_air_key_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    for (int i = 0; i < TETRA_AIR_SLOTS; i++) {
        if (state->tetra_air_key_valid[i] && state->tetra_air_key_mcc[i] == mcc && state->tetra_air_key_mnc[i] == mnc) {
            return state->tetra_air_key[i];
        }
    }
    return NULL;
}

int
tetra_air_decrypt_voice(dsd_state* state, uint8_t* bits, int nbits, int ks_bit_offset) {
    int ksg;
    const uint8_t* key;
    uint32_t iv;
    int ks_bits;
    uint8_t ks[64];
    if (!state || !bits || nbits <= 0 || ks_bit_offset < 0 || state->tetra_dmo) {
        return 0;
    }
    if (state->tetra_enc_mode != TETRA_ENC_MODE_ON && state->tetra_enc_mode != TETRA_ENC_MODE_ON_AUTH) {
        return 0;
    }
    if (!state->tetra_net_known || !state->tetra_hyperframe_valid || !state->tetra_tdma_valid) {
        return 0;
    }
    ksg = tetra_air_ksg_lookup(state, state->tetra_mcc, state->tetra_mnc);
    if (ksg < 0 || ksg > 2) {
        return 0;
    }
    key = tetra_air_key_lookup(state, state->tetra_mcc, state->tetra_mnc);
    if (!key) {
        return 0;
    }
    iv = tetra_air_build_iv(state->tetra_tn, state->tetra_fn, state->tetra_mn, state->tetra_hyperframe,
                            state->tetra_uplink ? 1u : 0u);
    /* A legal frame number is at least 1, so a valid IV cannot be zero. */
    if (iv == 0) {
        return 0;
    }
    ks_bits = ks_bit_offset + nbits;
    if (ks_bits <= 0 || (ks_bits + 7) / 8 > (int)sizeof(ks)) {
        return 0;
    }
    memset(ks, 0, sizeof(ks));
    tetra_tea_keystream(ksg, key, iv, ks, (ks_bits + 7) / 8);
    for (int i = 0; i < nbits; i++) {
        int bit = ks_bit_offset + i;
        uint8_t kb = (uint8_t)((ks[bit / 8] >> (7 - (bit % 8))) & 1u);
        bits[i] = (uint8_t)((bits[i] & 1u) ^ kb);
    }
    return 1;
}
