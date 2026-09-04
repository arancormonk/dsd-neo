// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for the BCH(63,16,11) decoder.
 *
 * Validates decode correctness, GF(2^6) field properties, and error detection
 * limits using codewords built from an independent P25 generator matrix.
 */

#include <cstdio>
#include <cstring>
#include <dsd-neo/fec/BCH_63_16.hpp>
#include "dsd-neo/core/safe_api.h"
#include "p25_nid_generator.hpp"

/**
 * @brief Verify that the decoder accepts a valid NAC=0x293, DUID=0x5 (LDU1)
 *        codeword with zero corrections.
 *
 * The 16-bit info word is:
 *   NAC=0x293 (12 bits): 0010 1001 0011
 *   DUID=0x5  (4 bits):  0101
 *   Combined (MSB first): 0010 1001 0011 0101
 *
 * Validates: Requirements 2.4, 6.3
 */
static int
test_decode_nac_293_ldu1(void) {
    BCH_63_16_11 bch;

    // NAC=0x293 in binary (12 bits, MSB first): 0010 1001 0011
    // DUID=0x5 in binary (4 bits, MSB first): 0101
    // Combined 16-bit info word (MSB first): 0010 1001 0011 0101
    char info[16] = {0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1};
    char codeword[63];

    p25_test_generate_nid_codeword(info, codeword);

    // Decode the clean codeword - should succeed with 0 errors
    char decoded[16];
    BCH_63_16_Result result = bch.decode_with_result(codeword, decoded);

    if (!result.success) {
        DSD_FPRINTF(stderr, "test_decode_nac_293_ldu1: decode of clean codeword failed\n");
        return 1;
    }
    if (result.error_count != 0) {
        DSD_FPRINTF(stderr, "test_decode_nac_293_ldu1: expected error_count=0, got %d\n", result.error_count);
        return 1;
    }

    // Verify decoded bits match original info
    if (std::memcmp(decoded, info, 16) != 0) {
        DSD_FPRINTF(stderr, "test_decode_nac_293_ldu1: decoded bits do not match input\n");
        return 1;
    }

    return 0;
}

/**
 * @brief Verify that decoding a clean (error-free) codeword succeeds
 *        with error_count=0.
 *
 * Validates: Requirements 5.2
 */
static int
test_decode_no_errors(void) {
    BCH_63_16_11 bch;

    // Use a non-trivial info word: 1010 1010 1010 1010
    char info[16] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    char codeword[63];
    char decoded[16];

    p25_test_generate_nid_codeword(info, codeword);
    BCH_63_16_Result result = bch.decode_with_result(codeword, decoded);

    if (!result.success) {
        DSD_FPRINTF(stderr, "test_decode_no_errors: decode failed on clean codeword\n");
        return 1;
    }
    if (result.error_count != 0) {
        DSD_FPRINTF(stderr, "test_decode_no_errors: expected error_count=0, got %d\n", result.error_count);
        return 1;
    }
    if (std::memcmp(decoded, info, 16) != 0) {
        DSD_FPRINTF(stderr, "test_decode_no_errors: decoded bits do not match input\n");
        return 1;
    }

    return 0;
}

/**
 * @brief Verify that 12 bit errors (beyond t=11 correction capability)
 *        cause decode failure with no error count.
 *
 * Validates: Requirements 1.5, 5.3
 */
static int
test_decode_failure_12_errors(void) {
    BCH_63_16_11 bch;

    // Generate the all-zero info word (simplest valid codeword).
    char info[16];
    char codeword[63];
    char corrupted[63];
    char decoded[16];

    DSD_MEMSET(info, 0, sizeof(info));
    p25_test_generate_nid_codeword(info, codeword);

    // Copy and flip 12 distinct bit positions
    DSD_MEMCPY(corrupted, codeword, 63);
    int flip_positions[12] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55};
    for (int i = 0; i < 12; i++) {
        corrupted[flip_positions[i]] ^= 1;
    }

    BCH_63_16_Result result = bch.decode_with_result(corrupted, decoded);

    if (result.success) {
        DSD_FPRINTF(stderr,
                    "test_decode_failure_12_errors: expected decode failure, "
                    "but got success with error_count=%d\n",
                    result.error_count);
        return 1;
    }
    if (result.error_count != 0) {
        DSD_FPRINTF(stderr,
                    "test_decode_failure_12_errors: expected error_count=0 on failure, "
                    "got %d\n",
                    result.error_count);
        return 1;
    }

    return 0;
}

