// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for BCH(63,16,11) encoder/decoder.
 *
 * Validates encode/decode correctness, GF(2^6) field properties, and error
 * detection limits.
 */

#include <cstdio>
#include <cstring>
#include <dsd-neo/fec/BCH_63_16.hpp>
#include "dsd-neo/core/safe_api.h"

static unsigned long long
pack_63_bits(const char codeword[63]) {
    unsigned long long packed = 0;

    for (int i = 0; i < 63; i++) {
        packed <<= 1;
        packed |= codeword[i] ? 1ULL : 0ULL;
    }

    return packed;
}

/**
 * @brief Verify that encoding the all-zero 16-bit info word produces
 *        the all-zero 63-bit codeword.
 *
 * This is a fundamental property of any linear code: the zero word is
 * always a valid codeword.
 *
 * Validates: Requirements 2.3
 */
static int
test_encode_all_zeros(void) {
    BCH_63_16_11 bch;
    char info[16];
    char codeword[63];

    DSD_MEMSET(info, 0, sizeof(info));
    bch.encode(info, codeword);

    for (int i = 0; i < 63; i++) {
        if (codeword[i] != 0) {
            DSD_FPRINTF(stderr, "test_encode_all_zeros: expected codeword[%d]=0, got %d\n", i, (int)codeword[i]);
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Verify encoder output against the P25 NID generator matrix used by
 *        sdrtrunk for fixed test vectors.
 *
 * sdrtrunk's 16 generator rows include the 47 BCH parity bits plus the final
 * NID parity bit. The local encoder produces only the 63-bit BCH codeword, so
 * the expected value drops each row's final parity bit.
 */
static int
test_encode_matches_p25_generator_matrix(void) {
    BCH_63_16_11 bch;
    static const unsigned long long generator_rows[16] = {
        0xCD930BDD3B2AULL, 0xAB5A8E33A6BEULL, 0x983E4CC4E874ULL, 0x4C1F2662743AULL,
        0xEB9C98EC0136ULL, 0xB85D47AB3BB0ULL, 0x5C2EA3D59DD8ULL, 0x2E1751EACEECULL,
        0x170BA8F56776ULL, 0xC616DFA78890ULL, 0x630B6FD3C448ULL, 0x3185B7E9E224ULL,
        0x18C2DBF4F112ULL, 0xC1F2662743A2ULL, 0xAD6A38CE9AFBULL, 0x9B2617BA7657ULL,
    };

    for (int bit = 0; bit < 16; bit++) {
        char info[16] = {0};
        char codeword[63] = {0};

        info[bit] = 1;
        bch.encode(info, codeword);

        unsigned long long expected = (1ULL << (62 - bit)) | (generator_rows[bit] >> 1);
        unsigned long long actual = pack_63_bits(codeword);

        if (actual != expected) {
            DSD_FPRINTF(stderr, "test_encode_matches_p25_generator_matrix: bit %d expected 0x%016llX, got 0x%016llX\n",
                        bit, expected, actual);
            return 1;
        }
    }

    return 0;
}

/**
 * @brief Verify that encoding NAC=0x293, DUID=0x5 (LDU1) produces a
 *        valid codeword (zero syndrome on decode).
 *
 * The 16-bit info word is:
 *   NAC=0x293 (12 bits): 0010 1001 0011
 *   DUID=0x5  (4 bits):  0101
 *   Combined (MSB first): 0010 1001 0011 0101
 *
 * Validates: Requirements 2.4, 6.3
 */
static int
test_encode_nac_293_ldu1(void) {
    BCH_63_16_11 bch;

    // NAC=0x293 in binary (12 bits, MSB first): 0010 1001 0011
    // DUID=0x5 in binary (4 bits, MSB first): 0101
    // Combined 16-bit info word (MSB first): 0010 1001 0011 0101
    char info[16] = {0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1};
    char codeword[63];

    bch.encode(info, codeword);

    // Verify systematic property: first 16 bits must equal info
    for (int i = 0; i < 16; i++) {
        if (codeword[i] != info[i]) {
            DSD_FPRINTF(stderr,
                        "test_encode_nac_293_ldu1: systematic check failed at bit %d: "
                        "expected %d, got %d\n",
                        i, (int)info[i], (int)codeword[i]);
            return 1;
        }
    }

    // Decode the clean codeword - should succeed with 0 errors
    char decoded[16];
    BCH_63_16_Result result = bch.decode_with_result(codeword, decoded);

    if (!result.success) {
        DSD_FPRINTF(stderr, "test_encode_nac_293_ldu1: decode of clean codeword failed\n");
        return 1;
    }
    if (result.error_count != 0) {
        DSD_FPRINTF(stderr, "test_encode_nac_293_ldu1: expected error_count=0, got %d\n", result.error_count);
        return 1;
    }

    // Verify decoded bits match original info
    if (std::memcmp(decoded, info, 16) != 0) {
        DSD_FPRINTF(stderr, "test_encode_nac_293_ldu1: decoded bits do not match input\n");
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

    bch.encode(info, codeword);
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

    // Encode the all-zero info word (simplest valid codeword)
    char info[16];
    char codeword[63];
    char corrupted[63];
    char decoded[16];

    DSD_MEMSET(info, 0, sizeof(info));
    bch.encode(info, codeword);

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
    // We verify the field properties by instantiating the BCH class and
    // encoding/decoding to exercise the GF tables, then checking the
    // round-trip property which depends on correct field arithmetic.
    //
    // Direct access to alpha_to[] and index_of[] is not possible since
    // they are private. Instead, we verify the field indirectly:
    //
    // 1. Encode a known word, introduce exactly 11 errors (max correctable),
    //    decode successfully - this exercises all 63 field elements in the
    //    Chien search.
    // 2. Verify that the all-zero codeword property holds (linear code).
    // 3. Verify that distinct info words produce distinct codewords (injectivity).

    BCH_63_16_11 bch;

    // Test 1: Max correction (11 errors) exercises full Chien search over GF(2^6)
    {
        char info[16] = {1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0};
        char codeword[63];
        char corrupted[63];
        char decoded[16];

        bch.encode(info, codeword);
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

    // Test 2: Verify injectivity - two different info words produce different codewords
    // This confirms the GF tables generate a proper code (no collisions)
    {
        char info_a[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        char info_b[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
        char cw_a[63], cw_b[63];

        bch.encode(info_a, cw_a);
        bch.encode(info_b, cw_b);

        if (std::memcmp(cw_a, cw_b, 63) == 0) {
            DSD_FPRINTF(stderr, "test_gf_field_properties: different info words produced "
                                "identical codewords (field generation error)\n");
            return 1;
        }
    }

    // Test 3: Verify all 63 single-bit error positions are correctable
    // This exercises alpha^i for all i in [0,62], confirming 63 distinct elements
    {
        char info[16] = {1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0};
        char codeword[63];
        char corrupted[63];
        char decoded[16];

        bch.encode(info, codeword);

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

    rc |= test_encode_all_zeros();
    rc |= test_encode_matches_p25_generator_matrix();
    rc |= test_encode_nac_293_ldu1();
    rc |= test_decode_no_errors();
    rc |= test_decode_failure_12_errors();
    rc |= test_gf_field_properties();
    if (rc == 0) {
        std::printf("BCH(63,16,11) unit tests passed.\n");
    }

    return rc;
}
