// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/fec/dmr_late_entry.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static void
append_bits(uint8_t* dst, unsigned start, uint32_t val, unsigned k) {
    // Append k bits of val MSB-first into dst starting at index 'start'
    for (unsigned i = 0; i < k; i++) {
        unsigned bit = (val >> (k - 1 - i)) & 1U;
        dst[start + i] = (uint8_t)bit;
    }
}

static void
test_crc7_append_property(void) {
    uint8_t bits[32];
    DSD_MEMSET(bits, 0, sizeof(bits));
    // message: 13 arbitrary bits
    unsigned L = 13;
    uint8_t msg[32] = {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1};
    DSD_MEMCPY(bits, msg, L);
    uint8_t c = crc7(bits, L);
    // build augmented vector with 7 CRC bits appended (MSB-first)
    uint8_t aug[64];
    DSD_MEMCPY(aug, bits, L);
    append_bits(aug, L, c, 7);
    // property: remainder over [msg|crc] is 0
    assert(crc7(aug, L + 7) == 0);
}

static void
test_crc8_append_property(void) {
    uint8_t bits[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    unsigned L = 17;
    for (unsigned i = 0; i < L; i++) {
        bits[i] = (i * 3) & 1U; // pattern
    }
    uint8_t c = crc8(bits, L);
    uint8_t aug[64];
    DSD_MEMCPY(aug, bits, L);
    append_bits(aug, L, c, 8);
    assert(crc8(aug, L + 8) == 0);
}

static void
test_crc7_crc8_length_guards(void) {
    uint8_t bits[256];
    DSD_MEMSET(bits, 1, sizeof(bits));

    assert(crc8(bits, 248U) != 0U);
    assert(crc8(bits, 249U) == 0U);
    assert(crc7(bits, 249U) != 0U);
    assert(crc7(bits, 250U) == 0U);
}

static void
test_crc5_sum_mod31_contract(void) {
    uint8_t zero_bits[72];
    uint8_t patterned_bits[72];
    DSD_MEMSET(zero_bits, 0, sizeof(zero_bits));
    DSD_MEMSET(patterned_bits, 0, sizeof(patterned_bits));

    assert(ComputeCrc5Bit(zero_bits) == 0U);

    for (unsigned byte = 0; byte < 9U; byte++) {
        uint8_t value = (uint8_t)(byte + 1U);
        for (unsigned bit = 0; bit < 8U; bit++) {
            patterned_bits[(byte * 8U) + bit] = (uint8_t)((value >> (7U - bit)) & 1U);
        }
    }
    assert(ComputeCrc5Bit(patterned_bits) == 14U);
}

static void
test_crc3_append_property(void) {
    uint8_t bits[16] = {1, 1, 0, 1, 0, 0, 1, 1};
    unsigned L = 8;
    uint8_t c = crc3(bits, L);
    uint8_t aug[32];
    DSD_MEMCPY(aug, bits, L);
    append_bits(aug, L, c, 3);
    assert(crc3(aug, L + 3) == 0);
}

static void
test_crc4_append_property(void) {
    uint8_t bits[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    unsigned L = 11;
    for (unsigned i = 0; i < L; i++) {
        bits[i] = (i ^ 3) & 1U;
    }
    uint8_t c_inv = dsd_dmr_crc4(bits, L); // function returns inverted remainder
    uint8_t c = (uint8_t)(c_inv ^ 0x0F);   // get actual remainder
    uint8_t aug[40];
    DSD_MEMCPY(aug, bits, L);
    append_bits(aug, L, c, 4);
    // Over augmented message, remainder is 0, function returns 0^0xF = 0xF
    assert(dsd_dmr_crc4(aug, L + 4) == 0x0F);
}

static void
test_ccitt_zeros(void) {
    // 80 zero bits -> CRC should be 0xFFFF with this implementation
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    uint16_t crc = ComputeCrcCCITT(bits);
    assert(crc == 0xFFFF);
}

int
main(void) {
    test_crc7_append_property();
    test_crc8_append_property();
    test_crc7_crc8_length_guards();
    test_crc5_sum_mod31_contract();
    test_crc3_append_property();
    test_crc4_append_property();
    test_ccitt_zeros();
    printf("DMR CRC properties: OK\n");
    return 0;
}
