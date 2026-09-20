// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/dmr_keystream.h>
#include <stddef.h>
#include <stdint.h>

uint32_t
dmr_mi_advance32(uint32_t mi) {
    uint64_t lfsr = mi;
    for (unsigned int i = 0; i < 32U; i++) {
        const uint64_t bit = ((lfsr >> 31U) ^ (lfsr >> 3U) ^ (lfsr >> 1U)) & 1U;
        lfsr = (lfsr << 1U) | bit;
    }
    return (uint32_t)lfsr;
}

uint32_t
dmr_aes_expand_iv(uint32_t mi, uint8_t iv[16]) {
    if (iv == NULL) {
        return mi;
    }
    for (unsigned int i = 0; i < 4U; i++) {
        iv[i] = (uint8_t)(mi >> (24U - 8U * i));
    }
    uint32_t lfsr = mi;
    for (unsigned int i = 4; i < 16U; i++) {
        uint8_t octet = 0;
        for (unsigned int j = 0; j < 8U; j++) {
            const uint32_t bit = ((lfsr >> 31U) ^ (lfsr >> 21U) ^ (lfsr >> 1U) ^ lfsr) & 1U;
            lfsr = (lfsr << 1U) | bit;
            octet = (uint8_t)((octet << 1U) | bit);
        }
        iv[i] = octet;
    }
    return ((uint32_t)iv[4] << 24U) | ((uint32_t)iv[5] << 16U) | ((uint32_t)iv[6] << 8U) | iv[7];
}
