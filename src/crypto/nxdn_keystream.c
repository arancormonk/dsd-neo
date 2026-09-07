// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#include <dsd-neo/crypto/nxdn_keystream.h>
#include <stddef.h>

void
nxdn_pdu_scrambler_keystream_creation(uint8_t* ks, int lfsr, int len_bits) {
    if (ks == NULL || len_bits <= 0) {
        return;
    }
    uint16_t reg = (uint16_t)lfsr & 0x7FFFU;
    for (int i = 0; i < len_bits; i++) {
        ks[i] = (uint8_t)(reg & 1U);
        const uint16_t bit = (uint16_t)(((reg >> 1) ^ reg) & 1U);
        reg = (uint16_t)((reg >> 1) | (bit << 14));
    }
}

void
nxdn_lfsr128_expand_iv_from_mi64(uint64_t mi, uint8_t out[16]) {
    if (out == NULL) {
        return;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(mi >> (56 - i * 8));
    }
    uint64_t lfsr = mi;
    for (int i = 8; i < 16; i++) {
        uint8_t octet = 0;
        for (int j = 0; j < 8; j++) {
            const uint64_t bit =
                ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 1U;
            lfsr = (lfsr << 1) | bit;
            octet = (uint8_t)((octet << 1) | bit);
        }
        out[i] = octet;
    }
}