/**
 * @brief Verify GF(2^6) field properties: alpha^63 = 1 and all 63
 *        nonzero elements are distinct.
 *
 * The BCH code operates over GF(2^6) with primitive polynomial x^6+x+1.
 * The primitive element alpha must satisfy alpha^63 = 1 (since the
 * multiplicative group has order 2^6 - 1 = 63), and the 63 powers
 * alpha^0 through alpha^62 must all be distinct nonzero elements.
 *
 * Validates: Requirements 2.1
 */
static int
test_gf_field_properties(void) {
    // We verify the field properties by decoding independently generated
    // codewords and checking the correction behavior that depends on the GF
    // tables.
    //
    // Direct access to alpha_to[] and index_of[] is not possible since
    // they are private. Instead, we verify the field indirectly:
    //
    // 1. Introduce exactly 11 errors (max correctable) into a valid word and
    //    decode successfully.
    // 2. Verify all 63 single-bit error positions are correctable, exercising
    //    the full Chien search.

    BCH_63_16_11 bch;

    // Test 1: Max correction (11 errors) exercises full Chien search over GF(2^6)
    {
        char info[16] = {1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0};
        char codeword[63];
        char corrupted[63];
        char decoded[16];

        p25_test_generate_nid_codeword(info, codeword);
        DSD_MEMCPY(corrupted, codeword, 63);

        // Flip 11 distinct positions spread across the codeword
        int positions[11] = {0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60};
        for (int i = 0; i < 11; i++) {
            corrupted[positions[i]] ^= 1;
        }

        BCH_63_16_Result result = bch.decode_with_result(corrupted, decoded);
        if (!result.success) {
            DSD_FPRINTF(stderr, "test_gf_field_properties: 11-error decode failed "
                                "(field arithmetic error)\n");
            return 1;
        }
        if (result.error_count != 11) {
            DSD_FPRINTF(stderr, "test_gf_field_properties: expected error_count=11, got %d\n", result.error_count);
            return 1;
        }
        if (std::memcmp(decoded, info, 16) != 0) {
            DSD_FPRINTF(stderr, "test_gf_field_properties: 11-error decode produced wrong bits\n");
            return 1;
        }
    }

    // Test 2: Verify all 63 single-bit error positions are correctable
    // This exercises alpha^i for all i in [0,62], confirming 63 distinct elements
    {
        char info[16] = {1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0};
        char codeword[63];
        char corrupted[63];
        char decoded[16];

        p25_test_generate_nid_codeword(info, codeword);

        for (int pos = 0; pos < 63; pos++) {
            DSD_MEMCPY(corrupted, codeword, 63);
            corrupted[pos] ^= 1;

            BCH_63_16_Result result = bch.decode_with_result(corrupted, decoded);
            if (!result.success) {
                DSD_FPRINTF(stderr,
                            "test_gf_field_properties: single-error at pos %d failed "
                            "(missing field element alpha^%d)\n",
                            pos, pos);
                return 1;
            }
            if (result.error_count != 1) {
                DSD_FPRINTF(stderr,
                            "test_gf_field_properties: single-error at pos %d "
                            "reported %d errors instead of 1\n",
                            pos, result.error_count);
                return 1;
            }
            if (std::memcmp(decoded, info, 16) != 0) {
                DSD_FPRINTF(stderr,
                            "test_gf_field_properties: single-error at pos %d "
                            "decoded wrong bits\n",
                            pos);
                return 1;
            }
        }
    }

    return 0;
}

int
main(void) {
    int rc = 0;

    rc |= test_decode_nac_293_ldu1();
    rc |= test_decode_no_errors();
    rc |= test_decode_failure_12_errors();
    rc |= test_gf_field_properties();
    if (rc == 0) {
        std::printf("BCH(63,16,11) decoder unit tests passed.\n");
    }

    return rc;
}
