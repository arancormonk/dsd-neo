// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for check_NID() with full BCH correction, DUID validation,
 *        and parity override logic.
 *
 * This file is C++ because it uses the BCH encoder (BCH_63_16.hpp) to generate
 * valid codewords as test vectors. It includes both the C header for check_NID
 * and the C++ BCH class.
 *
 * Validates: Requirements 3.3, 4.1, 4.2, 4.3, 7.1, 7.2
 */

#include <dsd-neo/fec/BCH_63_16.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>

#include <cstdio>
#include <cstring>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Encode a NAC + DUID into a 16-bit info word (MSB first).
 *
 * Bits 0-11: NAC (12 bits, MSB first)
 * Bits 12-15: DUID (4 bits, MSB first)
 */
static void
make_info_word(int nac, int duid, char info[16]) {
    for (int i = 0; i < 12; i++) {
        info[i] = (char)((nac >> (11 - i)) & 1);
    }
    for (int i = 0; i < 4; i++) {
        info[12 + i] = (char)((duid >> (3 - i)) & 1);
    }
}

/**
 * @brief Get the expected parity bit for a given DUID value.
 *
 * Per TIA-102.BAAA-A Table 8-4: P=1 for LDU1 (0x5) and LDU2 (0xA),
 * P=0 for all other defined DUIDs.
 */
