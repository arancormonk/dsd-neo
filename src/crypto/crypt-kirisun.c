// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/pc4.h>

#include <dsd-neo/core/state.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KIR_MD2_MAX_BLOCK 264U

typedef struct {
    size_t block_len;
    uint8_t x1;
    size_t x2;
    uint8_t h2[KIR_MD2_MAX_BLOCK];
    uint8_t h1[KIR_MD2_MAX_BLOCK * 3U];
} kir_md2ii_ctx;

static const uint8_t KIR_MD2_SBOX[256] = {
    13,  199, 11,  67,  237, 193, 164, 77,  115, 184, 141, 222, 73,  38,  147, 36,  150, 87,  21,  104, 12,  61,
    156, 101, 111, 145, 119, 22,  207, 35,  198, 37,  171, 167, 80,  30,  219, 28,  213, 121, 86,  29,  214, 242,
    6,   4,   89,  162, 110, 175, 19,  157, 3,   88,  234, 94,  144, 118, 159, 239, 100, 17,  182, 173, 238, 68,
    16,  79,  132, 54,  163, 52,  9,   58,  57,  55,  229, 192, 170, 226, 56,  231, 187, 158, 70,  224, 233, 245,
    26,  47,  32,  44,  247, 8,   251, 20,  197, 185, 109, 153, 204, 218, 93,  178, 212, 137, 84,  174, 24,  120,
    130, 149, 72,  180, 181, 208, 255, 189, 152, 18,  143, 176, 60,  249, 27,  227, 128, 139, 243, 253, 59,  123,
    172, 108, 211, 96,  138, 10,  215, 42,  225, 40,  81,  65,  90,  25,  98,  126, 154, 64,  124, 116, 122, 5,
    1,   168, 83,  190, 131, 191, 244, 240, 235, 177, 155, 228, 125, 66,  43,  201, 248, 220, 129, 188, 230, 62,
    75,  71,  78,  34,  31,  216, 254, 136, 91,  114, 106, 46,  217, 196, 92,  151, 209, 133, 51,  236, 33,  252,
    127, 179, 69,  7,   183, 105, 146, 97,  39,  15,  205, 112, 200, 166, 223, 45,  48,  246, 186, 41,  148, 140,
    107, 76,  85,  95,  194, 142, 50,  49,  134, 23,  135, 169, 221, 210, 203, 63,  165, 82,  161, 202, 53,  14,
    206, 232, 103, 102, 195, 117, 250, 99,  0,   74,  160, 241, 2,   113,
};

static const uint8_t KIR_TAPS_R1[] = {0,  3,  5,  9,  10, 11, 12, 17, 18, 28, 33, 34, 35, 36,
                                      37, 39, 42, 43, 44, 46, 47, 49, 50, 57, 60, 61, 62, 63};
static const uint8_t KIR_TAPS_R2[] = {0,  3,  5,  8,  9,  10, 12, 13, 15, 17, 19, 20, 21, 22, 24, 27, 30,
                                      31, 33, 34, 35, 36, 37, 40, 41, 42, 51, 52, 55, 56, 59, 60, 62, 63};
static const uint8_t KIR_TAPS_R3[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 14, 15, 16, 17, 18,
                                      22, 23, 25, 26, 27, 28, 29, 31, 32, 34, 35, 36, 38, 41,
                                      42, 43, 44, 45, 47, 48, 49, 50, 51, 54, 55, 59, 61, 63};

static inline uint64_t
rol48(uint64_t x, int n) {
    return ((x << n) | (x >> (48 - n))) & 0xFFFFFFFFFFFFULL;
}

static void
kir_md2ii_init(kir_md2ii_ctx* ctx, size_t block_len) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->block_len = block_len;
}

static void
kir_md2ii_update(kir_md2ii_ctx* ctx, const uint8_t* input, size_t len) {
    size_t in_pos = 0;
    while (len > 0U) {
        while (len > 0U && ctx->x2 < ctx->block_len) {
            const uint8_t b = input[in_pos++];
            ctx->h1[ctx->x2 + ctx->block_len] = b;
            ctx->h1[ctx->x2 + (ctx->block_len * 2U)] = (uint8_t)(b ^ ctx->h1[ctx->x2]);
            ctx->x1 = ctx->h2[ctx->x2] ^= KIR_MD2_SBOX[b ^ ctx->x1];
            ctx->x2++;
            len--;
        }

        if (ctx->x2 == ctx->block_len) {
            uint8_t t = 0;
            ctx->x2 = 0;
            for (size_t round = 0; round < (ctx->block_len + 2U); round++) {
                for (size_t i = 0; i < (ctx->block_len * 3U); i++) {
                    t = ctx->h1[i] ^= KIR_MD2_SBOX[t];
                }
                t = (uint8_t)((t + round) & 0xFFU);
            }
        }
    }
}

static void
kir_md2ii_final(kir_md2ii_ctx* ctx, uint8_t* out, size_t out_len) {
    uint8_t pad[KIR_MD2_MAX_BLOCK];
    memset(pad, 0, sizeof(pad));
    const size_t pad_len = ctx->block_len - ctx->x2;
    for (size_t i = 0; i < pad_len; i++) {
        pad[i] = (uint8_t)pad_len;
    }

    kir_md2ii_update(ctx, pad, pad_len);
    kir_md2ii_update(ctx, ctx->h2, ctx->block_len);

    if (out_len > ctx->block_len) {
        out_len = ctx->block_len;
    }
    memcpy(out, ctx->h1, out_len);
}

