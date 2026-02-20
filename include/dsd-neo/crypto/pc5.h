// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief PC5 context and helpers for Baofeng-style DMR AP.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define PC5_ATTR_UNUSED __attribute__((unused))
#else
#define PC5_ATTR_UNUSED
#endif

#define PC5_NBROUND 254
#define PC5_MD2_N   264

typedef struct {
    short bits[49];
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
    int x1;
    int x2;
    int i;
    unsigned char h2[PC5_MD2_N];
    unsigned char h1[PC5_MD2_N * 3];
    uint8_t numbers[25];
} PC5Context;

extern PC5Context ctxpc5;

void create_keys_pc5(PC5Context* ctx, unsigned char key1[], size_t size1);
void pc5encrypt(PC5Context* ctx);
void pc5decrypt(PC5Context* ctx);
void binhexpc5(PC5Context* ctx, short* z, int length);
void hexbinpc5(PC5Context* ctx, short* q, uint8_t w, uint8_t hex);

static PC5_ATTR_UNUSED void
decrypt_frame_49_pc5(short frame_bits_in[49]) {
    for (int i = 24; i < 49; i++) {
        frame_bits_in[i] = (short)(frame_bits_in[i] ^ ctxpc5.numbers[i - 24]);
    }

    ctxpc5.ptconvert = 0;
    binhexpc5(&ctxpc5, frame_bits_in, 24);

    uint8_t convert[6];
    for (int i = 0; i < 3; i++) {
        convert[i] = ctxpc5.convert[i];
    }

    ctxpc5.convert[0] = convert[0] >> 4;
    ctxpc5.convert[1] = convert[0] & 0xF;
    ctxpc5.convert[2] = convert[1] >> 4;
    ctxpc5.convert[3] = convert[1] & 0xF;
    ctxpc5.convert[4] = convert[2] >> 4;
    ctxpc5.convert[5] = convert[2] & 0xF;

    pc5decrypt(&ctxpc5);

    for (int i = 0; i < 6; i++) {
        convert[i] = ctxpc5.convert[i];
    }

    ctxpc5.convert[0] = (uint8_t)((convert[0] << 4) | convert[1]);
    ctxpc5.convert[1] = (uint8_t)((convert[2] << 4) | convert[3]);
    ctxpc5.convert[2] = (uint8_t)((convert[4] << 4) | convert[5]);

    for (int q = 0; q < 3; q++) {
        uint8_t w = (uint8_t)(q * 8);
        hexbinpc5(&ctxpc5, frame_bits_in, w, ctxpc5.convert[q]);
    }

    for (int i = 0; i < 49; i++) {
        ctxpc5.bits[i] = frame_bits_in[i];
    }
}