static unsigned char
expected_parity(int duid) {
    if (duid == 0x5 || duid == 0xA) {
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test: All 7 valid DUIDs are accepted
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that check_NID accepts all 7 valid DUID values.
 *
 * For each valid DUID, encode a codeword with NAC=0x123, call check_NID
 * with the correct parity, and verify NID_OK is returned.
 *
 * Validates: Requirements 4.1, 4.2
 */
static int
test_duid_validation_all_valid(void) {
    BCH_63_16_11 bch;
    int valid_duids[] = {0x0, 0x3, 0x5, 0x7, 0xA, 0xC, 0xF};
    int nac = 0x123;

    for (int idx = 0; idx < 7; idx++) {
        int duid = valid_duids[idx];
        char info[16];
        char codeword[63];

        make_info_word(nac, duid, info);
        bch.encode(info, codeword);

        int decoded_nac = -1;
        char decoded_duid[3] = {0};
        int error_count = -1;
        unsigned char parity = expected_parity(duid);

        int result = check_NID(codeword, &decoded_nac, decoded_duid, parity, &error_count);

        if (result != NID_OK) {
            std::fprintf(stderr,
                         "test_duid_validation_all_valid: DUID=0x%X expected NID_OK (1), "
                         "got %d\n",
                         duid, result);
            return 1;
        }
        if (decoded_nac != nac) {
            std::fprintf(stderr,
                         "test_duid_validation_all_valid: DUID=0x%X expected NAC=0x%X, "
                         "got 0x%X\n",
                         duid, nac, decoded_nac);
            return 1;
        }
        if (error_count != 0) {
            std::fprintf(stderr,
                         "test_duid_validation_all_valid: DUID=0x%X expected error_count=0, "
                         "got %d\n",
                         duid, error_count);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test: All 9 invalid DUIDs are rejected
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that check_NID rejects all 9 invalid DUID values.
 *
 * For each invalid DUID, encode a codeword with NAC=0x456, call check_NID
 * with parity=0, and verify NID_DECODE_FAIL is returned.
 *
 * Validates: Requirements 4.3
 */
static int
test_duid_validation_all_invalid(void) {
    BCH_63_16_11 bch;
    int invalid_duids[] = {0x1, 0x2, 0x4, 0x6, 0x8, 0x9, 0xB, 0xD, 0xE};
    int nac = 0x456;

    for (int idx = 0; idx < 9; idx++) {
        int duid = invalid_duids[idx];
        char info[16];
        char codeword[63];

        make_info_word(nac, duid, info);
        bch.encode(info, codeword);

        int decoded_nac = -1;
        char decoded_duid[3] = {0};
        int error_count = -1;

        // Use parity=0 (doesn't matter since DUID validation happens first)
        int result = check_NID(codeword, &decoded_nac, decoded_duid, 0, &error_count);

        if (result != NID_DECODE_FAIL) {
            std::fprintf(stderr,
                         "test_duid_validation_all_invalid: DUID=0x%X expected "
                         "NID_DECODE_FAIL (0), got %d\n",
                         duid, result);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test: Parity table values
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that P=1 for LDU1 and LDU2, P=0 for all other valid DUIDs.
 *
 * Encodes each valid DUID with NAC=0x789, then calls check_NID with
 * parity=1. For LDU1/LDU2 this should return NID_OK (parity matches).
 * For all others, parity=1 is wrong so it should return NID_PARITY_OVERRIDE
 * (since 0 errors ≤ 6 threshold).
 *
 * Validates: Requirements 3.3
 */
static int
test_parity_table_values(void) {
    BCH_63_16_11 bch;
    int valid_duids[] = {0x0, 0x3, 0x5, 0x7, 0xA, 0xC, 0xF};
    int nac = 0x789;

    for (int idx = 0; idx < 7; idx++) {
        int duid = valid_duids[idx];
        char info[16];
        char codeword[63];

        make_info_word(nac, duid, info);
        bch.encode(info, codeword);

        int decoded_nac = -1;
        char decoded_duid[3] = {0};
        int error_count = -1;

        // Call with parity=1 for all DUIDs
        int result = check_NID(codeword, &decoded_nac, decoded_duid, 1, &error_count);

        if (duid == 0x5 || duid == 0xA) {
            // LDU1 and LDU2 have expected parity=1, so parity=1 matches
            if (result != NID_OK) {
                std::fprintf(stderr,
                             "test_parity_table_values: DUID=0x%X (P=1) expected "
                             "NID_OK (1), got %d\n",
                             duid, result);
                return 1;
            }
        } else {
            // All others have expected parity=0, so parity=1 is a mismatch.
            // With 0 errors corrected (≤ 6 threshold), should get NID_PARITY_OVERRIDE.
            if (result != NID_PARITY_OVERRIDE) {
                std::fprintf(stderr,
                             "test_parity_table_values: DUID=0x%X (P=0) with parity=1 "
                             "expected NID_PARITY_OVERRIDE (2), got %d\n",
                             duid, result);
                return 1;
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test: Parity override threshold boundary (6 vs 7 errors)
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that exactly 6 errors with wrong parity returns
 *        NID_PARITY_OVERRIDE, and exactly 7 errors with wrong parity
 *        returns NID_PARITY_MISMATCH.
 *
 * Uses NAC=0x293, DUID=0x5 (LDU1, expected parity=1). Introduces errors
 * and calls check_NID with wrong parity (0 instead of 1).
 *
 * Validates: Requirements 7.1, 7.2
 */
static int
test_parity_override_threshold_boundary(void) {
    BCH_63_16_11 bch;

    // NAC=0x293, DUID=0x5 (LDU1) — expected parity is 1
    int nac = 0x293;
    int duid = 0x5;
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    // --- Case 1: Exactly 6 errors, wrong parity → NID_PARITY_OVERRIDE ---
    {
        char corrupted[63];
        std::memcpy(corrupted, codeword, 63);

        // Flip 6 distinct bit positions in the parity portion (positions 16+)
        // to avoid changing the info bits directly
        int flip_6[6] = {16, 22, 28, 34, 40, 46};
        for (int i = 0; i < 6; i++) {
            corrupted[flip_6[i]] ^= 1;
        }

        int decoded_nac = -1;
        char decoded_duid[3] = {0};
        int error_count = -1;

        // Wrong parity: expected is 1 for LDU1, pass 0
        int result = check_NID(corrupted, &decoded_nac, decoded_duid, 0, &error_count);

        if (result != NID_PARITY_OVERRIDE) {
            std::fprintf(stderr,
                         "test_parity_override_threshold_boundary: 6 errors + wrong parity "
                         "expected NID_PARITY_OVERRIDE (2), got %d\n",
                         result);
            return 1;
        }
        if (error_count != 6) {
            std::fprintf(stderr,
                         "test_parity_override_threshold_boundary: 6 errors expected "
                         "error_count=6, got %d\n",
                         error_count);
            return 1;
        }
        if (decoded_nac != nac) {
            std::fprintf(stderr,
                         "test_parity_override_threshold_boundary: 6 errors expected "
                         "NAC=0x%X, got 0x%X\n",
                         nac, decoded_nac);
            return 1;
        }
    }

    // --- Case 2: Exactly 7 errors, wrong parity → NID_PARITY_MISMATCH ---
    {
        char corrupted[63];
        std::memcpy(corrupted, codeword, 63);

        // Flip 7 distinct bit positions in the parity portion
        int flip_7[7] = {16, 22, 28, 34, 40, 46, 52};
        for (int i = 0; i < 7; i++) {
            corrupted[flip_7[i]] ^= 1;
        }

        int decoded_nac = -1;
        char decoded_duid[3] = {0};
        int error_count = -1;

        // Wrong parity: expected is 1 for LDU1, pass 0
        int result = check_NID(corrupted, &decoded_nac, decoded_duid, 0, &error_count);

        if (result != NID_PARITY_MISMATCH) {
            std::fprintf(stderr,
                         "test_parity_override_threshold_boundary: 7 errors + wrong parity "
                         "expected NID_PARITY_MISMATCH (-1), got %d\n",
                         result);
            return 1;
        }
        if (error_count != 7) {
            std::fprintf(stderr,
                         "test_parity_override_threshold_boundary: 7 errors expected "
                         "error_count=7, got %d\n",
                         error_count);
            return 1;
        }
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test: Decode failure returns NID_DECODE_FAIL with error_count=0
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that a heavily corrupted codeword (12+ errors) returns
 *        NID_DECODE_FAIL and sets error_count to 0.
 *
 * Validates: Requirements 3.3 (decode failure path)
 */
static int
test_decode_failure_no_error_count(void) {
    BCH_63_16_11 bch;

    // Encode a valid codeword, then corrupt 12 bits (beyond t=11)
    int nac = 0x100;
    int duid = 0x3; // TDU
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    char corrupted[63];
    std::memcpy(corrupted, codeword, 63);

    // Flip 12 distinct positions — exceeds correction capability
    int flip_12[12] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55};
    for (int i = 0; i < 12; i++) {
        corrupted[flip_12[i]] ^= 1;
    }

    int decoded_nac = -1;
    char decoded_duid[3] = {0};
    int error_count = -1;

    int result = check_NID(corrupted, &decoded_nac, decoded_duid, 0, &error_count);

    if (result != NID_DECODE_FAIL) {
        std::fprintf(stderr,
                     "test_decode_failure_no_error_count: expected NID_DECODE_FAIL (0), "
                     "got %d\n",
                     result);
        return 1;
    }
    if (error_count != 0) {
        std::fprintf(stderr,
                     "test_decode_failure_no_error_count: expected error_count=0, "
                     "got %d\n",
                     error_count);
        return 1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int
main(void) {
    int rc = 0;

    rc |= test_duid_validation_all_valid();
    rc |= test_duid_validation_all_invalid();
    rc |= test_parity_table_values();
    rc |= test_parity_override_threshold_boundary();
    rc |= test_decode_failure_no_error_count();

    if (rc == 0) {
        std::printf("P25 P1 NID full correction unit tests passed.\n");
    }

    return rc;
}
