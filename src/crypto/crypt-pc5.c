// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/secret_redaction.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "md2ii.h"

#define PC5_NBROUND 254
#define PC5_MD2_N   264

typedef struct {
    uint8_t ptconvert;
    uint8_t convert[7];
    uint8_t perm[16][256];
    uint8_t new1[256];
    uint8_t decal[PC5_NBROUND];
    uint8_t rngxor[PC5_NBROUND][3];
    uint8_t rngxor2[PC5_NBROUND][3];
    uint8_t rounds;
    uint8_t tab[256];
    uint8_t inv[256];
    uint8_t permut[3][3];
    uint8_t tot[3];
    uint8_t l[2][3];
    uint8_t r[2][3];
    uint8_t y;
    uint32_t result;
    uint8_t xyz;
    uint8_t count;
    uint64_t bb;
    uint64_t x;
    unsigned char array_arc4[256];
    int i_arc4;
    int j_arc4;
    uint8_t numbers[25];
} PC5Context;

static PC5Context g_pc5_context;

static uint32_t
pc5_ror(uint32_t x, int shift, int bits) {
    uint32_t m0 = (1u << (bits - shift)) - 1u;
    uint32_t m1 = (1u << shift) - 1u;
    return ((x >> shift) & m0) | ((x & m1) << (bits - shift));
}

