// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include "dmr_rs_test_encoder.h"
#include <dsd-neo/core/safe_api.h>
#include <stddef.h>

static uint8_t
gf_multiply(uint8_t a, uint8_t b) {
    uint16_t factor = a;
    uint8_t product = 0;
    while (b != 0) {
        if (b & 1U) {
            product ^= (uint8_t)factor;
        }
        factor <<= 1;
        if (factor & 0x100U) {
            factor ^= 0x11DU;
        }
        b >>= 1;
    }
    return product;
}

void
dmr_test_encode_rs_12_9(const uint8_t data[9], uint8_t parity[3]) {
    /* Descending coefficients of (x + alpha)(x + alpha^2)(x + alpha^3). */
    uint8_t generator[4] = {1, 0, 0, 0};
    const uint8_t roots[3] = {2, 4, 8};
    for (size_t root = 0; root < 3; ++root) {
        for (size_t j = root + 1; j > 0; --j) {
            generator[j] ^= gf_multiply(generator[j - 1], roots[root]);
        }
    }
    uint8_t word[12] = {0};
    DSD_MEMCPY(word, data, 9);
    for (size_t i = 0; i < 9; ++i) {
        uint8_t factor = word[i];
        for (size_t j = 0; j < 4; ++j) {
            word[i + j] ^= gf_multiply(factor, generator[j]);
        }
    }
    DSD_MEMCPY(parity, word + 9, 3);
}
