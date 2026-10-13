// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA air-interface keystream wrapper around the pinned TETRA_crypto
 * generators (cmake/TetraCrypto.cmake). This is the only project file that
 * includes their headers.
 */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/tetra_tea.h>

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
