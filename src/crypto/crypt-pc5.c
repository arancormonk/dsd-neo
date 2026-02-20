// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/pc5.h>

#include <dsd-neo/core/state.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PC5Context ctxpc5;

static uint32_t
pc5_ror(uint32_t x, int shift, int bits) {
    uint32_t m0 = (1u << (bits - shift)) - 1u;
    uint32_t m1 = (1u << shift) - 1u;
    return ((x >> shift) & m0) | ((x & m1) << (bits - shift));
}

static uint32_t
pc5_rol(uint32_t x, int shift, int bits) {
    uint32_t m0 = (1u << (bits - shift)) - 1u;
    uint32_t m1 = (1u << shift) - 1u;
    return ((x & m0) << shift) | ((x >> (bits - shift)) & m1);
}

static uint64_t
pc5_next_rng(PC5Context* ctx) {
    uint64_t z = (ctx->x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void
pc5_arc4_init(PC5Context* ctx, unsigned char key[]) {
    int tmp = 0;
    for (ctx->i_arc4 = 0; ctx->i_arc4 < 256; ctx->i_arc4++) {
        ctx->array_arc4[ctx->i_arc4] = (unsigned char)ctx->i_arc4;
    }

    ctx->j_arc4 = 0;
    for (ctx->i_arc4 = 0; ctx->i_arc4 < 256; ctx->i_arc4++) {
        ctx->j_arc4 = (ctx->j_arc4 + ctx->array_arc4[ctx->i_arc4] + key[ctx->i_arc4 % 256]) % 256;
        tmp = ctx->array_arc4[ctx->i_arc4];
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

static void
pc5_md2_init(PC5Context* ctx) {
    ctx->x1 = 0;
    ctx->x2 = 0;
    for (ctx->i = 0; ctx->i < PC5_MD2_N; ctx->i++) {
        ctx->h2[ctx->i] = 0;
        ctx->h1[ctx->i] = 0;
    }
}

static void
pc5_md2_hashing(PC5Context* ctx, unsigned char t1[], size_t b6) {
    static const unsigned char s4[256] = {
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

    int b1 = 0;
    int b2 = 0;
    int b3 = 0;
    int b4 = 0;
    int b5 = 0;

    if (ctx->x2 < 0 || ctx->x2 > PC5_MD2_N) {
        ctx->x2 = 0;
    }
    if (ctx->x1 < 0 || ctx->x1 > 255) {
        ctx->x1 = 0;
    }

    while (b6) {
        for (; b6 && ctx->x2 < PC5_MD2_N; b6--, ctx->x2++) {
            b5 = t1[b4++];
            ctx->h1[ctx->x2 + PC5_MD2_N] = (unsigned char)b5;
            ctx->h1[ctx->x2 + (PC5_MD2_N * 2)] = (unsigned char)(b5 ^ ctx->h1[ctx->x2]);
            ctx->x1 = ctx->h2[ctx->x2] ^= s4[b5 ^ ctx->x1];
        }

        if (ctx->x2 == PC5_MD2_N) {
            b2 = 0;
            ctx->x2 = 0;
            for (b3 = 0; b3 < (PC5_MD2_N + 2); b3++) {
                for (b1 = 0; b1 < (PC5_MD2_N * 3); b1++) {
                    b2 = ctx->h1[b1] ^= s4[b2];
                }
                b2 = (b2 + b3) % 256;
            }
        }
    }
}

static void
pc5_md2_end(PC5Context* ctx, unsigned char h4[PC5_MD2_N]) {
    unsigned char h3[PC5_MD2_N];
    size_t x2 = 0;
    if (ctx->x2 >= 0 && ctx->x2 <= PC5_MD2_N) {
        x2 = (size_t)ctx->x2;
    }

    size_t n4 = (size_t)PC5_MD2_N - x2;
    memset(h3, (unsigned char)n4, sizeof(h3));
    pc5_md2_hashing(ctx, h3, n4);
    pc5_md2_hashing(ctx, ctx->h2, sizeof(ctx->h2));
    for (int i = 0; i < PC5_MD2_N; i++) {
        h4[i] = ctx->h1[i];
    }
}

static int
pc5_mixy(PC5Context* ctx, int nn2) {
    return pc5_arc4_output(ctx) % nn2;
}

static void
pc5_mixer(PC5Context* ctx, uint8_t* mixu, int nn) {
    int ii = 0;
    int jj = 0;
    int tmp = 0;
    for (ii = nn - 1; ii > 0; ii--) {
        jj = pc5_mixy(ctx, ii + 1);
        tmp = mixu[jj];
        mixu[jj] = mixu[ii];
        mixu[ii] = (uint8_t)tmp;
    }
}

void
create_keys_pc5(PC5Context* ctx, unsigned char key1[], size_t size1) {
    int i = 0;
    int w = 0;
    int k = 0;
    unsigned char h4[PC5_MD2_N];
    memset(h4, 0, sizeof(h4));

    pc5_md2_init(ctx);
    pc5_md2_hashing(ctx, key1, size1);
    pc5_md2_end(ctx, h4);

    pc5_arc4_init(ctx, h4);

    ctx->x = 0;
    for (i = 0; i < 8; i++) {
        ctx->x = (ctx->x << 8) + (uint64_t)(h4[256 + i] & 0xffu);
    }
    ctx->xyz = 0;
    ctx->count = 0;

    for (i = 0; i < 23000; i++) {
        (void)pc5_arc4_output(ctx);
    }

    uint8_t numbers[256];
    memset(numbers, 0, sizeof(numbers));

    for (w = 0; w < 253; w++) {
        k = pc5_arc4_output(ctx) + 256;
        for (i = 0; i < k; i++) {
            (void)pc5_arc4_output(ctx);
        }
        for (i = 0; i < 16; i++) {
            numbers[i] = (uint8_t)i;
        }
        pc5_mixer(ctx, numbers, 16);
        for (i = 0; i < 16; i++) {
            ctx->perm[i][w] = numbers[i];
        }
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (i = 0; i < 16; i++) {
        numbers[i] = (uint8_t)i;
    }
    pc5_mixer(ctx, numbers, 16);
    for (i = 0; i < 16; i++) {
        ctx->new1[i] = numbers[i];
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (i = 0; i < PC5_NBROUND; i++) {
        ctx->decal[i] = (uint8_t)((pc5_arc4_output(ctx) % 11) + 1);
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (w = 0; w < 3; w++) {
        for (i = 0; i < PC5_NBROUND; i++) {
            ctx->rngxor[i][w] = (uint8_t)(pc5_arc4_output(ctx) % 16);
        }
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (i = 0; i < 16; i++) {
        numbers[i] = (uint8_t)i;
    }
    pc5_mixer(ctx, numbers, 16);
    for (i = 0; i < 16; i++) {
        ctx->tab[i] = numbers[i];
        ctx->inv[ctx->tab[i]] = (unsigned char)i;
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (w = 0; w < 3; w++) {
        k = pc5_arc4_output(ctx) + 256;
        for (i = 0; i < k; i++) {
            (void)pc5_arc4_output(ctx);
        }
        for (i = 0; i < 3; i++) {
            numbers[i] = (uint8_t)i;
        }
        pc5_mixer(ctx, numbers, 3);
        for (i = 0; i < 3; i++) {
            ctx->permut[w][i] = numbers[i];
        }
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (w = 0; w < 3; w++) {
        for (i = 0; i < PC5_NBROUND; i++) {
            ctx->rngxor2[i][w] = (uint8_t)(pc5_arc4_output(ctx) % 16);
        }
    }

    k = pc5_arc4_output(ctx) + 256;
    for (i = 0; i < k; i++) {
        (void)pc5_arc4_output(ctx);
    }
    for (w = 0; w < 25; w++) {
        ctx->numbers[w] = (uint8_t)(pc5_arc4_output(ctx) % 2);
    }
}

static void
pc5_compute(PC5Context* ctx, uint8_t* tab1, uint8_t round) {
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

void
binhexpc5(PC5Context* ctx, short* z, int length) {
    short* b = (short*)z;
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

void
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

void
pc5encrypt(PC5Context* ctx) {
    unsigned int rounds = ctx->rounds;
    if (rounds == 0 || rounds > PC5_NBROUND) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        ctx->l[0][i] = ctx->convert[i];
        ctx->r[0][i] = ctx->convert[i + 3];
    }

    for (unsigned int i = 1; i <= rounds; i++) {
        ctx->r[(i - 1) % 2][0] = (uint8_t)((ctx->r[(i - 1) % 2][0] + (uint8_t)(~ctx->rngxor2[rounds - i][0])) & 0x0FU);
        ctx->r[(i - 1) % 2][1] = (uint8_t)((ctx->r[(i - 1) % 2][1] ^ (uint8_t)(~ctx->rngxor2[rounds - i][1])) & 0x0FU);
        ctx->r[(i - 1) % 2][2] = (uint8_t)((ctx->r[(i - 1) % 2][2] + (uint8_t)(~ctx->rngxor2[rounds - i][2])) & 0x0FU);

        ctx->result =
            ((uint32_t)ctx->r[(i - 1) % 2][0] << 8) + ((uint32_t)ctx->r[(i - 1) % 2][1] << 4) + ctx->r[(i - 1) % 2][2];
        ctx->result = pc5_rol(ctx->result, ctx->decal[i - 1], 12);

        ctx->r[(i - 1) % 2][0] = (uint8_t)(ctx->result >> 8);
        ctx->r[(i - 1) % 2][1] = (uint8_t)((ctx->result >> 4) & 0xFU);
        ctx->r[(i - 1) % 2][2] = (uint8_t)(ctx->result & 0xFU);

        ctx->r[(i - 1) % 2][0] = (uint8_t)((ctx->tab[ctx->r[(i - 1) % 2][0]] ^ ctx->rngxor[i - 1][0]) & 0x0FU);
        ctx->r[(i - 1) % 2][1] = pc5_sub_mod16(ctx->inv[ctx->r[(i - 1) % 2][1]], ctx->rngxor[i - 1][1]);
        ctx->r[(i - 1) % 2][2] = (uint8_t)((ctx->tab[ctx->r[(i - 1) % 2][2]] ^ ctx->rngxor[i - 1][2]) & 0x0FU);

        pc5_compute(ctx, ctx->r[(i - 1) % 2], (uint8_t)((i - 1) % 253));

        ctx->l[i % 2][0] = ctx->r[(i - 1) % 2][0];
        ctx->r[i % 2][0] = pc5_sub_mod16(ctx->l[(i - 1) % 2][0], ctx->tot[0]);

        ctx->l[i % 2][1] = ctx->r[(i - 1) % 2][1];
        ctx->r[i % 2][1] = (uint8_t)((ctx->l[(i - 1) % 2][1] ^ ctx->tot[1]) & 0x0FU);

        ctx->l[i % 2][2] = ctx->r[(i - 1) % 2][2];
        ctx->r[i % 2][2] = pc5_sub_mod16(ctx->l[(i - 1) % 2][2], ctx->tot[2]);
    }

    for (int i = 0; i < 3; i++) {
        ctx->convert[i + 3] = ctx->l[(rounds - 1) % 2][i];
        ctx->convert[i] = ctx->r[(rounds - 1) % 2][i];
    }
}

void
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
        out[w++] = (char)toupper((unsigned char)*p);
    }
    out[w] = '\0';
    return w;
}

static int
pc5_parse_hex_bytes(const char* hex, size_t nhex, uint8_t* out, size_t out_len) {
    if (!hex || !out || (out_len * 2) != nhex) {
        return -1;
    }

    for (size_t i = 0; i < out_len; i++) {
        unsigned int v = 0;
        if (sscanf(&hex[i * 2], "%2X", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

int
baofeng_ap_pc5_keystream_creation(dsd_state* state, const char* input) {
    if (!state || !input) {
        return -1;
    }

    char hex[65];
    size_t nhex = pc5_collect_hex_digits(input, hex, sizeof(hex));
    if (nhex == 0) {
        fprintf(stderr, "DMR PC5 key parse failed: expected hex input\n");
        return -1;
    }

    if (nhex == 32) {
        uint8_t raw[16];
        uint8_t reversed[16];
        memset(raw, 0, sizeof(raw));
        memset(reversed, 0, sizeof(reversed));
        if (pc5_parse_hex_bytes(hex, nhex, raw, sizeof(raw)) != 0) {
            fprintf(stderr, "DMR PC5 key parse failed: invalid 128-bit key\n");
            return -1;
        }
        for (int i = 0; i < 16; i++) {
            reversed[i] = raw[15 - i];
        }
        create_keys_pc5(&ctxpc5, reversed, sizeof(reversed));
        ctxpc5.rounds = PC5_NBROUND;
        fprintf(stderr, "DMR Baofeng AP (PC5) 128-bit key with forced application\n");
        state->baofeng_ap = 1;
        return 0;
    }

    if (nhex == 64) {
        uint8_t raw[32];
        memset(raw, 0, sizeof(raw));
        if (pc5_parse_hex_bytes(hex, nhex, raw, sizeof(raw)) != 0) {
            fprintf(stderr, "DMR PC5 key parse failed: invalid 256-bit key\n");
            return -1;
        }
        create_keys_pc5(&ctxpc5, raw, sizeof(raw));
        ctxpc5.rounds = PC5_NBROUND;
        fprintf(stderr, "DMR Baofeng AP (PC5) 256-bit key with forced application\n");
        state->baofeng_ap = 1;
        return 0;
    }

    fprintf(stderr, "DMR PC5 key parse failed: expected 32 or 64 hex characters, got %zu\n", nhex);
    return -1;
}
