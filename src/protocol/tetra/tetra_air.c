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

/* A key stream segment covers at most 432 bits on a phase modulation channel (EN 300 392-7 clause 6.3.0). */
#define TETRA_AIR_KSS_MAX_BITS 432

/* KSG numbers 4 to 7 are reserved (EN 300 392-7 table 6.2); 8 to 15 are proprietary. */
static int
tetra_air_ksg_defined(uint8_t ksg) {
    return ksg < 4u || (ksg > 7u && ksg < 16u);
}

/* The evidence entry of the network, or -1. */
static int
tetra_air_ksg_find(const tetra_state_t* ts, uint16_t mcc, uint16_t mnc) {
    for (int i = 0; i < TETRA_AIR_KSG_NETS; i++) {
        if (ts->air_ksg[i].valid && ts->air_ksg[i].mcc == mcc && ts->air_ksg[i].mnc == mnc) {
            return i;
        }
    }
    return -1;
}

/* Whether an air key of either kind is loaded for the network. */
static int
tetra_air_network_keyed(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS; i++) {
        const dsd_tetra_air_key* key = &state->tetra_air_keys[i];
        if (key->loaded && key->mcc == mcc && key->mnc == mnc) {
            return 1;
        }
    }
    return 0;
}

/* A free entry, else the oldest one of a network no air key is loaded for; NULL when every entry is keyed (more networks
 * than the key table holds, so only after the keys changed). */
static tetra_air_ksg_net*
tetra_air_ksg_entry(const dsd_state* state, tetra_state_t* ts) {
    tetra_air_ksg_net* oldest = NULL;
    for (int i = 0; i < TETRA_AIR_KSG_NETS; i++) {
        tetra_air_ksg_net* const e = &ts->air_ksg[i];
        if (!e->valid) {
            return e;
        }
        if (!tetra_air_network_keyed(state, e->mcc, e->mnc) && (!oldest || e->seq < oldest->seq)) {
            oldest = e;
        }
    }
    return oldest;
}

/* The network's evidence entry, made when it has none; NULL for a reserved KSG or a full cache. */
static tetra_air_ksg_net*
tetra_air_ksg_note(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !tetra_air_ksg_defined(ksg)) {
        return NULL;
    }
    const int found = tetra_air_ksg_find(ts, mcc, mnc);
    tetra_air_ksg_net* e = found >= 0 ? &ts->air_ksg[found] : NULL;
    if (!e) {
        e = tetra_air_ksg_entry(state, ts);
        if (!e) {
            return NULL;
        }
        DSD_MEMSET(e, 0, sizeof(*e));
        e->valid = 1;
        e->mcc = mcc;
        e->mnc = mnc;
        e->seq = ++ts->air_ksg_seq;
    }
    return e;
}

int
tetra_air_ksg_remember(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg) {
    tetra_air_ksg_net* const e = tetra_air_ksg_note(state, mcc, mnc, ksg);
    if (!e) {
        return 0;
    }
    e->mask = (uint16_t)(e->mask | (1u << ksg));
    return 1;
}

int
tetra_air_ksg_request(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg) {
    tetra_air_ksg_net* const e = tetra_air_ksg_note(state, mcc, mnc, ksg);
    if (!e) {
        return 0;
    }
    e->requested = (uint16_t)(e->requested | (1u << ksg));
    return 1;
}

uint16_t
tetra_air_ksg_seen(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    const int found = ts ? tetra_air_ksg_find(ts, mcc, mnc) : -1;
    return found >= 0 ? ts->air_ksg[found].mask : 0U;
}

uint16_t
tetra_air_ksg_requested(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    const int found = ts ? tetra_air_ksg_find(ts, mcc, mnc) : -1;
    return found >= 0 ? ts->air_ksg[found].requested : 0U;
}

int
tetra_air_ksg_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc) {
    const uint16_t mask = tetra_air_ksg_seen(state, mcc, mnc);
    if (mask == 0U || (mask & (mask - 1U)) != 0U) {
        return -1;
    }
    int ksg = 0;
    while (((mask >> ksg) & 1u) == 0u) {
        ksg++;
    }
    return ksg;
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

/* The loaded key of @p kind for the network being received: for a TEA1 reduced key, the one of @p carrier in its
 * location area and colour code. NULL when none is loaded. */
static const dsd_tetra_air_key*
tetra_air_find_key(const dsd_state* state, const tetra_state_t* ts, uint8_t kind, int carrier) {
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS; i++) {
        const dsd_tetra_air_key* const key = &state->tetra_air_keys[i];
        if (!key->loaded || key->kind != kind || key->mcc != ts->mcc || key->mnc != ts->mnc) {
            continue;
        }
        if (kind != DSD_TETRA_AIR_KEY_TEA1_REDUCED
            || (key->carrier == (uint16_t)carrier && key->la == ts->la && key->cc == (uint8_t)(ts->colour & 0x3Fu))) {
            return key;
        }
    }
    return NULL;
}

