// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * SlotType Golay(20,8) correction tests using fixed reference codewords.
 * - Verifies clean decode
 * - Verifies up to 2 bit error correction
 * - Verifies failure on 3 bit flips
 */

#include <assert.h>
#include <dsd-neo/fec/block_codes.h>
#include <stdbool.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static unsigned char
byte_from_bits(const unsigned char in[8]) {
    unsigned char v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (unsigned char)(in[i] & 1U) << i;
    }
    return v;
}

static void
test_clean_decode(void) {
    static const struct {
        unsigned char value;
        unsigned char codeword[20];
    } fixtures[] = {
        {0x00U, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x34U, {0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0}},
        {0xA5U, {1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1}},
    };

    Golay_20_8_init();
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        unsigned char received[20];
        DSD_MEMCPY(received, fixtures[i].codeword, sizeof(received));
        assert(Golay_20_8_decode(received) == true);
        assert(byte_from_bits(received) == fixtures[i].value);
    }
}

static void
test_two_bit_correction(void) {
    static const unsigned char codeword[20] = {0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0};
    Golay_20_8_init();
    for (int i = 0; i < 20; i++) {
        for (int j = i + 1; j < 20; j++) {
            unsigned char tmp[20];
            DSD_MEMCPY(tmp, codeword, sizeof(tmp));
            tmp[i] ^= 1U;
            tmp[j] ^= 1U;
            assert(Golay_20_8_decode(tmp) == true);
            assert(byte_from_bits(tmp) == 0x34U);
        }
    }
}

static void
test_three_bit_failure(void) {
    Golay_20_8_init();
    // Flip three distinct positions
    unsigned char tmp[20] = {0};
    tmp[0] ^= 1U;
    tmp[5] ^= 1U;
    tmp[13] ^= 1U;
    // Implementation limits correction to at most 2 flips
    assert(Golay_20_8_decode(tmp) == false);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    test_clean_decode();
    test_two_bit_correction();
    test_three_bit_failure();
    printf("DMR SlotType Golay(20,8): OK\n");
    return 0;
}
