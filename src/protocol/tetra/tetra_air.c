// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA trunked mode air interface decryption of speech with an operator's cipher key, EN 300 392-7 clause 6.
 */
#include <dsd-neo/protocol/tetra/tetra_air.h>

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"
#include "tetra_calls.h"
#include "tetra_state.h"

#define TETRA_AIR_SLOTS        4

/* A key stream segment covers at most 432 bits on a phase modulation channel (EN 300 392-7 clause 6.3.0). */
#define TETRA_AIR_KSS_MAX_BITS 432

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
    tetra_state_t* const ts = tetra_st(state);
    int slot;
    if (!ts || ksg > 3u) {
        return 0;
    }
    slot = tetra_air_slot(ts->air_ksg_valid, ts->air_ksg_mcc, ts->air_ksg_mnc, mcc, mnc);
    ts->air_ksg_valid[slot] = 1;
    ts->air_ksg_mcc[slot] = mcc;
    ts->air_ksg_mnc[slot] = mnc;
    ts->air_ksg[slot] = ksg;
    return 1;
}

int
tetra_air_ksg_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts) {
        return -1;
    }
    for (int i = 0; i < TETRA_AIR_SLOTS; i++) {
        if (ts->air_ksg_valid[i] && ts->air_ksg_mcc[i] == mcc && ts->air_ksg_mnc[i] == mnc) {
            return ts->air_ksg[i];
        }
    }
    return -1;
}

const char*
tetra_air_ksg_name(int ksg) {
    static const char* const k_names[] = {"TEA1", "TEA2", "TEA3", "TEA4"};
    return (ksg >= 0 && ksg < 4) ? k_names[ksg] : NULL;
}

uint32_t
tetra_air_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir) {
    /* The crypto wrapper packs the fields and rejects illegal ones. */
    return tetra_tea_build_iv(tn, fn, mn, hn, dir);
}

static const uint8_t*
tetra_air_key_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS; i++) {
        const dsd_tetra_air_key* key = &state->tetra_air_keys[i];
        if (key->loaded && key->mcc == mcc && key->mnc == mnc) {
            return key->key;
        }
    }
    return NULL;
}

int
tetra_air_key_usable(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || ts->dmo || !ts->net_known || !ts->sysinfo_known) {
        return 0;
    }
    /* TEA4 is named in the KSG number but its key stream generator is not available. */
    const int ksg = tetra_air_ksg_lookup(state, ts->mcc, ts->mnc);
    return ksg >= 0 && ksg <= 2 && tetra_air_key_lookup(state, ts->mcc, ts->mnc) != NULL;
}

/* The cipher key, algorithm and initial value for the current slot, or NULL when speech encrypted under @p enc_mode is
   not decrypted here. */
static const uint8_t*
tetra_air_slot_key(const dsd_state* state, const tetra_state_t* ts, uint8_t enc_mode, int* ksg, uint32_t* iv) {
    if (ts->dmo || !tetra_enc_mode_encrypted(enc_mode)) {
        return NULL;
    }
    if (!ts->net_known || !ts->sysinfo_known || !ts->hyperframe_valid || !ts->tdma_valid) {
        return NULL;
    }
    /* TEA4 is named in the KSG number but its key stream generator is not available. */
    *ksg = tetra_air_ksg_lookup(state, ts->mcc, ts->mnc);
    if (*ksg < 0 || *ksg > 2) {
        return NULL;
    }
    /* Every legal initial value is non-zero: the frame number is at least 1. */
    *iv = tetra_air_build_iv(ts->tn, ts->fn, ts->mn, ts->hyperframe, ts->uplink ? 1u : 0u);
    return *iv != 0u ? tetra_air_key_lookup(state, ts->mcc, ts->mnc) : NULL;
}

int
tetra_air_decrypt_voice(const dsd_opts* opts, const dsd_state* state, uint8_t enc_mode, uint8_t* bits, int nbits,
                        int ks_bit_offset) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    uint8_t ks[(TETRA_AIR_KSS_MAX_BITS + 7) / 8];
    uint8_t eck[10];
    int ksg = -1;
    uint32_t iv = 0;
    uint16_t cn = 0;
    if (!ts || !bits || nbits <= 0 || ks_bit_offset < 0 || ks_bit_offset + nbits > TETRA_AIR_KSS_MAX_BITS) {
        return 0;
    }
    const uint8_t* const ck = tetra_air_slot_key(state, ts, enc_mode, &ksg, &iv);
    if (!ck || !tetra_calls_carrier(opts, state, &cn)) {
        return 0;
    }
    /* The generator never takes the cipher key itself: TB5 binds it to this carrier, location area and colour code. */
    tetra_tea_derive_eck(cn, ts->la, (uint8_t)(ts->colour & 0x3Fu), ck, eck);
    const int ks_bytes = (ks_bit_offset + nbits + 7) / 8;
    DSD_MEMSET(ks, 0, sizeof(ks));
    tetra_tea_keystream(ksg, eck, iv, ks, ks_bytes);
    DSD_SECURE_ZERO(eck, sizeof(eck));
    for (int i = 0; i < nbits; i++) {
        const int bit = ks_bit_offset + i;
        const uint8_t kb = (uint8_t)((ks[bit / 8] >> (7 - (bit % 8))) & 1u);
        bits[i] = (uint8_t)((bits[i] & 1u) ^ kb);
    }
    DSD_SECURE_ZERO(ks, sizeof(ks));
    return 1;
}
