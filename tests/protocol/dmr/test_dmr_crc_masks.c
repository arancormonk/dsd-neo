// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * CRC mask tests for DMR: verifies 24-bit LC masks (VLC/TLC) via
 * ComputeAndCorrectFullLinkControlCrc and 16-bit CCITT masks for
 * PI/CSBK/MBC Header/Data Header/USBD via ComputeCrcCCITT.
 */

#include <dsd-neo/core/bit_packing.h>

#include <assert.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

// Forward declaration of LC CRC helper
uint32_t ComputeAndCorrectFullLinkControlCrc(uint8_t* FullLinkControlDataBytes, uint32_t* CRCComputed,
                                             uint32_t CRCMask);

static void
build_masked_lc_codeword(uint8_t cw[12], uint32_t mask24, uint32_t* parity_unmasked) {
    static const uint8_t reference[12] = {0x10, 0x17, 0x1E, 0x25, 0x2C, 0x33, 0x3A, 0x41, 0x48, 0x90, 0x6C, 0x2C};
    DSD_MEMCPY(cw, reference, sizeof(reference));
    *parity_unmasked = 0x906C2CU;
    cw[9] ^= (uint8_t)(mask24 >> 16);
    cw[10] ^= (uint8_t)(mask24 >> 8);
    cw[11] ^= (uint8_t)mask24;
}

static void
append_bits(uint8_t* dst, unsigned start, uint32_t val, unsigned k) {
    // Append k bits of val MSB-first into dst starting at index 'start'
    for (unsigned i = 0; i < k; i++) {
        unsigned bit = (val >> (k - 1 - i)) & 1U;
        dst[start + i] = (uint8_t)bit;
    }
}

static void
test_lc_crc24_mask(uint32_t mask24) {
    uint8_t cw[12];
    uint32_t parity_unmasked = 0;
    build_masked_lc_codeword(cw, mask24, &parity_unmasked);

    // Feed into CRC check/correct with the same mask
    uint32_t crc_computed = 0;
    uint32_t ok = ComputeAndCorrectFullLinkControlCrc(cw, &crc_computed, mask24);
    assert(ok == 1);
    assert(crc_computed == parity_unmasked);

    // Ensure output remains masked.
    assert(cw[9] == (uint8_t)(0x90U ^ (uint8_t)(mask24 >> 16)));
    assert(cw[10] == (uint8_t)(0x6CU ^ (uint8_t)(mask24 >> 8)));
    assert(cw[11] == (uint8_t)(0x2CU ^ (uint8_t)mask24));
}

static void
test_lc_crc24_corrects_single_byte_error(void) {
    uint8_t cw[12];
    uint8_t expected[12];
    uint32_t parity_unmasked = 0;
    const uint32_t mask24 = 0x969696U;
    build_masked_lc_codeword(cw, mask24, &parity_unmasked);
    DSD_MEMCPY(expected, cw, sizeof(expected));

    cw[2] ^= 0x55U;
    uint32_t crc_computed = 0;
    uint32_t ok = ComputeAndCorrectFullLinkControlCrc(cw, &crc_computed, mask24);
    assert(ok == 1);
    assert(crc_computed == parity_unmasked);
    assert(memcmp(cw, expected, sizeof(cw)) == 0);
}

static void
test_lc_crc24_rejects_uncorrectable_error(void) {
    uint8_t cw[12];
    uint32_t parity_unmasked = 0;
    const uint32_t mask24 = 0x999999U;
    build_masked_lc_codeword(cw, mask24, &parity_unmasked);

    cw[1] ^= 0x22U;
    cw[7] ^= 0x11U;
    uint32_t crc_computed = 0;
    uint32_t ok = ComputeAndCorrectFullLinkControlCrc(cw, &crc_computed, mask24);
    assert(ok == 0);
}

static void
test_ccitt16_mask(uint16_t mask16) {
    // Simulate BPTC(196,96) deinterleaved payload: first 80 bits are info,
    // last 16 bits [80..95] carry the masked CCITT CRC.
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));

    // Fill 80 info bits with a deterministic pattern
    for (unsigned i = 0; i < 80; i++) {
        bits[i] = (uint8_t)(((i * 5) ^ 0x3) & 1U);
    }

    // Compute CCITT over first 80 bits
    uint16_t ccitt = ComputeCrcCCITT(bits);
    uint16_t masked = (uint16_t)(ccitt ^ mask16);

    // Place masked CRC at [80..95], MSB-first
    append_bits(bits, 80, masked, 16);

    // Emulate extraction
    uint16_t ext = (uint16_t)convert_bits_into_output(&bits[80], 16);
    ext ^= mask16;
    uint16_t cmp = ComputeCrcCCITT(bits);
    assert(ext == cmp);

    // Negative: flip an info bit and require mismatch
    bits[37] ^= 1U;
    cmp = ComputeCrcCCITT(bits);
    assert(ext != cmp);
}

int
main(void) {
    // 24-bit LC masks (VLC/TLC)
    test_lc_crc24_mask(0x969696); // VLC
    test_lc_crc24_mask(0x999999); // TLC
    test_lc_crc24_corrects_single_byte_error();
    test_lc_crc24_rejects_uncorrectable_error();

    // 16-bit CCITT masks for other PDUs
    test_ccitt16_mask(0x6969); // PI
    test_ccitt16_mask(0xA5A5); // CSBK
    test_ccitt16_mask(0xAAAA); // MBC Header
    test_ccitt16_mask(0xCCCC); // Data Header
    test_ccitt16_mask(0x3333); // USBD

    printf("DMR CRC masks: OK\n");
    return 0;
}
