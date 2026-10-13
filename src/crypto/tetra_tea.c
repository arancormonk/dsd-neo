// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA air-interface keystream wrapper around the pinned TETRA_crypto
 * generators (cmake/TetraCrypto.cmake). This is the only project file that
 * includes their headers.
 */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/tetra_tea.h>

#include <stddef.h>
#include <stdint.h>

#include <tea1.h>
#include <tea2.h>
#include <tea3.h>

/* TEA1's 32-bit key register, built as TETRA_crypto's tea1_init_key_register() builds it but in an unsigned register:
 * upstream shifts a signed int32_t left past its range, which C leaves undefined. Two's complement gives the same bits,
 * and the register's top byte is masked to 8 bits either way, so the key stream is unchanged. */
static uint32_t
tetra_tea1_key_register(const uint8_t* key) {
    uint32_t reg = 0;
    for (int i = 0; i < 10; i++) {
        reg = (reg << 8) | g_abTea1Sbox[((reg >> 24) ^ key[i] ^ reg) & 0xFFu];
    }
    return reg;
}

void
tetra_tea_keystream(int alg, const uint8_t key[10], uint32_t iv, uint8_t* out, int nbytes) {
    uint8_t key_copy[10];
    if (!key || !out || nbytes <= 0) {
        return;
    }
    /* TEA2 and TEA3 take a non-const key pointer; hand them a scratch copy. */
    DSD_MEMCPY(key_copy, key, sizeof(key_copy));
    if (alg == 0) {
        uint32_t key_register = tetra_tea1_key_register(key_copy);
        tea1_inner(tea1_expand_iv(iv), key_register, (uint32_t)nbytes, out);
        DSD_SECURE_ZERO(&key_register, sizeof(key_register));
    } else if (alg == 1) {
        tea2(iv, key_copy, (uint32_t)nbytes, out);
    } else if (alg == 2) {
        tea3(iv, key_copy, (uint32_t)nbytes, out);
    }
    DSD_SECURE_ZERO(key_copy, sizeof(key_copy));
}

/* Append the low @p bits bits of @p value to @p mask at bit @p *pos, most significant first. */
static void
tetra_tb5_put(uint8_t* mask, int* pos, uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        if (((value >> i) & 1u) != 0u) {
            mask[*pos / 8] = (uint8_t)(mask[*pos / 8] | (0x80u >> (*pos % 8)));
        }
        (*pos)++;
    }
}

/* EN 300 392-7 names TB5's inputs and output but leaves its definition to the TETRA algorithm specifications. As
 * TETRA_crypto implements it, the ECK is the cipher key XORed with an 80-bit mask of the location area (14 bits)
 * followed by the carrier number (12 bits) and colour code (6 bits) three times and the carrier number once more,
 * most significant bit first. */
void
tetra_tea_derive_eck(uint16_t cn, uint16_t la, uint8_t cc, const uint8_t* ck, uint8_t* eck) {
    uint8_t mask[10];
    int pos = 0;
    if (!ck || !eck) {
        return;
    }
    DSD_MEMSET(mask, 0, sizeof(mask));
    tetra_tb5_put(mask, &pos, la & 0x3FFFu, 14);
    for (int i = 0; i < 3; i++) {
        tetra_tb5_put(mask, &pos, cn & 0x0FFFu, 12);
        tetra_tb5_put(mask, &pos, cc & 0x3Fu, 6);
    }
    tetra_tb5_put(mask, &pos, cn & 0x0FFFu, 12);
    for (size_t i = 0; i < sizeof(mask); i++) {
        eck[i] = (uint8_t)(ck[i] ^ mask[i]);
    }
}

uint32_t
tetra_tea_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir) {
    if (tn < 1 || tn > 4 || fn < 1 || fn > 18 || mn < 1 || mn > 60 || dir > 1) {
        return 0;
    }
    /* EN 300 392-7 clause 6.3.2.1: TN-1 in bits 0-1, FN in 2-6, MN in 7-12,
     * the low 15 bits of HN in 13-27 and the direction in bit 28. */
    return (uint32_t)(tn - 1u) | ((uint32_t)fn << 2) | ((uint32_t)mn << 7) | (((uint32_t)hn & 0x7FFFu) << 13)
           | ((uint32_t)dir << 28);
}
