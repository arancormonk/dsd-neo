// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN PN95 dibit descrambler.
 */

#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t scramble_t[182] = { // values are the position values we need to invert in the descramble
    2,   5,   6,   7,   10,  12,  14,  16,  17,  22,  23,  25,  26,  27,  28,  30,  33,  34,  36,  37,  38,  41,  45,
    47,  52,  54,  56,  57,  59,  62,  63,  64,  65,  66,  67,  69,  70,  73,  76,  79,  81,  82,  84,  85,  86,  87,
    88,  89,  92,  95,  96,  98,  100, 103, 104, 107, 108, 116, 117, 121, 122, 125, 127, 131, 132, 134, 137, 139, 140,
    141, 142, 143, 144, 145, 147, 151, 153, 154, 158, 159, 160, 162, 164, 165, 168, 170, 171, 174, 175, 176, 177, 181};

static uint16_t
nxdn_pn95_normalized_seed(uint16_t seed) {
    if (seed == 0U) {
        return 228U;
    }
    if (seed > 0x1FFU) {
        return 0x1FFU;
    }
    return seed;
}

void
nxdn_descramble_with_seed(uint8_t dibits[], int len, uint16_t seed) {
    if (dibits == NULL || len <= 0) {
        return;
    }

    seed = nxdn_pn95_normalized_seed(seed);
    if (seed == 228U) {
        for (size_t i = 0; i < sizeof(scramble_t) / sizeof(scramble_t[0]); i++) {
            if (scramble_t[i] >= len) {
                break;
            }
            dibits[scramble_t[i]] ^= 0x2; // invert sign of scrambled dibits
        }
        return;
    }

    uint16_t lfsr = seed;
    const int limit = (len < 182) ? len : 182;
    for (int i = 0; i < limit; i++) {
        const uint8_t pn = (uint8_t)(lfsr & 1U);
        if (pn != 0U) {
            dibits[i] ^= 0x2;
        }
        const uint16_t bit = (uint16_t)(((lfsr >> 4) ^ lfsr) & 1U);
        lfsr = (uint16_t)((lfsr >> 1) | (uint16_t)(bit << 8));
    }
}