/* The key that decrypts @p carrier (-1 unknown) of the cell being received, and its KSG number 0..2
 * (tetra_air_key_for_carrier()). */
static const dsd_tetra_air_key*
tetra_air_resolve(const dsd_state* state, const tetra_state_t* ts, int carrier, int* ksg) {
    if (!ts || ts->dmo || !ts->net_known || !ts->sysinfo_known) {
        return NULL;
    }
    const dsd_tetra_air_key* const reduced =
        carrier >= 0 ? tetra_air_find_key(state, ts, DSD_TETRA_AIR_KEY_TEA1_REDUCED, carrier) : NULL;
    if (reduced) {
        *ksg = 0;
        return reduced;
    }
    const dsd_tetra_air_key* const ck = tetra_air_find_key(state, ts, DSD_TETRA_AIR_KEY_CK, carrier);
    if (!ck) {
        return NULL;
    }
    /* TEA4 is named in the KSG number but its key stream generator is not available. */
    *ksg = ck->alg >= 1U && ck->alg <= 3U ? (int)ck->alg - 1 : tetra_air_ksg_lookup(state, ts->mcc, ts->mnc);
    return (*ksg >= 0 && *ksg <= 2) ? ck : NULL;
}

int
tetra_air_key_for_carrier(const dsd_state* state, int carrier, uint8_t* kind, uint8_t* tea) {
    int ksg = -1;
    const dsd_tetra_air_key* const key = state ? tetra_air_resolve(state, tetra_st_peek(state), carrier, &ksg) : NULL;
    if (!key) {
        return 0;
    }
    if (kind) {
        *kind = key->kind;
    }
    if (tea) {
        *tea = (uint8_t)(ksg + 1);
    }
    return 1;
}

int
tetra_air_key_usable_on(const dsd_state* state, int carrier) {
    return tetra_air_key_for_carrier(state, carrier, NULL, NULL);
}

/* Fill @p ks with @p ks_bytes of the key stream @p key makes on carrier @p cn at initial value @p iv. */
static void
tetra_air_key_stream(const tetra_state_t* ts, const dsd_tetra_air_key* key, int ksg, uint16_t cn, uint32_t iv,
                     uint8_t* ks, int ks_bytes) {
    if (key->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED) {
        /* The register TEA1 compresses its ECK into: TB5 and the compression already happened where it was found. */
        uint32_t reg = ((uint32_t)key->key[0] << 24) | ((uint32_t)key->key[1] << 16) | ((uint32_t)key->key[2] << 8)
                       | (uint32_t)key->key[3];
        tetra_tea1_keystream_reduced(reg, iv, ks, ks_bytes);
        DSD_SECURE_ZERO(&reg, sizeof(reg));
        return;
    }
    /* The generator never takes the cipher key itself: TB5 binds it to this carrier, location area and colour code. */
    uint8_t eck[10];
    tetra_tea_derive_eck(cn, ts->la, (uint8_t)(ts->colour & 0x3Fu), key->key, eck);
    tetra_tea_keystream(ksg, eck, iv, ks, ks_bytes);
    DSD_SECURE_ZERO(eck, sizeof(eck));
}

int
tetra_air_decrypt_voice(const dsd_opts* opts, const dsd_state* state, uint8_t enc_mode, uint8_t* bits, int nbits,
                        int ks_bit_offset) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    uint8_t ks[(TETRA_AIR_KSS_MAX_BITS + 7) / 8];
    int ksg = -1;
    uint16_t cn = 0;
    if (!ts || !bits || nbits <= 0 || ks_bit_offset < 0 || ks_bit_offset + nbits > TETRA_AIR_KSS_MAX_BITS) {
        return 0;
    }
    if (ts->dmo || !tetra_enc_mode_encrypted(enc_mode) || !ts->hyperframe_valid || !ts->tdma_valid
        || !tetra_calls_carrier(opts, state, &cn)) {
        return 0;
    }
    const dsd_tetra_air_key* const key = tetra_air_resolve(state, ts, (int)cn, &ksg);
    /* Every legal initial value is non-zero: the frame number is at least 1. */
    const uint32_t iv = tetra_air_build_iv(ts->tn, ts->fn, ts->mn, ts->hyperframe, ts->uplink ? 1u : 0u);
    if (!key || iv == 0u) {
        return 0;
    }
    const int ks_bytes = (ks_bit_offset + nbits + 7) / 8;
    DSD_MEMSET(ks, 0, sizeof(ks));
    tetra_air_key_stream(ts, key, ksg, cn, iv, ks, ks_bytes);
    for (int i = 0; i < nbits; i++) {
        const int bit = ks_bit_offset + i;
        const uint8_t kb = (uint8_t)((ks[bit / 8] >> (7 - (bit % 8))) & 1u);
        bits[i] = (uint8_t)((bits[i] & 1u) ^ kb);
    }
    DSD_SECURE_ZERO(ks, sizeof(ks));
    return 1;
}
