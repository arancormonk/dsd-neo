// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "nxdn_crc.h"

uint32_t
nxdn_crc32_bits(const uint8_t* bits, size_t bit_count) {
    uint32_t crc = 0xFFFFFFFFU;

    if (bits == NULL || bit_count == 0U) {
        return crc;
    }

    for (size_t i = 0U; i < bit_count; i++) {
        const uint32_t input_bit = (uint32_t)(bits[i] & 1U);
        const uint32_t feedback = ((crc >> 31U) & 1U) ^ input_bit;
        crc <<= 1U;
        if (feedback != 0U) {
            crc ^= 0x04C11DB7U;
        }
    }

    return crc;
}
