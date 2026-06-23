// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static void
build_slc17(uint8_t slc[17], uint16_t payload) {
    for (int i = 0; i < 12; i++) {
        slc[i] = (uint8_t)((payload >> (11 - i)) & 1U);
    }
    slc[12] = slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[6] ^ slc[7] ^ slc[9];
    slc[13] = slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[7] ^ slc[8] ^ slc[10];
    slc[14] = slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[5] ^ slc[8] ^ slc[9] ^ slc[11];
    slc[15] = slc[0] ^ slc[1] ^ slc[4] ^ slc[5] ^ slc[7] ^ slc[10];
    slc[16] = slc[0] ^ slc[1] ^ slc[2] ^ slc[5] ^ slc[6] ^ slc[8] ^ slc[11];
}

static void
test_hamming17123_single_bit_correction(void) {
    uint8_t expected[17];
    build_slc17(expected, 0xA53U);

    for (int bit = 0; bit < 17; bit++) {
        uint8_t word[17];
        DSD_MEMCPY(word, expected, sizeof(word));
        word[bit] ^= 1U;
        assert(Hamming17123(word) == true);
        assert(memcmp(word, expected, sizeof(word)) == 0);
    }
}

static void
test_hamming17123_uncorrectable_word_is_rejected(void) {
    uint8_t word[17];
    uint8_t expected[17];
    build_slc17(expected, 0x5A6U);
    DSD_MEMCPY(word, expected, sizeof(word));

    word[0] ^= 1U;
    word[2] ^= 1U;
    assert(Hamming17123(word) == false);
}

int
main(void) {
    // Build a 16-bit pattern 0xABCD in MSB-first bit order
    uint8_t bits[16];
    uint16_t val = 0xABCD;
    for (int i = 0; i < 16; i++) {
        bits[i] = (uint8_t)((val >> (15 - i)) & 1);
    }
    uint64_t out = ConvertBitIntoBytes(bits, 16);
    assert(out == 0xABCDULL);

    // Zero-length converts to 0
    assert(ConvertBitIntoBytes(bits, 0) == 0ULL);

    test_hamming17123_single_bit_correction();
    test_hamming17123_uncorrectable_word_is_rejected();

    printf("DMR bit utils: OK\n");
    return 0;
}