static uint64_t
pc5_next_rng(PC5Context* ctx) {
    uint64_t z = (ctx->x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void
pc5_arc4_init(PC5Context* ctx, const unsigned char key[]) {
    for (ctx->i_arc4 = 0; ctx->i_arc4 < 256; ctx->i_arc4++) {
        ctx->array_arc4[ctx->i_arc4] = (unsigned char)ctx->i_arc4;
    }

    ctx->j_arc4 = 0;
    for (ctx->i_arc4 = 0; ctx->i_arc4 < 256; ctx->i_arc4++) {
        ctx->j_arc4 = (ctx->j_arc4 + ctx->array_arc4[ctx->i_arc4] + key[ctx->i_arc4 % 256]) % 256;
        int tmp = ctx->array_arc4[ctx->i_arc4];
        ctx->array_arc4[ctx->i_arc4] = ctx->array_arc4[ctx->j_arc4];
        ctx->array_arc4[ctx->j_arc4] = (unsigned char)tmp;
    }
    ctx->i_arc4 = 0;
    ctx->j_arc4 = 0;
}

static unsigned char
pc5_arc4_output(PC5Context* ctx) {
    uint8_t rndbyte = 0;
    uint8_t decal = 0;
    int tmp = 0;
    int t = 0;

    ctx->i_arc4 = (ctx->i_arc4 + 1) % 256;
    ctx->j_arc4 = (ctx->j_arc4 + ctx->array_arc4[ctx->i_arc4]) % 256;
    tmp = ctx->array_arc4[ctx->i_arc4];
    ctx->array_arc4[ctx->i_arc4] = ctx->array_arc4[ctx->j_arc4];
    ctx->array_arc4[ctx->j_arc4] = (unsigned char)tmp;
    t = (ctx->array_arc4[ctx->i_arc4] + ctx->array_arc4[ctx->j_arc4]) % 256;

    if (ctx->xyz == 0) {
        ctx->bb = pc5_next_rng(ctx);
    }
    decal = (uint8_t)(56 - (8 * ctx->xyz));
    rndbyte = (uint8_t)((ctx->bb >> decal) & 0xffu);
    ctx->xyz++;
    if (ctx->xyz == 8) {
        ctx->xyz = 0;
    }

    if (ctx->count == 0) {
        rndbyte = (uint8_t)(rndbyte ^ ctx->array_arc4[t]);
        ctx->count = 1;
    } else {
        rndbyte = (uint8_t)(rndbyte + ctx->array_arc4[t]);
        ctx->count = 0;
    }
    return rndbyte;
}

static int
pc5_mixy(PC5Context* ctx, int nn2) {
    return pc5_arc4_output(ctx) % nn2;
}

static void
pc5_mixer(PC5Context* ctx, uint8_t* mixu, int nn) {
    int ii = 0;
    for (ii = nn - 1; ii > 0; ii--) {
        int jj = pc5_mixy(ctx, ii + 1);
        uint8_t tmp = mixu[jj];
        mixu[jj] = mixu[ii];
        mixu[ii] = tmp;
    }
}

static void
pc5_discard_arc4(PC5Context* ctx) {
    int count = pc5_arc4_output(ctx) + 256;
    for (int i = 0; i < count; i++) {
        (void)pc5_arc4_output(ctx);
    }
}

static void
pc5_fill_sequence(uint8_t* numbers, int count) {
    for (int i = 0; i < count; i++) {
        numbers[i] = (uint8_t)i;
    }
}

static void
pc5_shuffle_into(PC5Context* ctx, uint8_t* numbers, uint8_t* dst, int count) {
    pc5_fill_sequence(numbers, count);
    pc5_mixer(ctx, numbers, count);
    for (int i = 0; i < count; i++) {
        dst[i] = numbers[i];
    }
}

static void
pc5_init_hash_state(PC5Context* ctx, const unsigned char key1[], size_t size1, unsigned char h4[PC5_MD2_N]) {
    (void)dsd_md2ii_hash(key1, size1, PC5_MD2_N, h4, PC5_MD2_N);

    pc5_arc4_init(ctx, h4);

    ctx->x = 0;
    for (int i = 0; i < 8; i++) {
        ctx->x = (ctx->x << 8) + (uint64_t)(h4[256 + i] & 0xffu);
    }
    ctx->xyz = 0;
    ctx->count = 0;

    for (int i = 0; i < 23000; i++) {
        (void)pc5_arc4_output(ctx);
    }
}

static void
pc5_init_perm_columns(PC5Context* ctx, uint8_t* numbers) {
    for (int w = 0; w < 253; w++) {
        pc5_discard_arc4(ctx);
        pc5_fill_sequence(numbers, 16);
        pc5_mixer(ctx, numbers, 16);
        for (int i = 0; i < 16; i++) {
            ctx->perm[i][w] = numbers[i];
        }
    }
}

static void
pc5_init_round_xor(PC5Context* ctx, uint8_t dst[PC5_NBROUND][3]) {
    for (int w = 0; w < 3; w++) {
        for (int i = 0; i < PC5_NBROUND; i++) {
            dst[i][w] = (uint8_t)(pc5_arc4_output(ctx) % 16);
        }
    }
}

static void
pc5_init_tab_inverse(PC5Context* ctx, uint8_t* numbers) {
    pc5_shuffle_into(ctx, numbers, ctx->tab, 16);
    for (int i = 0; i < 16; i++) {
        ctx->inv[ctx->tab[i]] = (unsigned char)i;
    }
}

static void
pc5_init_permutations(PC5Context* ctx, uint8_t* numbers) {
    pc5_discard_arc4(ctx);
    for (int w = 0; w < 3; w++) {
        pc5_discard_arc4(ctx);
        pc5_shuffle_into(ctx, numbers, ctx->permut[w], 3);
    }
}

static void
pc5_init_tail_numbers(PC5Context* ctx) {
    /* The OTA schedule consumes one ARC4 byte before emitting the 25 tail mask bits. */
    (void)pc5_arc4_output(ctx);
    for (int w = 0; w < 25; w++) {
        ctx->numbers[w] = (uint8_t)(pc5_arc4_output(ctx) % 2);
    }
}

static void
create_keys_pc5(PC5Context* ctx, const unsigned char key1[], size_t size1) {
    unsigned char h4[PC5_MD2_N];
    DSD_MEMSET(h4, 0, sizeof(h4));
    uint8_t numbers[256];

    DSD_MEMSET(numbers, 0, sizeof(numbers));

    pc5_init_hash_state(ctx, key1, size1, h4);
    pc5_init_perm_columns(ctx, numbers);

    pc5_discard_arc4(ctx);
    pc5_shuffle_into(ctx, numbers, ctx->new1, 16);

    pc5_discard_arc4(ctx);
    for (int i = 0; i < PC5_NBROUND; i++) {
        ctx->decal[i] = (uint8_t)((pc5_arc4_output(ctx) % 11) + 1);
    }

    pc5_discard_arc4(ctx);
    pc5_init_round_xor(ctx, ctx->rngxor);

    pc5_discard_arc4(ctx);
    pc5_init_tab_inverse(ctx, numbers);

    pc5_init_permutations(ctx, numbers);

    pc5_discard_arc4(ctx);
    pc5_init_round_xor(ctx, ctx->rngxor2);

    pc5_init_tail_numbers(ctx);
}

static void
pc5_compute(PC5Context* ctx, const uint8_t* tab1, uint8_t round) {
    ctx->tot[0] = (uint8_t)((ctx->perm[tab1[ctx->permut[0][0]]][round] + ctx->perm[tab1[ctx->permut[0][1]]][round])
                            ^ ctx->perm[tab1[ctx->permut[0][2]]][round]);
    ctx->tot[0] = (uint8_t)((ctx->tot[0] + ctx->new1[ctx->tot[0]]) % 16);
    ctx->tot[1] = (uint8_t)((ctx->perm[tab1[ctx->permut[1][0]]][round] + ctx->perm[tab1[ctx->permut[1][1]]][round])
                            ^ ctx->perm[tab1[ctx->permut[1][2]]][round]);
    ctx->tot[1] = (uint8_t)((ctx->tot[1] + ctx->new1[ctx->tot[1]]) % 16);
    ctx->tot[2] = (uint8_t)((ctx->perm[tab1[ctx->permut[2][0]]][round] + ctx->perm[tab1[ctx->permut[2][1]]][round])
                            ^ ctx->perm[tab1[ctx->permut[2][2]]][round]);
    ctx->tot[2] = (uint8_t)((ctx->tot[2] + ctx->new1[ctx->tot[2]]) % 16);
}

static void
binhexpc5(PC5Context* ctx, const short* z, int length) {
    const short* b = z;
    uint8_t i = 0;
    uint8_t j = 0;
    for (i = 0; i < (uint8_t)length; i = j) {
        uint8_t a = 0;
        for (j = i; j < (uint8_t)(i + 8); ++j) {
            a |= (uint8_t)(b[((short)(7 - (j % 8)) + j) - (j % 8)] << (j - i));
        }
        ctx->convert[ctx->ptconvert++] = a;
    }
}

static void
hexbinpc5(PC5Context* ctx, short* q, uint8_t w, uint8_t hex) {
    (void)ctx;
    short* bits = (short*)q;
    for (uint8_t i = 0; i < 8; ++i) {
        bits[(short)(7 + w) - i] = (short)((hex >> i) & 1u);
    }
}

static inline uint8_t
pc5_sub_mod16(uint8_t a, uint8_t b) {
    return (uint8_t)((a + 16U - (b & 0x0FU)) & 0x0FU);
}

static void
pc5decrypt(PC5Context* ctx) {
    unsigned int rounds = ctx->rounds;
    if (rounds == 0 || rounds > PC5_NBROUND) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        ctx->l[0][i] = ctx->convert[i];
        ctx->r[0][i] = ctx->convert[i + 3];
    }

    ctx->y = (uint8_t)((rounds - 1) % 253);
    if (ctx->y == 0) {
        ctx->y = 253;
    }

    for (unsigned int i = 1; i <= rounds; i++) {
        ctx->y--;
        pc5_compute(ctx, ctx->r[(i - 1) % 2], ctx->y);
        if (ctx->y == 0) {
            ctx->y = 253;
        }

        ctx->l[(i - 1) % 2][0] = (uint8_t)((ctx->l[(i - 1) % 2][0] ^ ctx->rngxor[rounds - i][0]) & 0x0FU);
        ctx->l[(i - 1) % 2][0] = ctx->inv[ctx->l[(i - 1) % 2][0]];

        ctx->l[(i - 1) % 2][1] = (uint8_t)((ctx->l[(i - 1) % 2][1] + ctx->rngxor[rounds - i][1]) & 0x0FU);
        ctx->l[(i - 1) % 2][1] = ctx->tab[ctx->l[(i - 1) % 2][1]];

        ctx->l[(i - 1) % 2][2] = (uint8_t)((ctx->l[(i - 1) % 2][2] ^ ctx->rngxor[rounds - i][2]) & 0x0FU);
        ctx->l[(i - 1) % 2][2] = ctx->inv[ctx->l[(i - 1) % 2][2]];

        ctx->result =
            ((uint32_t)ctx->l[(i - 1) % 2][0] << 8) + ((uint32_t)ctx->l[(i - 1) % 2][1] << 4) + ctx->l[(i - 1) % 2][2];
        ctx->result = pc5_ror(ctx->result, ctx->decal[rounds - i], 12);

        ctx->l[(i - 1) % 2][0] = (uint8_t)(ctx->result >> 8);
        ctx->l[(i - 1) % 2][1] = (uint8_t)((ctx->result >> 4) & 0xFU);
        ctx->l[(i - 1) % 2][2] = (uint8_t)(ctx->result & 0xFU);

        ctx->l[(i - 1) % 2][0] = pc5_sub_mod16(ctx->l[(i - 1) % 2][0], (uint8_t)(~ctx->rngxor2[i - 1][0]));
        ctx->l[(i - 1) % 2][1] = (uint8_t)((ctx->l[(i - 1) % 2][1] ^ (uint8_t)(~ctx->rngxor2[i - 1][1])) & 0x0FU);
        ctx->l[(i - 1) % 2][2] = pc5_sub_mod16(ctx->l[(i - 1) % 2][2], (uint8_t)(~ctx->rngxor2[i - 1][2]));

        ctx->l[i % 2][0] = ctx->r[(i - 1) % 2][0];
        ctx->r[i % 2][0] = (uint8_t)((ctx->l[(i - 1) % 2][0] + ctx->tot[0]) & 0x0FU);

        ctx->l[i % 2][1] = ctx->r[(i - 1) % 2][1];
        ctx->r[i % 2][1] = (uint8_t)((ctx->l[(i - 1) % 2][1] ^ ctx->tot[1]) & 0x0FU);

        ctx->l[i % 2][2] = ctx->r[(i - 1) % 2][2];
        ctx->r[i % 2][2] = (uint8_t)((ctx->l[(i - 1) % 2][2] + ctx->tot[2]) & 0x0FU);
    }

    for (int i = 0; i < 3; i++) {
        ctx->convert[i + 3] = ctx->l[(rounds - 1) % 2][i];
        ctx->convert[i] = ctx->r[(rounds - 1) % 2][i];
    }
}

static void
pc5_decrypt_frame49(short frame_bits[49]) {
    PC5Context* ctx = &g_pc5_context;
    for (int i = 24; i < 49; i++) {
        frame_bits[i] = (short)(frame_bits[i] ^ ctx->numbers[i - 24]);
    }

    ctx->ptconvert = 0;
    binhexpc5(ctx, frame_bits, 24);

    uint8_t convert[6];
    for (int i = 0; i < 3; i++) {
        convert[i] = ctx->convert[i];
    }

    ctx->convert[0] = convert[0] >> 4;
    ctx->convert[1] = convert[0] & 0xF;
    ctx->convert[2] = convert[1] >> 4;
    ctx->convert[3] = convert[1] & 0xF;
    ctx->convert[4] = convert[2] >> 4;
    ctx->convert[5] = convert[2] & 0xF;

    pc5decrypt(ctx);

    for (int i = 0; i < 6; i++) {
        convert[i] = ctx->convert[i];
    }

    ctx->convert[0] = (uint8_t)((convert[0] << 4) | convert[1]);
    ctx->convert[1] = (uint8_t)((convert[2] << 4) | convert[3]);
    ctx->convert[2] = (uint8_t)((convert[4] << 4) | convert[5]);

    for (int q = 0; q < 3; q++) {
        uint8_t w = (uint8_t)(q * 8);
        hexbinpc5(ctx, frame_bits, w, ctx->convert[q]);
    }
}

static size_t
pc5_collect_hex_digits(const char* input, char* out, size_t out_cap) {
    if (!input || !out || out_cap < 2) {
        return 0;
    }
    size_t w = 0;
    for (const char* p = input; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return 0;
        }
        if (w + 1 >= out_cap) {
            return 0;
        }
        out[w++] = *p;
    }
    out[w] = '\0';
    return w;
}

