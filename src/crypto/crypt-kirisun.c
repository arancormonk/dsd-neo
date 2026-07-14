// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "md2ii.h"
#include "pc4_internal.h"

static const uint8_t KIR_TAPS_R1[] = {0,  3,  5,  9,  10, 11, 12, 17, 18, 28, 33, 34, 35, 36,
                                      37, 39, 42, 43, 44, 46, 47, 49, 50, 57, 60, 61, 62, 63};
static const uint8_t KIR_TAPS_R2[] = {0,  3,  5,  8,  9,  10, 12, 13, 15, 17, 19, 20, 21, 22, 24, 27, 30,
                                      31, 33, 34, 35, 36, 37, 40, 41, 42, 51, 52, 55, 56, 59, 60, 62, 63};
static const uint8_t KIR_TAPS_R3[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 14, 15, 16, 17, 18,
                                      22, 23, 25, 26, 27, 28, 29, 31, 32, 34, 35, 36, 38, 41,
                                      42, 43, 44, 45, 47, 48, 49, 50, 51, 54, 55, 59, 61, 63};

static void
store_u64_be(uint64_t value, uint8_t out[8]) {
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)((value >> (56 - (i * 8))) & 0xFFU);
    }
}

static int
kir_load_slot_key(dsd_state* state, uint8_t slot, uint8_t out_key[32]) {
    if (!state || !out_key || slot > 1U) {
        return -1;
    }

    const int all_words_nonzero =
        state->A1[slot] != 0ULL && state->A2[slot] != 0ULL && state->A3[slot] != 0ULL && state->A4[slot] != 0ULL;
    const int complete_key = state->aes_key_segments[slot] == 4U && all_words_nonzero;
    state->aes_key_loaded[slot] = complete_key ? 1 : 0;
    if (!complete_key) {
        return -1;
    }

    store_u64_be((uint64_t)state->A1[slot], &out_key[0]);
    store_u64_be((uint64_t)state->A2[slot], &out_key[8]);
    store_u64_be((uint64_t)state->A3[slot], &out_key[16]);
    store_u64_be((uint64_t)state->A4[slot], &out_key[24]);
    return 0;
}

static void
kir_store_keystream(dsd_state* state, uint8_t slot, const uint8_t* ks, size_t len) {
    if (slot == 0U) {
        DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
        DSD_MEMCPY(state->ks_octetL, ks, len);
    } else {
        DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
        DSD_MEMCPY(state->ks_octetR, ks, len);
    }
}

static int
kir_threshold(uint64_t r1, uint64_t r2, uint64_t r3) {
    const int total = (int)(((r1 >> 31) & 1ULL) + ((r2 >> 31) & 1ULL) + ((r3 >> 31) & 1ULL));
    return (total > 1) ? 0 : 1;
}

static uint64_t
kir_clock_register(int ctl, uint64_t reg, const uint8_t* taps, size_t ntaps) {
    ctl ^= (int)((reg >> 31) & 1ULL);
    if (ctl) {
        uint64_t feedback = 0;
        for (size_t i = 0; i < ntaps; i++) {
            feedback ^= (reg >> taps[i]) & 1ULL;
        }
        reg = (reg << 1) & 0xFFFFFFFFFFFFFFFFULL;
        if (feedback & 1ULL) {
            reg ^= 1ULL;
        }
    }
    return reg;
}

static void
kir_keystream37(const uint8_t key[24], uint64_t frame, uint8_t output[126]) {
    uint64_t r1 = 0;
    uint64_t r2 = 0;
    uint64_t r3 = 0;
    for (int i = 0; i < 8; i++) {
        r1 = (r1 << 8) | key[i];
        r2 = (r2 << 8) | key[i + 8];
        r3 = (r3 << 8) | key[i + 16];
    }

    for (int i = 0; i < 64; i++) {
        const int ctl = kir_threshold(r1, r2, r3);
        r1 = kir_clock_register(ctl, r1, KIR_TAPS_R1, sizeof(KIR_TAPS_R1));
        r2 = kir_clock_register(ctl, r2, KIR_TAPS_R2, sizeof(KIR_TAPS_R2));
        r3 = kir_clock_register(ctl, r3, KIR_TAPS_R3, sizeof(KIR_TAPS_R3));
        if ((frame & 1ULL) != 0ULL) {
            r1 ^= 1ULL;
            r2 ^= 1ULL;
            r3 ^= 1ULL;
        }
        frame >>= 1;
    }

    for (int i = 0; i < 384; i++) {
        const int ctl = kir_threshold(r1, r2, r3);
        r1 = kir_clock_register(ctl, r1, KIR_TAPS_R1, sizeof(KIR_TAPS_R1));
        r2 = kir_clock_register(ctl, r2, KIR_TAPS_R2, sizeof(KIR_TAPS_R2));
        r3 = kir_clock_register(ctl, r3, KIR_TAPS_R3, sizeof(KIR_TAPS_R3));
    }

    size_t out_pos = 0;
    uint8_t byte = 0;
    int bit_count = 0;
    for (int i = 0; i < 1008; i++) {
        const int ctl = kir_threshold(r1, r2, r3);
        r1 = kir_clock_register(ctl, r1, KIR_TAPS_R1, sizeof(KIR_TAPS_R1));
        r2 = kir_clock_register(ctl, r2, KIR_TAPS_R2, sizeof(KIR_TAPS_R2));
        r3 = kir_clock_register(ctl, r3, KIR_TAPS_R3, sizeof(KIR_TAPS_R3));
        const uint8_t bit = (uint8_t)(((r1 >> 63) ^ (r2 >> 63) ^ (r3 >> 63)) & 1ULL);
        byte = (uint8_t)((byte << 1) | bit);
        bit_count++;
        if (bit_count == 8) {
            output[out_pos++] = byte;
            bit_count = 0;
            byte = 0;
        }
    }
}

