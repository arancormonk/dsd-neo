// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Retevis RC2/RC4/MD2 composite cipher helpers.
 */
#ifndef RETEVIS_AP
#define RETEVIS_AP

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t array_rc4[256];
    int i_rc4, j_rc4;
    uint64_t x;
    uint8_t xyz, count;
    uint64_t bb;
} RC4State;

typedef struct {
    uint16_t xkey[64];
    uint8_t plain[8];
    uint8_t cipher[8];
} RC2State;

typedef struct {
    RC4State rc4;
    RC2State rc2;
    uint64_t internal_state;
    uint64_t internal_zero;
    uint8_t keys[16];
} CryptoContext;

/* Public API */
/** @brief Derive cipher keys from provided seed bytes. */
void create_keys_rc2(CryptoContext* ctx, const unsigned char key1[], size_t size1);
/** @brief Decrypt a 49-bit frame using the composite cipher suite. */
void decrypt_rc2(CryptoContext* ctx, uint8_t bits[49]);

#endif
