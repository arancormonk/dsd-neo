// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

static inline uint64_t
pack_8_bits_msb(const uint8_t* bits) {
    return ((uint64_t)(bits[0] & 1U) << 7) | ((uint64_t)(bits[1] & 1U) << 6) | ((uint64_t)(bits[2] & 1U) << 5)
           | ((uint64_t)(bits[3] & 1U) << 4) | ((uint64_t)(bits[4] & 1U) << 3) | ((uint64_t)(bits[5] & 1U) << 2)
           | ((uint64_t)(bits[6] & 1U) << 1) | (uint64_t)(bits[7] & 1U);
}

uint64_t
convert_bits_into_output(const uint8_t* input, uint32_t len) {
    uint64_t output = 0;
    const uint8_t* bits = input;

    while (len >= 8U) {
        output = (output << 8) | pack_8_bits_msb(bits);
        bits += 8;
        len -= 8U;
    }
    while (len-- > 0U) {
        output = (output << 1) | (uint64_t)(*bits++ & 1U);
    }
    return output;
}

uint16_t
dsd_crc_ccitt16_bits(const uint8_t* input, size_t bit_count) {
    if (input == NULL) {
        return 0U;
    }

    uint16_t crc = 0U;
    for (size_t i = 0U; i < bit_count; i++) {
        const uint16_t feedback = (uint16_t)(((crc >> 15U) & 1U) ^ (input[i] & 1U));
        crc = (uint16_t)(crc << 1U);
        if (feedback != 0U) {
            crc ^= 0x1021U;
        }
    }
    return (uint16_t)(crc ^ 0xFFFFU);
}