int
baofeng_pc5_apply_frame49(const dsd_state* state, char ambe_d[49]) {
    if (state == NULL || ambe_d == NULL || state->baofeng_ap != 1) {
        return 0;
    }
    if (dmr_ambe49_should_skip_crypto(ambe_d) == 1) {
        return 0;
    }

    short frame1_cipher[49];
    for (int i = 0; i < 49; i++) {
        frame1_cipher[i] = (short)(((unsigned char)ambe_d[i]) & 1U);
    }
    pc5_decrypt_frame49(frame1_cipher);
    DSD_MEMSET(ambe_d, 0, 49 * sizeof(char));
    for (int i = 0; i < 49; i++) {
        ambe_d[i] = (char)(frame1_cipher[i] & 1);
    }
    return 1;
}

int
baofeng_ap_pc5_keystream_creation(dsd_state* state, const char* input, int show_keys) {
    if (!state || !input) {
        return -1;
    }

    char hex[65];
    size_t nhex = pc5_collect_hex_digits(input, hex, sizeof(hex));
    if (nhex == 0) {
        DSD_FPRINTF(stderr, "DMR PC5 key parse failed: expected hex input\n");
        return -1;
    }

    if (nhex == 32) {
        uint8_t raw[16];
        uint8_t reversed[16];
        DSD_MEMSET(raw, 0, sizeof(raw));
        DSD_MEMSET(reversed, 0, sizeof(reversed));
        if (dsd_parse_hex_bytes_exact(hex, nhex, raw, sizeof(raw)) != 0) {
            DSD_FPRINTF(stderr, "DMR PC5 key parse failed: invalid 128-bit key\n");
            return -1;
        }
        for (int i = 0; i < 16; i++) {
            reversed[i] = raw[15 - i];
        }
        create_keys_pc5(&g_pc5_context, reversed, sizeof(reversed));
        g_pc5_context.rounds = PC5_NBROUND;
        char key_text[33];
        DSD_FPRINTF(stderr, "DMR Baofeng AP (PC5) 128-bit key with forced application: %s\n",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, show_keys, raw, sizeof(raw)));
        state->baofeng_ap = 1;
        return 0;
    }

    if (nhex == 64) {
        /* PC5-256 uses the 64 ASCII hex characters as OTA key material, unlike
         * PC5-128 which is decoded to bytes and reversed. */
        create_keys_pc5(&g_pc5_context, (const unsigned char*)hex, nhex);
        g_pc5_context.rounds = PC5_NBROUND;
        char key_text[65];
        DSD_FPRINTF(stderr, "DMR Baofeng AP (PC5) 256-bit key with forced application: %s\n",
                    dsd_secret_format_string(key_text, sizeof key_text, show_keys, hex));
        state->baofeng_ap = 1;
        return 0;
    }

    DSD_FPRINTF(stderr, "DMR PC5 key parse failed: expected 32 or 64 hex characters, got %zu\n", nhex);
    return -1;
}