static int
kir_load_slot_key(dsd_state* state, uint8_t slot, uint8_t out_key[32]) {
    if (!state || !out_key || slot > 1U) {
        return -1;
    }
    if (state->aes_key_loaded[slot] != 1) {
        return -1;
    }

    u64_to_bytes_be((uint64_t)state->A1[slot], &out_key[0]);
    u64_to_bytes_be((uint64_t)state->A2[slot], &out_key[8]);
    u64_to_bytes_be((uint64_t)state->A3[slot], &out_key[16]);
    u64_to_bytes_be((uint64_t)state->A4[slot], &out_key[24]);
    return 0;
}

static void
kir_store_keystream(dsd_state* state, uint8_t slot, const uint8_t* ks, size_t len) {
    if (slot == 0U) {
        memset(state->ks_octetL, 0, sizeof(state->ks_octetL));
        memcpy(state->ks_octetL, ks, len);
    } else {
        memset(state->ks_octetR, 0, sizeof(state->ks_octetR));
        memcpy(state->ks_octetR, ks, len);
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
    memset(user_key, 0, sizeof(user_key));
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
    memset(real_key, 0, sizeof(real_key));
    {
        kir_md2ii_ctx md2;
        kir_md2ii_init(&md2, 32U);
        kir_md2ii_update(&md2, user_key, sizeof(user_key));
        kir_md2ii_final(&md2, real_key, sizeof(real_key));
    }

    uint8_t hash8[8];
    memset(hash8, 0, sizeof(hash8));
    {
        kir_md2ii_ctx md2;
        kir_md2ii_init(&md2, 8U);
        kir_md2ii_update(&md2, mi_bytes, sizeof(mi_bytes));
        kir_md2ii_update(&md2, real_key, sizeof(real_key));
        kir_md2ii_final(&md2, hash8, sizeof(hash8));
    }

    uint64_t internal_state = 0;
    for (int i = 0; i < 8; i++) {
        internal_state = (internal_state << 8) | hash8[i];
    }

    uint8_t key24[24];
    memset(key24, 0, sizeof(key24));
    {
        kir_md2ii_ctx md2;
        kir_md2ii_init(&md2, 24U);
        kir_md2ii_update(&md2, mi_bytes, sizeof(mi_bytes));
        kir_md2ii_update(&md2, real_key, sizeof(real_key));
        kir_md2ii_final(&md2, key24, sizeof(key24));
    }

    uint8_t ks_bytes[126];
    memset(ks_bytes, 0, sizeof(ks_bytes));
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
    memset(user_key, 0, sizeof(user_key));
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
    memset(real_key, 0, sizeof(real_key));
    {
        kir_md2ii_ctx md2;
        kir_md2ii_init(&md2, 32U);
        kir_md2ii_update(&md2, user_key, sizeof(user_key));
        kir_md2ii_final(&md2, real_key, sizeof(real_key));
    }

    uint8_t hash32[32];
    memset(hash32, 0, sizeof(hash32));
    {
        kir_md2ii_ctx md2;
        kir_md2ii_init(&md2, 32U);
        kir_md2ii_update(&md2, mi_bytes, sizeof(mi_bytes));
        kir_md2ii_update(&md2, real_key, sizeof(real_key));
        kir_md2ii_final(&md2, hash32, sizeof(hash32));
    }

    uint64_t internal_state = 0;
    for (int i = 0; i < 6; i++) {
        internal_state = (internal_state << 8) | hash32[i];
    }

    PC4Context local_ctx;
    memset(&local_ctx, 0, sizeof(local_ctx));
    create_keys(&local_ctx, user_key, sizeof(user_key));
    local_ctx.rounds = nbround;

    uint8_t ks_bytes[126];
    memset(ks_bytes, 0, sizeof(ks_bytes));

    int k = 0;
    for (int frame = 0; frame < 18; frame++) {
        local_ctx.convert[0] = (uint8_t)((internal_state >> 40) & 0xFFU);
        local_ctx.convert[1] = (uint8_t)((internal_state >> 32) & 0xFFU);
        local_ctx.convert[2] = (uint8_t)((internal_state >> 24) & 0xFFU);
        local_ctx.convert[3] = (uint8_t)((internal_state >> 16) & 0xFFU);
        local_ctx.convert[4] = (uint8_t)((internal_state >> 8) & 0xFFU);
        local_ctx.convert[5] = (uint8_t)(internal_state & 0xFFU);

        pc4encrypt(&local_ctx);

        internal_state = 0;
        for (int i = 0; i < 6; i++) {
            internal_state = (internal_state << 8) | local_ctx.convert[i];
        }
        internal_state = rol48(internal_state, 1);

        for (int i = 0; i < 6; i++) {
            ks_bytes[k++] = local_ctx.convert[i];
        }
        k++; // Every 7th byte is skipped in the on-air layout.
    }

    kir_store_keystream(state, slot, ks_bytes, sizeof(ks_bytes));
}
