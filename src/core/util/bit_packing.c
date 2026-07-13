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
