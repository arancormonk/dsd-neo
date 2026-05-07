// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for BCH(63,16,11) encoder/decoder.
 *
 * Validates encode/decode correctness, GF(2^6) field properties, error
 * detection limits, and backward-compatible legacy interface.
 */

#include <dsd-neo/fec/BCH_63_16.hpp>

#include <cstdio>
#include <cstring>

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

    std::memset(info, 0, sizeof(info));
    bch.encode(info, codeword);

    for (int i = 0; i < 63; i++) {
        if (codeword[i] != 0) {
            std::fprintf(stderr, "test_encode_all_zeros: expected codeword[%d]=0, got %d\n", i, (int)codeword[i]);
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
            std::fprintf(stderr,
                         "test_encode_nac_293_ldu1: systematic check failed at bit %d: "
                         "expected %d, got %d\n",
                         i, (int)info[i], (int)codeword[i]);
            return 1;
        }
    }

    // Decode the clean codeword — should succeed with 0 errors
    char decoded[16];
    BCH_63_16_Result result = bch.decode(codeword, decoded);

    if (!result.success) {
        std::fprintf(stderr, "test_encode_nac_293_ldu1: decode of clean codeword failed\n");
        return 1;
    }
    if (result.error_count != 0) {
        std::fprintf(stderr, "test_encode_nac_293_ldu1: expected error_count=0, got %d\n", result.error_count);
        return 1;
    }

    // Verify decoded bits match original info
    if (std::memcmp(decoded, info, 16) != 0) {
        std::fprintf(stderr, "test_encode_nac_293_ldu1: decoded bits do not match input\n");
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
    BCH_63_16_Result result = bch.decode(codeword, decoded);

    if (!result.success) {
        std::fprintf(stderr, "test_decode_no_errors: decode failed on clean codeword\n");
        return 1;
    }
    if (result.error_count != 0) {
        std::fprintf(stderr, "test_decode_no_errors: expected error_count=0, got %d\n", result.error_count);
        return 1;
    }
    if (std::memcmp(decoded, info, 16) != 0) {
        std::fprintf(stderr, "test_decode_no_errors: decoded bits do not match input\n");
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

    std::memset(info, 0, sizeof(info));
    bch.encode(info, codeword);

    // Copy and flip 12 distinct bit positions
    std::memcpy(corrupted, codeword, 63);
    int flip_positions[12] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55};
    for (int i = 0; i < 12; i++) {
        corrupted[flip_positions[i]] ^= 1;
    }

    BCH_63_16_Result result = bch.decode(corrupted, decoded);

    if (result.success) {
        std::fprintf(stderr,
                     "test_decode_failure_12_errors: expected decode failure, "
                     "but got success with error_count=%d\n",
                     result.error_count);
        return 1;
    }
    if (result.error_count != 0) {
        std::fprintf(stderr,
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
    //    decode successfully — this exercises all 63 field elements in the
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
        std::memcpy(corrupted, codeword, 63);

        // Flip 11 distinct positions spread across the codeword
        int positions[11] = {0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60};
        for (int i = 0; i < 11; i++) {
            corrupted[positions[i]] ^= 1;
        }

        BCH_63_16_Result result = bch.decode(corrupted, decoded);
        if (!result.success) {
            std::fprintf(stderr, "test_gf_field_properties: 11-error decode failed "
                                 "(field arithmetic error)\n");
            return 1;
        }
        if (result.error_count != 11) {
            std::fprintf(stderr, "test_gf_field_properties: expected error_count=11, got %d\n", result.error_count);
            return 1;
        }
        if (std::memcmp(decoded, info, 16) != 0) {
            std::fprintf(stderr, "test_gf_field_properties: 11-error decode produced wrong bits\n");
            return 1;
        }
    }

    // Test 2: Verify injectivity — two different info words produce different codewords
    // This confirms the GF tables generate a proper code (no collisions)
    {
        char info_a[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        char info_b[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
        char cw_a[63], cw_b[63];

        bch.encode(info_a, cw_a);
        bch.encode(info_b, cw_b);

        if (std::memcmp(cw_a, cw_b, 63) == 0) {
            std::fprintf(stderr, "test_gf_field_properties: different info words produced "
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
            std::memcpy(corrupted, codeword, 63);
            corrupted[pos] ^= 1;

            BCH_63_16_Result result = bch.decode(corrupted, decoded);
            if (!result.success) {
                std::fprintf(stderr,
                             "test_gf_field_properties: single-error at pos %d failed "
                             "(missing field element alpha^%d)\n",
                             pos, pos);
                return 1;
            }
            if (result.error_count != 1) {
                std::fprintf(stderr,
                             "test_gf_field_properties: single-error at pos %d "
                             "reported %d errors instead of 1\n",
                             pos, result.error_count);
                return 1;
            }
            if (std::memcmp(decoded, info, 16) != 0) {
                std::fprintf(stderr,
                             "test_gf_field_properties: single-error at pos %d "
                             "decoded wrong bits\n",
                             pos);
                return 1;
            }
        }
    }

    return 0;
}

/**
 * @brief Verify that the legacy bool decode interface still works.
 *
 * The decode_legacy() wrapper should return true on success and false
 * on failure, discarding the error count.
 *
 * Validates: Requirements 5.2, 5.3
 */
static int
test_backward_compat_legacy_decode(void) {
    BCH_63_16_11 bch;

    // Test success case
    {
        char info[16] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0};
        char codeword[63];
        char decoded[16];

        bch.encode(info, codeword);

        // Introduce 3 errors
        codeword[2] ^= 1;
        codeword[20] ^= 1;
        codeword[40] ^= 1;

        bool ok = bch.decode_legacy(codeword, decoded);
        if (!ok) {
            std::fprintf(stderr, "test_backward_compat_legacy_decode: expected true, got false\n");
            return 1;
        }
        if (std::memcmp(decoded, info, 16) != 0) {
            std::fprintf(stderr, "test_backward_compat_legacy_decode: decoded bits mismatch\n");
            return 1;
        }
    }

    // Test failure case (12 errors)
    {
        char info[16];
        char codeword[63];
        char decoded[16];

        std::memset(info, 0, sizeof(info));
        bch.encode(info, codeword);

        // Flip 12 bits
        for (int i = 0; i < 12; i++) {
            codeword[i * 5] ^= 1;
        }

        bool ok = bch.decode_legacy(codeword, decoded);
        if (ok) {
            std::fprintf(stderr, "test_backward_compat_legacy_decode: expected false on 12 errors, "
                                 "got true\n");
            return 1;
        }
    }

    return 0;
}

int
main(void) {
    int rc = 0;

    rc |= test_encode_all_zeros();
    rc |= test_encode_nac_293_ldu1();
    rc |= test_decode_no_errors();
    rc |= test_decode_failure_12_errors();
    rc |= test_gf_field_properties();
    rc |= test_backward_compat_legacy_decode();

    if (rc == 0) {
        std::printf("BCH(63,16,11) unit tests passed.\n");
    }

    return rc;
}
