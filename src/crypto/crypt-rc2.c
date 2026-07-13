// SPDX-License-Identifier: ISC
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/rc2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"
#include "md2ii.h"
#include "vendor_ap_key_parse.h"

enum { RC2_MD2_BLOCK_SIZE = 264 };

static inline uint64_t
rol64(uint64_t x, int n) {
    return ((x << n) | (x >> (63 - n) >> 1)) & 0xffffffffffffffff;
}

static void
swapbit(uint64_t* internalstate, uint8_t bit) {
    unsigned char bitB = bit & 1;
    if (bitB) {
        *internalstate |= bitB;
    } else {
        *internalstate &= (~bitB ^ 1);
    }
}

// RC4 functions
static uint64_t
next(RC4State* state) {
    uint64_t z = (state->x += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

static void
rc4_init(RC4State* state, const unsigned char key[]) {
    for (state->i_rc4 = 0; state->i_rc4 < 256; state->i_rc4++) {
        state->array_rc4[state->i_rc4] = state->i_rc4;
    }

    state->j_rc4 = 0;
    for (state->i_rc4 = 0; state->i_rc4 < 256; state->i_rc4++) {
        state->j_rc4 = (state->j_rc4 + state->array_rc4[state->i_rc4] + key[state->i_rc4 % 256]) % 256;
        int tmp = state->array_rc4[state->i_rc4];
        state->array_rc4[state->i_rc4] = state->array_rc4[state->j_rc4];
        state->array_rc4[state->j_rc4] = tmp;
    }
    state->i_rc4 = 0;
    state->j_rc4 = 0;
}

static unsigned char
rc4_output(RC4State* state) {
    uint8_t rndbyte, decal;
    int tmp, t;

    state->i_rc4 = (state->i_rc4 + 1) % 256;
    state->j_rc4 = (state->j_rc4 + state->array_rc4[state->i_rc4]) % 256;
    tmp = state->array_rc4[state->i_rc4];
    state->array_rc4[state->i_rc4] = state->array_rc4[state->j_rc4];
    state->array_rc4[state->j_rc4] = tmp;
    t = (state->array_rc4[state->i_rc4] + state->array_rc4[state->j_rc4]) % 256;

    if (state->xyz == 0) {
        state->bb = next(state);
    }
    decal = 56 - (8 * state->xyz);
    rndbyte = (state->bb >> decal) & 0xff;
    state->xyz++;
    if (state->xyz == 8) {
        state->xyz = 0;
    }

    if (state->count == 0) {
        rndbyte = rndbyte ^ state->array_rc4[t];
        state->count = 1;
    } else {
        rndbyte = rndbyte + state->array_rc4[t];
        state->count = 0;
    }

    return rndbyte;
}

// RC2 functions
static void
rc2_keyschedule(RC2State* state) {
    unsigned i;
    i = 63;
    do {
        state->xkey[i] =
            ((unsigned char*)state->xkey)[((size_t)2) * i] + (((unsigned char*)state->xkey)[((size_t)2) * i + 1] << 8);
    } while (i--);
}

static void
rc2_encrypt(RC2State* state) {
    uint16_t x76, x54, x32, x10, i;

    x76 = (state->plain[7] << 8) + state->plain[6];
    x54 = (state->plain[5] << 8) + state->plain[4];
    x32 = (state->plain[3] << 8) + state->plain[2];
    x10 = (state->plain[1] << 8) + state->plain[0];

    for (i = 0; i < 16; i++) {
        x10 += (x32 & ~x76) + (x54 & x76) + state->xkey[4 * i + 0];
        x10 = (x10 << 1) + (x10 >> 15 & 1);

        x32 += (x54 & ~x10) + (x76 & x10) + state->xkey[4 * i + 1];
        x32 = (x32 << 2) + (x32 >> 14 & 3);

        x54 += (x76 & ~x32) + (x10 & x32) + state->xkey[4 * i + 2];
        x54 = (x54 << 3) + (x54 >> 13 & 7);

        x76 += (x10 & ~x54) + (x32 & x54) + state->xkey[4 * i + 3];
        x76 = (x76 << 5) + (x76 >> 11 & 31);

        if (i == 4 || i == 10) {
            x10 += state->xkey[x76 & 63];
            x32 += state->xkey[x10 & 63];
            x54 += state->xkey[x32 & 63];
            x76 += state->xkey[x54 & 63];
        }
    }

    state->cipher[0] = (unsigned char)x10;
    state->cipher[1] = (unsigned char)(x10 >> 8);
    state->cipher[2] = (unsigned char)x32;
    state->cipher[3] = (unsigned char)(x32 >> 8);
    state->cipher[4] = (unsigned char)x54;
    state->cipher[5] = (unsigned char)(x54 >> 8);
    state->cipher[6] = (unsigned char)x76;
    state->cipher[7] = (unsigned char)(x76 >> 8);
}

// Main cryptographic functions
void
create_keys_rc2(CryptoContext* ctx, const unsigned char key1[], size_t size1) {
    unsigned char h4[RC2_MD2_BLOCK_SIZE];

    (void)dsd_md2ii_hash(key1, size1, RC2_MD2_BLOCK_SIZE, h4, RC2_MD2_BLOCK_SIZE);

    // Copy first 16 bytes to keys
    for (int i = 0; i < 16; i++) {
        ctx->keys[i] = h4[i];
    }

    // Initialize RC4 with hashed key
    rc4_init(&ctx->rc4, h4);

    // Initialize RC4 state variables
    ctx->rc4.x = 0;
    for (int i = 0; i < 8; i++) {
        ctx->rc4.x = (ctx->rc4.x << 8) + (h4[256 + i] & 0xff);
    }

    ctx->rc4.xyz = 0;
    ctx->rc4.count = 0;

    // Warm-up RC4
    for (int i = 0; i < 22000; i++) {
        rc4_output(&ctx->rc4);
    }

    // Generate RC2 keys
    int k = rc4_output(&ctx->rc4) + 256;
    for (int i = 0; i < k; i++) {
        rc4_output(&ctx->rc4);
    }

    for (int i = 0; i < 64; i++) {
        ctx->rc2.xkey[i] = (rc4_output(&ctx->rc4) << 8) + rc4_output(&ctx->rc4);
    }

    // Generate internal zero value
    k = rc4_output(&ctx->rc4) + 256;
    for (int i = 0; i < k; i++) {
        rc4_output(&ctx->rc4);
    }

    ctx->internal_zero = 0;
    for (int i = 0; i < 8; i++) {
        ctx->internal_zero = (ctx->internal_zero << 8) + rc4_output(&ctx->rc4);
    }

    // Generate RC2 key schedule
    rc2_keyschedule(&ctx->rc2);
}

void
decrypt_rc2(CryptoContext* ctx, uint8_t bits[49]) {
    ctx->internal_state = ctx->internal_zero;

    for (int sso = 0; sso < 49; sso++) {
        // Prepare plaintext from internal state
        ctx->rc2.plain[0] = (ctx->internal_state >> 56) & 0xff;
        ctx->rc2.plain[1] = (ctx->internal_state >> 48) & 0xff;
        ctx->rc2.plain[2] = (ctx->internal_state >> 40) & 0xff;
        ctx->rc2.plain[3] = (ctx->internal_state >> 32) & 0xff;
        ctx->rc2.plain[4] = (ctx->internal_state >> 24) & 0xff;
        ctx->rc2.plain[5] = (ctx->internal_state >> 16) & 0xff;
        ctx->rc2.plain[6] = (ctx->internal_state >> 8) & 0xff;
        ctx->rc2.plain[7] = ctx->internal_state & 0xff;

        // Encrypt with RC2
        rc2_encrypt(&ctx->rc2);

        // Reconstruct internal state from ciphertext
        ctx->internal_state = 0;
        for (int i = 0; i < 8; i++) {
            ctx->internal_state = (ctx->internal_state << 8) + (ctx->rc2.cipher[i] & 0xff);
        }

        // XOR the bit and update internal state
        uint8_t tempy = bits[48 - sso];
        bits[48 - sso] = bits[48 - sso] ^ (ctx->internal_state & 1);
        ctx->internal_state = rol64(ctx->internal_state, 1);
        swapbit(&ctx->internal_state, tempy);
    }
}

int
retevis_rc2_apply_frame49(dsd_state* state, char ambe_d[49]) {
    if (state == NULL || ambe_d == NULL || state->retevis_ap != 1 || state->rc2_context == NULL) {
        return 0;
    }
    if (dmr_ambe49_should_skip_crypto(ambe_d) == 1) {
        return 0;
    }

    uint8_t frame1_cipher[49];
    for (int i = 0; i < 49; i++) {
        frame1_cipher[i] = (uint8_t)(((unsigned char)ambe_d[i]) & 1U);
    }
    decrypt_rc2((CryptoContext*)state->rc2_context, frame1_cipher);
    DSD_MEMSET(ambe_d, 0, 49 * sizeof(char));
    for (int i = 0; i < 49; i++) {
        ambe_d[i] = (char)(frame1_cipher[i] & 1U);
    }
    return 1;
}

/* Key creation for Retevis AP */
void
retevis_rc2_keystream_creation(dsd_state* state, const char* input, int show_keys) {
    if (state == NULL || input == NULL) {
        return;
    }

    dsd_vendor_ap_key parsed;
    const int parse_rc = dsd_vendor_ap_key_parse(input, &parsed);
    if (parse_rc != DSD_VENDOR_AP_KEY_OK) {
        DSD_FPRINTF(stderr, "DMR RETEVIS AP (RC2) key parse failed: expected 32 or 64 hex characters\n");
        free(state->rc2_context);
        state->rc2_context = NULL;
        state->retevis_ap = 0;
        return;
    }

    CryptoContext rc2_ctx;
    DSD_MEMSET(&rc2_ctx, 0, sizeof(rc2_ctx));
    if (parsed.nhex == 64U) {
        create_keys_rc2(&rc2_ctx, parsed.hex, parsed.nhex);
        uint8_t key_bytes[32];
        char key_text[65];
        DSD_MEMSET(key_bytes, 0, sizeof(key_bytes));
        (void)dsd_parse_hex_bytes_exact((const char*)parsed.hex, parsed.nhex, key_bytes, sizeof(key_bytes));
        DSD_FPRINTF(stderr, "DMR RETEVIS AP (RC2) 256-bit key loaded with forced application: %s\n",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, show_keys, key_bytes, sizeof(key_bytes)));
    } else {
        unsigned char key1[16];
        DSD_MEMSET(key1, 0, sizeof(key1));
        unsigned char key2[16];
        DSD_MEMSET(key2, 0, sizeof(key2));

        if (dsd_parse_hex_bytes_exact((const char*)parsed.hex, parsed.nhex, key1, sizeof(key1)) != 0) {
            DSD_FPRINTF(stderr, "DMR RETEVIS AP (RC2) key parse failed: invalid 128-bit key\n");
            free(state->rc2_context);
            state->rc2_context = NULL;
            state->retevis_ap = 0;
            return;
        }
        for (int i = 0; i < 16; i++) {
            key2[i] = key1[15 - i];
        }
        create_keys_rc2(&rc2_ctx, key2, 16);
        char key_text[33];
        DSD_FPRINTF(stderr, "DMR RETEVIS AP (RC2) 128-bit key loaded with forced application: %s\n",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, show_keys, key1, sizeof(key1)));
    }

    // Store context in DSD state
    free(state->rc2_context);
    state->rc2_context = malloc(sizeof(CryptoContext));
    if (state->rc2_context == NULL) {
        DSD_FPRINTF(stderr, "DMR RETEVIS AP (RC2) key allocation failed\n");
        state->retevis_ap = 0;
        return;
    }
    DSD_MEMCPY(state->rc2_context, &rc2_ctx, sizeof(CryptoContext));

    state->retevis_ap = 1;
}
