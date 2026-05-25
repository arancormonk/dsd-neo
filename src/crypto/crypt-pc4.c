// SPDX-License-Identifier: ISC
#include <dsd-neo/crypto/pc4.h>
#include <stddef.h>
#include <stdint.h>

/* Global PC4 context instance */
PC4Context g_pc4_context;

/* ---------------------------------
   Internal utility functions
----------------------------------- */

/* Rotate right */
static uint32_t
ror(uint32_t x, int shift, int bits) {
    uint32_t m0 = (1u << (bits - shift)) - 1u;
    uint32_t m1 = (1u << shift) - 1u;
    return ((x >> shift) & m0) | ((x & m1) << (bits - shift));
}

/* Rotate left */
static uint32_t
rol(uint32_t x, int shift, int bits) {
    uint32_t m0 = (1u << (bits - shift)) - 1u;
    uint32_t m1 = (1u << shift) - 1u;
    return ((x & m0) << shift) | ((x >> (bits - shift)) & m1);
}

/* SplitMix64 random number generator */
static uint64_t
next_rng(PC4Context* pc4_ctx) {
    uint64_t z = (pc4_ctx->x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* ARC4 initialization */
static void
arc4_init(PC4Context* pc4_ctx, const unsigned char key[]) {
    for (pc4_ctx->i_arc4 = 0; pc4_ctx->i_arc4 < 256; pc4_ctx->i_arc4++) {
        pc4_ctx->array_arc4[pc4_ctx->i_arc4] = (unsigned char)pc4_ctx->i_arc4;
    }

    pc4_ctx->j_arc4 = 0;
    for (pc4_ctx->i_arc4 = 0; pc4_ctx->i_arc4 < 256; pc4_ctx->i_arc4++) {
        pc4_ctx->j_arc4 = (pc4_ctx->j_arc4 + pc4_ctx->array_arc4[pc4_ctx->i_arc4] + key[pc4_ctx->i_arc4 % 256]) % 256;
        int tmp = pc4_ctx->array_arc4[pc4_ctx->i_arc4];
        pc4_ctx->array_arc4[pc4_ctx->i_arc4] = pc4_ctx->array_arc4[pc4_ctx->j_arc4];
        pc4_ctx->array_arc4[pc4_ctx->j_arc4] = tmp;
    }
    pc4_ctx->i_arc4 = 0;
    pc4_ctx->j_arc4 = 0;
}

/* ARC4 output combined with SplitMix64 stream */
static unsigned char
arc4_output(PC4Context* pc4_ctx) {
    uint8_t rndbyte, decal;
    int tmp, t;

    pc4_ctx->i_arc4 = (pc4_ctx->i_arc4 + 1) % 256;
    pc4_ctx->j_arc4 = (pc4_ctx->j_arc4 + pc4_ctx->array_arc4[pc4_ctx->i_arc4]) % 256;
    tmp = pc4_ctx->array_arc4[pc4_ctx->i_arc4];
    pc4_ctx->array_arc4[pc4_ctx->i_arc4] = pc4_ctx->array_arc4[pc4_ctx->j_arc4];
    pc4_ctx->array_arc4[pc4_ctx->j_arc4] = tmp;
    t = (pc4_ctx->array_arc4[pc4_ctx->i_arc4] + pc4_ctx->array_arc4[pc4_ctx->j_arc4]) % 256;

    if (pc4_ctx->xyz == 0) {
        pc4_ctx->bb = next_rng(pc4_ctx);
    }
    decal = (uint8_t)(56 - (8 * pc4_ctx->xyz));
    rndbyte = (uint8_t)((pc4_ctx->bb >> decal) & 0xffu);
    pc4_ctx->xyz++;
    if (pc4_ctx->xyz == 8) {
        pc4_ctx->xyz = 0;
    }

    if (pc4_ctx->count == 0) {
        rndbyte = (uint8_t)(rndbyte ^ pc4_ctx->array_arc4[t]);
        pc4_ctx->count = 1;
    } else {
        rndbyte = (uint8_t)(rndbyte + pc4_ctx->array_arc4[t]);
        pc4_ctx->count = 0;
    }
    return rndbyte;
}

/* Initialize MD2-II state */
static void
md2_init(PC4Context* pc4_ctx) {
    pc4_ctx->x1 = 0;
    pc4_ctx->x2 = 0;
    for (pc4_ctx->i = 0; pc4_ctx->i < n1; pc4_ctx->i++) {
        pc4_ctx->h2[pc4_ctx->i] = 0;
    }
    for (pc4_ctx->i = 0; pc4_ctx->i < n1; pc4_ctx->i++) {
        pc4_ctx->h1[pc4_ctx->i] = 0;
    }
}

/* MD2-II hashing */
static void
md2_hashing(PC4Context* pc4_ctx, const unsigned char t1[], size_t b6) {
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
        206, 232, 103, 102, 195, 117, 250, 99,  0,   74,  160, 241, 2,   113};

    size_t t1_len = b6;
    size_t idx = 0;
    while (idx < t1_len) {
        for (; idx < t1_len && pc4_ctx->x2 < n1; pc4_ctx->x2++) {
            int b5 = t1[idx++];
            pc4_ctx->h1[pc4_ctx->x2 + n1] = (unsigned char)b5;
            pc4_ctx->h1[pc4_ctx->x2 + (n1 * 2)] = (unsigned char)(b5 ^ pc4_ctx->h1[pc4_ctx->x2]);
            pc4_ctx->x1 = pc4_ctx->h2[pc4_ctx->x2] ^= s4[b5 ^ pc4_ctx->x1];
        }
        if (pc4_ctx->x2 == n1) {
            int b2 = 0;
            pc4_ctx->x2 = 0;
            for (int b3 = 0; b3 < (n1 + 2); b3++) {
                for (int b1 = 0; b1 < (n1 * 3); b1++) {
                    b2 = pc4_ctx->h1[b1] ^= s4[b2];
                }
                b2 = (b2 + b3) % 256;
            }
        }
    }
}

/* Finalize MD2-II */
static void
md2_end(PC4Context* pc4_ctx, unsigned char h4[n1]) {
    unsigned char h3[n1];
    int i, n4 = n1 - pc4_ctx->x2;
    for (i = 0; i < n4; i++) {
        h3[i] = (unsigned char)n4;
    }
    md2_hashing(pc4_ctx, h3, (size_t)n4);
    md2_hashing(pc4_ctx, pc4_ctx->h2, sizeof(pc4_ctx->h2));
    for (i = 0; i < n1; i++) {
        h4[i] = pc4_ctx->h1[i];
    }
}

/* Generate a random index */
static int
mixy(PC4Context* pc4_ctx, int nn2) {
    return arc4_output(pc4_ctx) % nn2;
}

/* Fisher-Yates shuffle */
static void
mixer(PC4Context* pc4_ctx, uint8_t* mixu, int nn) {
    int ii;
    for (ii = nn - 1; ii > 0; ii--) {
        int jj = mixy(pc4_ctx, ii + 1);
        uint8_t tmmp = mixu[jj];
        mixu[jj] = mixu[ii];
        mixu[ii] = tmmp;
    }
}

static void
pc4_discard_arc4(PC4Context* pc4_ctx) {
    int count = arc4_output(pc4_ctx) + 256;
    for (int i = 0; i < count; i++) {
        (void)arc4_output(pc4_ctx);
    }
}

static void
pc4_fill_sequence(uint8_t* numbers, int count) {
    for (int i = 0; i < count; i++) {
        numbers[i] = (uint8_t)i;
    }
}

static void
pc4_shuffle_into(PC4Context* pc4_ctx, uint8_t* numbers, uint8_t* dst, int count) {
    pc4_fill_sequence(numbers, count);
    mixer(pc4_ctx, numbers, count);
    for (int i = 0; i < count; i++) {
        dst[i] = numbers[i];
    }
}

static void
pc4_init_hash_state(PC4Context* pc4_ctx, const unsigned char key1[], size_t size1, unsigned char h4[n1]) {
    md2_init(pc4_ctx);
    md2_hashing(pc4_ctx, key1, size1);
    md2_end(pc4_ctx, h4);

    for (int i = 0; i < 16; i++) {
        pc4_ctx->keys[i] = h4[i];
    }
    arc4_init(pc4_ctx, h4);

    pc4_ctx->x = 0;
    for (int i = 0; i < 8; i++) {
        pc4_ctx->x = (pc4_ctx->x << 8) + (uint64_t)(h4[256 + i] & 0xffu);
    }

    pc4_ctx->xyz = 0;
    pc4_ctx->count = 0;

    for (int i = 0; i < 20000; i++) {
        (void)arc4_output(pc4_ctx);
    }
}

static void
pc4_init_round_perms(PC4Context* pc4_ctx, uint8_t* numbers) {
    for (int w = 0; w < 16; w++) {
        pc4_discard_arc4(pc4_ctx);
        pc4_shuffle_into(pc4_ctx, numbers, pc4_ctx->perm[w], 256);
    }
}

static void
pc4_init_round_xor(PC4Context* pc4_ctx, uint8_t dst[nbround][3]) {
    for (int w = 0; w < 3; w++) {
        for (int i = 0; i < nbround; i++) {
            dst[i][w] = arc4_output(pc4_ctx);
        }
    }
}

static void
pc4_init_tab_inverse(PC4Context* pc4_ctx, uint8_t* numbers) {
    pc4_shuffle_into(pc4_ctx, numbers, pc4_ctx->tab, 256);
    for (int i = 0; i < 256; i++) {
        pc4_ctx->inv[pc4_ctx->tab[i]] = (unsigned char)i;
    }
}

static void
pc4_init_permutations(PC4Context* pc4_ctx, uint8_t* numbers) {
    pc4_discard_arc4(pc4_ctx);
    for (int w = 0; w < 3; w++) {
        pc4_discard_arc4(pc4_ctx);
        pc4_shuffle_into(pc4_ctx, numbers, pc4_ctx->permut[w], 3);
    }
}

/* Key schedule and S-box generation */
void
create_keys(PC4Context* pc4_ctx, const unsigned char key1[], size_t size1) {
    unsigned char h4[n1];
    uint8_t numbers[256];

    pc4_init_hash_state(pc4_ctx, key1, size1, h4);
    pc4_init_round_perms(pc4_ctx, numbers);

    pc4_discard_arc4(pc4_ctx);
    pc4_shuffle_into(pc4_ctx, numbers, pc4_ctx->new1, 256);

    pc4_discard_arc4(pc4_ctx);
    pc4_shuffle_into(pc4_ctx, numbers, pc4_ctx->array, 49);

    pc4_discard_arc4(pc4_ctx);
    for (int i = 0; i < nbround; i++) {
        pc4_ctx->decal[i] = (uint8_t)((arc4_output(pc4_ctx) % 23) + 1);
    }

    pc4_discard_arc4(pc4_ctx);
    pc4_init_round_xor(pc4_ctx, pc4_ctx->rngxor);

    pc4_discard_arc4(pc4_ctx);
    pc4_shuffle_into(pc4_ctx, numbers, pc4_ctx->array2, 49);

    pc4_discard_arc4(pc4_ctx);
    pc4_init_tab_inverse(pc4_ctx, numbers);

    pc4_init_permutations(pc4_ctx, numbers);

    pc4_discard_arc4(pc4_ctx);
    pc4_init_round_xor(pc4_ctx, pc4_ctx->rngxor2);
}

/* Compute round transformation */
static void
compute(PC4Context* pc4_ctx, const uint8_t* tab1, uint8_t round) {
    pc4_ctx->tot[0] = (uint8_t)((pc4_ctx->perm[round][tab1[pc4_ctx->permut[0][0]]]
                                 + pc4_ctx->perm[round][tab1[pc4_ctx->permut[0][1]]])
                                ^ pc4_ctx->perm[round][tab1[pc4_ctx->permut[0][2]]]);
    pc4_ctx->tot[0] = (uint8_t)(pc4_ctx->tot[0] + pc4_ctx->new1[pc4_ctx->tot[0]]);
    pc4_ctx->tot[1] = (uint8_t)((pc4_ctx->perm[round][tab1[pc4_ctx->permut[1][0]]]
                                 + pc4_ctx->perm[round][tab1[pc4_ctx->permut[1][1]]])
                                ^ pc4_ctx->perm[round][tab1[pc4_ctx->permut[1][2]]]);
    pc4_ctx->tot[1] = (uint8_t)(pc4_ctx->tot[1] + pc4_ctx->new1[pc4_ctx->tot[1]]);
    pc4_ctx->tot[2] = (uint8_t)((pc4_ctx->perm[round][tab1[pc4_ctx->permut[2][0]]]
                                 + pc4_ctx->perm[round][tab1[pc4_ctx->permut[2][1]]])
                                ^ pc4_ctx->perm[round][tab1[pc4_ctx->permut[2][2]]]);
    pc4_ctx->tot[2] = (uint8_t)(pc4_ctx->tot[2] + pc4_ctx->new1[pc4_ctx->tot[2]]);
}

/* Convert bits to bytes */
void
binhex(PC4Context* pc4_ctx, const short* z, int length) {
    const short* b = z;
    int i, j;
    for (i = 0; i < length; i = j) {
        uint8_t a = 0;
        for (j = i; j < i + 8; ++j) {
            a |= (uint8_t)(b[((short)(7 - (j % 8)) + j) - (j % 8)] << (j - i));
        }
        pc4_ctx->convert[pc4_ctx->ptconvert] = a;
        pc4_ctx->ptconvert++;
    }
}

/* Convert byte to bits */
void
hexbin(PC4Context* pc4_ctx, short* q, uint8_t w,
       uint8_t hex) { // warning: unused parameter ‘pc4_ctx’ [-Wunused-parameter]
    short* bits = (short*)q;
    for (uint8_t i = 0; i < 8; ++i) {
        bits[(short)(7 + w) - i] = (short)((hex >> i) & 1u);
    }

    UNUSEDPC4(pc4_ctx); //fix above warning
}

/* Encrypt one block */
void
pc4encrypt(PC4Context* pc4_ctx) {
    int i;
    pc4_ctx->totb = 0;

    for (i = 0; i < 3; i++) {
        pc4_ctx->l[0][i] = pc4_ctx->convert[i];
        pc4_ctx->r[0][i] = pc4_ctx->convert[i + 3];
    }

    for (i = 1; i <= pc4_ctx->rounds; i++) {
        pc4_ctx->totb ^= pc4_ctx->r[(i - 1) % 2][0];
        pc4_ctx->totb ^= pc4_ctx->r[(i - 1) % 2][1];
        pc4_ctx->totb ^= pc4_ctx->r[(i - 1) % 2][2];

        pc4_ctx->r[(i - 1) % 2][0] += (uint8_t)(~pc4_ctx->rngxor2[pc4_ctx->rounds - i][0]);
        pc4_ctx->r[(i - 1) % 2][1] ^= (uint8_t)(~pc4_ctx->rngxor2[pc4_ctx->rounds - i][1]);
        pc4_ctx->r[(i - 1) % 2][2] += (uint8_t)(~pc4_ctx->rngxor2[pc4_ctx->rounds - i][2]);

        pc4_ctx->result = 0;
        pc4_ctx->result += ((uint32_t)pc4_ctx->r[(i - 1) % 2][0] << 16);
        pc4_ctx->result += ((uint32_t)pc4_ctx->r[(i - 1) % 2][1] << 8);
        pc4_ctx->result += pc4_ctx->r[(i - 1) % 2][2];

        pc4_ctx->result = rol(pc4_ctx->result, pc4_ctx->decal[i - 1], 24);

        pc4_ctx->r[(i - 1) % 2][0] = (uint8_t)(pc4_ctx->result >> 16);
        pc4_ctx->r[(i - 1) % 2][1] = (uint8_t)((pc4_ctx->result >> 8) & 0xffu);
        pc4_ctx->r[(i - 1) % 2][2] = (uint8_t)(pc4_ctx->result & 0xffu);

        pc4_ctx->r[(i - 1) % 2][0] = pc4_ctx->tab[pc4_ctx->r[(i - 1) % 2][0]];
        pc4_ctx->r[(i - 1) % 2][0] ^= pc4_ctx->rngxor[i - 1][0];

        pc4_ctx->r[(i - 1) % 2][1] = pc4_ctx->inv[pc4_ctx->r[(i - 1) % 2][1]];
        pc4_ctx->r[(i - 1) % 2][1] -= pc4_ctx->rngxor[i - 1][1];

        pc4_ctx->r[(i - 1) % 2][2] = pc4_ctx->tab[pc4_ctx->r[(i - 1) % 2][2]];
        pc4_ctx->r[(i - 1) % 2][2] ^= pc4_ctx->rngxor[i - 1][2];

        compute(pc4_ctx, pc4_ctx->r[(i - 1) % 2], (uint8_t)((i - 1) % 16));

        pc4_ctx->l[i % 2][0] = pc4_ctx->r[(i - 1) % 2][0];
        pc4_ctx->r[i % 2][0] = pc4_ctx->l[(i - 1) % 2][0] - pc4_ctx->tot[0];

        pc4_ctx->l[i % 2][1] = pc4_ctx->r[(i - 1) % 2][1];
        pc4_ctx->r[i % 2][1] = pc4_ctx->l[(i - 1) % 2][1] ^ pc4_ctx->tot[1];

        pc4_ctx->l[i % 2][2] = pc4_ctx->r[(i - 1) % 2][2];
        pc4_ctx->r[i % 2][2] = pc4_ctx->l[(i - 1) % 2][2] - pc4_ctx->tot[2];
    }

    {
        uint8_t prev = (pc4_ctx->rounds > 0) ? (uint8_t)((pc4_ctx->rounds - 1) % 2) : 0;
        for (i = 0; i < 3; i++) {
            pc4_ctx->convert[i + 3] = pc4_ctx->l[prev][i];
            pc4_ctx->convert[i] = pc4_ctx->r[prev][i];
        }
    }

    pc4_ctx->totb %= 2;
}

/* Decrypt one block */
void
pc4decrypt(PC4Context* pc4_ctx) {
    int i;
    pc4_ctx->totb = 0;

    for (i = 0; i < 3; i++) {
        pc4_ctx->l[0][i] = pc4_ctx->convert[i];
        pc4_ctx->r[0][i] = pc4_ctx->convert[i + 3];
    }

    pc4_ctx->y = (uint8_t)((pc4_ctx->rounds - 1) % 16);
    if (pc4_ctx->y == 0) {
        pc4_ctx->y = 16;
    }

    for (i = 1; i <= pc4_ctx->rounds; i++) {
        pc4_ctx->y--;
        compute(pc4_ctx, pc4_ctx->r[(i - 1) % 2], pc4_ctx->y);
        if (pc4_ctx->y == 0) {
            pc4_ctx->y = 16;
        }

        pc4_ctx->result = 0;

        pc4_ctx->l[(i - 1) % 2][0] ^= pc4_ctx->rngxor[pc4_ctx->rounds - i][0];
        pc4_ctx->l[(i - 1) % 2][0] = pc4_ctx->inv[pc4_ctx->l[(i - 1) % 2][0]];

        pc4_ctx->l[(i - 1) % 2][1] += pc4_ctx->rngxor[pc4_ctx->rounds - i][1];
        pc4_ctx->l[(i - 1) % 2][1] = pc4_ctx->tab[pc4_ctx->l[(i - 1) % 2][1]];

        pc4_ctx->l[(i - 1) % 2][2] ^= pc4_ctx->rngxor[pc4_ctx->rounds - i][2];
        pc4_ctx->l[(i - 1) % 2][2] = pc4_ctx->inv[pc4_ctx->l[(i - 1) % 2][2]];

        pc4_ctx->result += ((uint32_t)pc4_ctx->l[(i - 1) % 2][0] << 16);
        pc4_ctx->result += ((uint32_t)pc4_ctx->l[(i - 1) % 2][1] << 8);
        pc4_ctx->result += pc4_ctx->l[(i - 1) % 2][2];

        pc4_ctx->result = ror(pc4_ctx->result, pc4_ctx->decal[pc4_ctx->rounds - i], 24);

        pc4_ctx->l[(i - 1) % 2][0] = (uint8_t)(pc4_ctx->result >> 16);
        pc4_ctx->l[(i - 1) % 2][1] = (uint8_t)((pc4_ctx->result >> 8) & 0xffu);
        pc4_ctx->l[(i - 1) % 2][2] = (uint8_t)(pc4_ctx->result & 0xffu);

        pc4_ctx->l[(i - 1) % 2][0] -= (uint8_t)(~pc4_ctx->rngxor2[i - 1][0]);
        pc4_ctx->l[(i - 1) % 2][1] ^= (uint8_t)(~pc4_ctx->rngxor2[i - 1][1]);
        pc4_ctx->l[(i - 1) % 2][2] -= (uint8_t)(~pc4_ctx->rngxor2[i - 1][2]);

        pc4_ctx->totb ^= pc4_ctx->l[(i - 1) % 2][0];
        pc4_ctx->totb ^= pc4_ctx->l[(i - 1) % 2][1];
        pc4_ctx->totb ^= pc4_ctx->l[(i - 1) % 2][2];

        pc4_ctx->l[i % 2][0] = pc4_ctx->r[(i - 1) % 2][0];
        pc4_ctx->r[i % 2][0] = pc4_ctx->l[(i - 1) % 2][0] + pc4_ctx->tot[0];

        pc4_ctx->l[i % 2][1] = pc4_ctx->r[(i - 1) % 2][1];
        pc4_ctx->r[i % 2][1] = pc4_ctx->l[(i - 1) % 2][1] ^ pc4_ctx->tot[1];

        pc4_ctx->l[i % 2][2] = pc4_ctx->r[(i - 1) % 2][2];
        pc4_ctx->r[i % 2][2] = pc4_ctx->l[(i - 1) % 2][2] + pc4_ctx->tot[2];
    }

    {
        uint8_t prev = (pc4_ctx->rounds > 0) ? (uint8_t)((pc4_ctx->rounds - 1) % 2) : 0;
        for (i = 0; i < 3; i++) {
            pc4_ctx->convert[i + 3] = pc4_ctx->l[prev][i];
            pc4_ctx->convert[i] = pc4_ctx->r[prev][i];
        }
    }

    pc4_ctx->totb %= 2;
}