void
kirisun_uni_keystream_creation(dsd_state* state) {
    if (!state) {
        return;
    }

    const uint8_t slot = (state->currentslot == 0) ? 0U : 1U;
    const uint32_t mi = (uint32_t)((slot == 0U) ? state->payload_mi : state->payload_miR);

    uint8_t user_key[32];
    DSD_MEMSET(user_key, 0, sizeof(user_key));
    if (kir_load_slot_key(state, slot, user_key) != 0) {
        return;
    }

    uint8_t mi_bytes[4] = {
        (uint8_t)((mi >> 24) & 0xFFU),
        (uint8_t)((mi >> 16) & 0xFFU),
        (uint8_t)((mi >> 8) & 0xFFU),
        (uint8_t)(mi & 0xFFU),
    };

    uint8_t real_key[32];
    DSD_MEMSET(real_key, 0, sizeof(real_key));
    {
        dsd_md2ii_ctx md2;
        (void)dsd_md2ii_init(&md2, 32U);
        dsd_md2ii_update(&md2, user_key, sizeof(user_key));
        dsd_md2ii_final(&md2, real_key, sizeof(real_key));
    }

    uint8_t hash8[8];
    DSD_MEMSET(hash8, 0, sizeof(hash8));
    {
        dsd_md2ii_ctx md2;
        (void)dsd_md2ii_init(&md2, 8U);
        dsd_md2ii_update(&md2, mi_bytes, sizeof(mi_bytes));
        dsd_md2ii_update(&md2, real_key, sizeof(real_key));
        dsd_md2ii_final(&md2, hash8, sizeof(hash8));
    }

    uint64_t internal_state = 0;
    for (int i = 0; i < 8; i++) {
        internal_state = (internal_state << 8) | hash8[i];
    }

    uint8_t key24[24];
    DSD_MEMSET(key24, 0, sizeof(key24));
    {
        dsd_md2ii_ctx md2;
        (void)dsd_md2ii_init(&md2, 24U);
        dsd_md2ii_update(&md2, mi_bytes, sizeof(mi_bytes));
        dsd_md2ii_update(&md2, real_key, sizeof(real_key));
        dsd_md2ii_final(&md2, key24, sizeof(key24));
    }

    uint8_t ks_bytes[126];
    DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));
    kir_keystream37(key24, internal_state, ks_bytes);
    kir_store_keystream(state, slot, ks_bytes, sizeof(ks_bytes));
}

void
kirisun_adv_keystream_creation(dsd_state* state) {
    if (!state) {
        return;
    }

    const uint8_t slot = (state->currentslot == 0) ? 0U : 1U;
    const uint32_t mi = (uint32_t)((slot == 0U) ? state->payload_mi : state->payload_miR);

    uint8_t user_key[32];
    DSD_MEMSET(user_key, 0, sizeof(user_key));
    if (kir_load_slot_key(state, slot, user_key) != 0) {
        return;
    }

    uint8_t real_key[32];
    DSD_MEMSET(real_key, 0, sizeof(real_key));
    {
        dsd_md2ii_ctx md2;
        (void)dsd_md2ii_init(&md2, 32U);
        dsd_md2ii_update(&md2, user_key, sizeof(user_key));
        dsd_md2ii_final(&md2, real_key, sizeof(real_key));
    }

    uint8_t hash32[32];
    DSD_MEMSET(hash32, 0, sizeof(hash32));
    {
        uint8_t mi_bytes[4] = {
            (uint8_t)((mi >> 24) & 0xFFU),
            (uint8_t)((mi >> 16) & 0xFFU),
            (uint8_t)((mi >> 8) & 0xFFU),
            (uint8_t)(mi & 0xFFU),
        };
        dsd_md2ii_ctx md2;
        (void)dsd_md2ii_init(&md2, 32U);
        dsd_md2ii_update(&md2, mi_bytes, sizeof(mi_bytes));
        dsd_md2ii_update(&md2, real_key, sizeof(real_key));
        dsd_md2ii_final(&md2, hash32, sizeof(hash32));
    }

    uint64_t internal_state = 0;
    for (int i = 0; i < 6; i++) {
        internal_state = (internal_state << 8) | hash32[i];
    }

    uint8_t ks_bytes[126];
    pc4_kirisun_generate_keystream(user_key, internal_state, ks_bytes);

    kir_store_keystream(state, slot, ks_bytes, sizeof(ks_bytes));
}
