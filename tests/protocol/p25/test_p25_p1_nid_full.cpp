// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for p25p1_nid_decode() with full BCH correction,
 *        DUID validation, and parity override logic.
 *
 * This file is C++ because it uses the BCH encoder (BCH_63_16.hpp) to generate
 * valid codewords as test vectors. It includes both the C header for NID
 * decoding and the C++ BCH class.
 *
 * Validates: Requirements 3.3, 4.1, 4.2, 4.3, 7.1, 7.2
 */

#include <cstdint>
#include <cstdio>
#include <dsd-neo/fec/BCH_63_16.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include "dsd-neo/core/safe_api.h"

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
 * @brief Verify that p25p1_nid_decode accepts all 7 valid DUID values.
 *
 * For each valid DUID, encode a codeword with NAC=0x123, call p25p1_nid_decode
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

        unsigned char parity = expected_parity(duid);

        struct p25p1_nid_result result = p25p1_nid_decode(codeword, nullptr, 0, parity, 0);

        if (result.status != NID_OK) {
            DSD_FPRINTF(stderr,
                        "test_duid_validation_all_valid: DUID=0x%X expected NID_OK (1), "
                        "got %d\n",
                        duid, result.status);
            return 1;
        }
        if (result.nac != nac) {
            DSD_FPRINTF(stderr,
                        "test_duid_validation_all_valid: DUID=0x%X expected NAC=0x%X, "
                        "got 0x%X\n",
                        duid, nac, result.nac);
            return 1;
        }
        if (result.duid != duid) {
            DSD_FPRINTF(stderr, "test_duid_validation_all_valid: expected DUID=0x%X, got 0x%X\n", duid, result.duid);
            return 1;
        }
        if (result.error_count != 0) {
            DSD_FPRINTF(stderr,
                        "test_duid_validation_all_valid: DUID=0x%X expected error_count=0, "
                        "got %d\n",
                        duid, result.error_count);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test: All 9 invalid DUIDs are rejected
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that p25p1_nid_decode rejects all 9 invalid DUID values.
 *
 * For each invalid DUID, encode a codeword with NAC=0x456, call p25p1_nid_decode
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

        // Use parity=0 (doesn't matter since DUID validation happens first)
        struct p25p1_nid_result result = p25p1_nid_decode(codeword, nullptr, 0, 0, 0);

        if (result.status != NID_DECODE_FAIL) {
            DSD_FPRINTF(stderr,
                        "test_duid_validation_all_invalid: DUID=0x%X expected "
                        "NID_DECODE_FAIL (0), got %d\n",
                        duid, result.status);
            return 1;
        }
        if (result.error_count != 0) {
            DSD_FPRINTF(stderr,
                        "test_duid_validation_all_invalid: DUID=0x%X expected "
                        "error_count=0, got %d\n",
                        duid, result.error_count);
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
 * Encodes each valid DUID with NAC=0x789, then calls p25p1_nid_decode with
 * parity=1. For LDU1/LDU2 this should return NID_OK (parity matches).
 * For all others, parity=1 is wrong so it should return NID_PARITY_OVERRIDE
 * after successful BCH decode.
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

        // Call with parity=1 for all DUIDs
        struct p25p1_nid_result result = p25p1_nid_decode(codeword, nullptr, 0, 1, 0);

        if (duid == 0x5 || duid == 0xA) {
            // LDU1 and LDU2 have expected parity=1, so parity=1 matches
            if (result.status != NID_OK) {
                DSD_FPRINTF(stderr,
                            "test_parity_table_values: DUID=0x%X (P=1) expected "
                            "NID_OK (1), got %d\n",
                            duid, result.status);
                return 1;
            }
        } else {
            // All others have expected parity=0, so parity=1 is a mismatch.
            // Successful BCH decode should still report NID_PARITY_OVERRIDE.
            if (result.status != NID_PARITY_OVERRIDE) {
                DSD_FPRINTF(stderr,
                            "test_parity_table_values: DUID=0x%X (P=0) with parity=1 "
                            "expected NID_PARITY_OVERRIDE (2), got %d\n",
                            duid, result.status);
                return 1;
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test: Parity mismatch override across the BCH correction range
 * -------------------------------------------------------------------------- */

/**
 * @brief Verify that a wrong final parity bit returns NID_PARITY_OVERRIDE
 *        for BCH-correctable NIDs across the correction range.
 *
 * Uses NAC=0x293, DUID=0x5 (LDU1, expected parity=1). Introduces errors
 * and calls p25p1_nid_decode with wrong parity (0 instead of 1).
 *
 * Validates: Requirements 7.1, 7.2
 */
static int
test_parity_override_correctable_range(void) {
    BCH_63_16_11 bch;

    // NAC=0x293, DUID=0x5 (LDU1) - expected parity is 1
    int nac = 0x293;
    int duid = 0x5;
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    int counts[] = {0, 6, 7, 11};
    int flip_positions[11] = {16, 22, 28, 34, 40, 46, 52, 18, 24, 30, 36};

    for (std::size_t count_index = 0; count_index < sizeof(counts) / sizeof(counts[0]); count_index++) {
        int corrections = counts[count_index];
        char corrupted[63];
        DSD_MEMCPY(corrupted, codeword, 63);

        // Flip distinct bit positions in the BCH parity portion to avoid
        // changing the info bits directly.
        for (int i = 0; i < corrections; i++) {
            corrupted[flip_positions[i]] ^= 1;
        }

        // Wrong parity: expected is 1 for LDU1, pass 0
        struct p25p1_nid_result result = p25p1_nid_decode(corrupted, nullptr, 0, 0, 0);

        if (result.status != NID_PARITY_OVERRIDE) {
            DSD_FPRINTF(stderr,
                        "test_parity_override_correctable_range: %d errors + wrong parity "
                        "expected NID_PARITY_OVERRIDE (2), got %d\n",
                        corrections, result.status);
            return 1;
        }
        if (result.error_count != corrections) {
            DSD_FPRINTF(stderr,
                        "test_parity_override_correctable_range: %d errors expected "
                        "error_count=%d, got %d\n",
                        corrections, corrections, result.error_count);
            return 1;
        }
        if (result.nac != nac) {
            DSD_FPRINTF(stderr,
                        "test_parity_override_correctable_range: %d errors expected "
                        "NAC=0x%X, got 0x%X\n",
                        corrections, nac, result.nac);
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
    DSD_MEMCPY(corrupted, codeword, 63);

    // Flip 12 distinct positions - exceeds correction capability
    int flip_12[12] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55};
    for (int i = 0; i < 12; i++) {
        corrupted[flip_12[i]] ^= 1;
    }

    struct p25p1_nid_result result = p25p1_nid_decode(corrupted, nullptr, 0, 0, 0);

    if (result.status != NID_DECODE_FAIL) {
        DSD_FPRINTF(stderr,
                    "test_decode_failure_no_error_count: expected NID_DECODE_FAIL (0), "
                    "got %d\n",
                    result.status);
        return 1;
    }
    if (result.error_count != 0) {
        DSD_FPRINTF(stderr,
                    "test_decode_failure_no_error_count: expected error_count=0, "
                    "got %d\n",
                    result.error_count);
        return 1;
    }

    return 0;
}

/**
 * @brief Verify the sdrtrunk-style observed NAC retry after BCH failure.
 *
 * The first decode has all 12 NAC bits inverted, which exceeds the BCH
 * correction limit. Supplying the known channel NAC replaces those bits and
 * allows the second BCH decode to succeed.
 */
static int
test_observed_nac_retry_after_bch_failure(void) {
    BCH_63_16_11 bch;

    int nac = 0x293;
    int duid = 0x5; // LDU1
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    char corrupted[63];
    DSD_MEMCPY(corrupted, codeword, 63);
    for (int i = 0; i < 12; i++) {
        corrupted[i] ^= 1;
    }

    struct p25p1_nid_result result = p25p1_nid_decode(corrupted, nullptr, 0, expected_parity(duid), 0);
    if (result.status != NID_DECODE_FAIL) {
        DSD_FPRINTF(stderr, "test_observed_nac_retry_after_bch_failure: expected initial decode failure, got %d\n",
                    result.status);
        return 1;
    }

    result = p25p1_nid_decode(corrupted, nullptr, nac, expected_parity(duid), 0);
    if (result.status != NID_OK) {
        DSD_FPRINTF(stderr, "test_observed_nac_retry_after_bch_failure: expected observed NAC retry success, got %d\n",
                    result.status);
        return 1;
    }
    if (result.nac != nac) {
        DSD_FPRINTF(stderr, "test_observed_nac_retry_after_bch_failure: expected NAC=0x%X, got 0x%X\n", nac,
                    result.nac);
        return 1;
    }
    if (result.duid != (uint8_t)duid) {
        DSD_FPRINTF(stderr, "test_observed_nac_retry_after_bch_failure: expected DUID=0x%X, got 0x%X\n", duid,
                    result.duid);
        return 1;
    }
    if (result.error_count != 0) {
        DSD_FPRINTF(stderr, "test_observed_nac_retry_after_bch_failure: expected retry error_count=0, got %d\n",
                    result.error_count);
        return 1;
    }

    return 0;
}

/**
 * @brief Verify soft NID retry can use low-reliability BCH bits to recover
 *        a codeword that is just outside the hard BCH correction radius.
 */
static int
test_soft_nid_low_reliability_recovery(void) {
    BCH_63_16_11 bch;

    int nac = 0x293;
    int duid = 0x5; // LDU1
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    char corrupted[63];
    DSD_MEMCPY(corrupted, codeword, 63);
    int flip_12[12] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55};
    for (int i = 0; i < 12; i++) {
        corrupted[flip_12[i]] ^= 1;
    }

    struct p25p1_nid_result hard_result = p25p1_nid_decode(corrupted, nullptr, 0, expected_parity(duid), 0);
    if (hard_result.status != NID_DECODE_FAIL) {
        DSD_FPRINTF(stderr, "test_soft_nid_low_reliability_recovery: expected hard decode failure, got %d\n",
                    hard_result.status);
        return 1;
    }

    uint8_t reliab[63];
    for (int i = 0; i < 63; i++) {
        reliab[i] = 220;
    }
    reliab[flip_12[0]] = 10;

    struct p25p1_nid_result soft_result = p25p1_nid_decode(corrupted, reliab, 0, expected_parity(duid), 220);
    if (soft_result.status != NID_OK) {
        DSD_FPRINTF(stderr, "test_soft_nid_low_reliability_recovery: expected soft decode success, got %d\n",
                    soft_result.status);
        return 1;
    }
    if (soft_result.nac != nac || soft_result.duid != (uint8_t)duid || soft_result.error_count != 11) {
        DSD_FPRINTF(stderr,
                    "test_soft_nid_low_reliability_recovery: expected NAC=0x%X DUID=0x%X errors=11, "
                    "got NAC=0x%X DUID=0x%X errors=%d\n",
                    nac, duid, soft_result.nac, soft_result.duid, soft_result.error_count);
        return 1;
    }

    return 0;
}

/**
 * @brief Verify soft NID fallback does not flip high-confidence bits when no
 *        low-reliability candidate supports the retry.
 */
static int
test_soft_nid_high_reliability_rejected(void) {
    BCH_63_16_11 bch;

    int nac = 0x293;
    int duid = 0x5;
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    char corrupted[63];
    DSD_MEMCPY(corrupted, codeword, 63);
    int flip_12[12] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55};
    for (int i = 0; i < 12; i++) {
        corrupted[flip_12[i]] ^= 1;
    }

    uint8_t reliab[63];
    for (int i = 0; i < 63; i++) {
        reliab[i] = 220;
    }

    struct p25p1_nid_result result = p25p1_nid_decode(corrupted, reliab, 0, expected_parity(duid), 220);
    if (result.status != NID_DECODE_FAIL) {
        DSD_FPRINTF(stderr, "test_soft_nid_high_reliability_rejected: expected soft decode failure, got %d\n",
                    result.status);
        return 1;
    }

    return 0;
}

/**
 * @brief Verify observed-NAC soft retry scores only Chase flips, not the trusted
 *        NAC rewrite itself.
 */
static int
test_soft_observed_nac_retry_exempts_forced_nac_bits(void) {
    BCH_63_16_11 bch;

    int nac = 0x293;
    int duid = 0x5;
    char info[16];
    char codeword[63];

    make_info_word(nac, duid, info);
    bch.encode(info, codeword);

    char corrupted[63];
    DSD_MEMCPY(corrupted, codeword, 63);
    corrupted[0] ^= 1; // High-confidence NAC bit corrected by observed_nac.

    int parity_errors[12] = {16, 22, 28, 34, 40, 46, 52, 18, 24, 30, 36, 42};
    for (int i = 0; i < 12; i++) {
        corrupted[parity_errors[i]] ^= 1;
    }

    struct p25p1_nid_result hard_result = p25p1_nid_decode(corrupted, nullptr, nac, expected_parity(duid), 0);
    if (hard_result.status != NID_DECODE_FAIL) {
        DSD_FPRINTF(stderr,
                    "test_soft_observed_nac_retry_exempts_forced_nac_bits: expected hard retry failure, got %d\n",
                    hard_result.status);
        return 1;
    }

    uint8_t reliab[63];
    for (int i = 0; i < 63; i++) {
        reliab[i] = 220;
    }
    reliab[parity_errors[0]] = 10;

    struct p25p1_nid_result soft_result = p25p1_nid_decode(corrupted, reliab, nac, expected_parity(duid), 220);
    if (soft_result.status != NID_OK) {
        DSD_FPRINTF(stderr,
                    "test_soft_observed_nac_retry_exempts_forced_nac_bits: expected soft retry success, got %d\n",
                    soft_result.status);
        return 1;
    }
    if (soft_result.nac != nac || soft_result.duid != (uint8_t)duid || soft_result.error_count != 11) {
        DSD_FPRINTF(stderr,
                    "test_soft_observed_nac_retry_exempts_forced_nac_bits: expected NAC=0x%X DUID=0x%X errors=11, "
                    "got NAC=0x%X DUID=0x%X errors=%d\n",
                    nac, duid, soft_result.nac, soft_result.duid, soft_result.error_count);
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
    rc |= test_parity_override_correctable_range();
    rc |= test_decode_failure_no_error_count();
    rc |= test_observed_nac_retry_after_bch_failure();
    rc |= test_soft_nid_low_reliability_recovery();
    rc |= test_soft_nid_high_reliability_rejected();
    rc |= test_soft_observed_nac_retry_exempts_forced_nac_bits();

    if (rc == 0) {
        std::printf("P25 P1 NID full correction unit tests passed.\n");
    }

    return rc;
}
