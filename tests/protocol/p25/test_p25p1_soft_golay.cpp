// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for soft-decision Golay(24,6) and Golay(24,12) decoders.
 */

#include <cstdio>
#include <dsd-neo/fec/Golay24.hpp>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include "dsd-neo/core/safe_api.h"

static int g_fail_count = 0;

#define ASSERT_EQ(a, b, msg)                                                                                           \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            DSD_FPRINTF(stderr, "FAIL: %s: expected %d, got %d\n", msg, (int)(b), (int)(a));                           \
            g_fail_count++;                                                                                            \
        }                                                                                                              \
    } while (0)

/* ---- Golay(24,6) tests ---- */

static void
test_golay_6_no_error() {
    DSDGolay24 golay;
    char data[6] = {1, 0, 1, 1, 0, 1};
    char parity[12];
    golay.encode_6(data, parity);

    int reliab[18];
    for (int i = 0; i < 18; i++) {
        reliab[i] = 200;
    }

    int fixed = 0;
    int result = check_and_fix_golay_24_6_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "No error case should succeed");
    ASSERT_EQ(fixed, 0, "No errors should be fixed");
}

static void
test_golay_6_single_error() {
    DSDGolay24 golay;
    char orig_data[6] = {0, 1, 0, 1, 1, 0};
    char data[6];
    char parity[12];

    DSD_MEMCPY(data, orig_data, 6);
    golay.encode_6(data, parity);

    /* Flip one data bit */
    data[2] ^= 1;

    int reliab[18];
    for (int i = 0; i < 18; i++) {
        reliab[i] = 200;
    }

    int fixed = 0;
    int result = check_and_fix_golay_24_6_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "Single error should be corrected");

    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(data[i], orig_data[i], "Corrected data should match original");
    }
}

static void
test_golay_6_two_errors() {
    DSDGolay24 golay;
    char orig_data[6] = {1, 1, 0, 0, 1, 1};
    char data[6] = {0};
    char parity[12] = {0};

    DSD_MEMCPY(data, orig_data, 6);
    golay.encode_6(data, parity);

    /* Flip two bits (well within Golay correction capability) */
    data[0] ^= 1;
    parity[5] ^= 1;

    int reliab[18];
    for (int i = 0; i < 18; i++) {
        reliab[i] = 200;
    }

    int fixed = 0;
    int result = check_and_fix_golay_24_6_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "Two errors should be correctable");

    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(data[i], orig_data[i], "Corrected data should match original");
    }
}

/* ---- Golay(24,12) tests ---- */

static void
test_golay_12_no_error() {
    DSDGolay24 golay;
    char data[12] = {1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0};
    char parity[12];
    golay.encode_12(data, parity);

    int reliab[24];
    for (int i = 0; i < 24; i++) {
        reliab[i] = 200;
    }

    int fixed = 0;
    int result = check_and_fix_golay_24_12_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "No error case should succeed");
    ASSERT_EQ(fixed, 0, "No errors should be fixed");
}

static void
test_golay_12_two_errors() {
    DSDGolay24 golay;
    char orig_data[12] = {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
    char data[12] = {0};
    char parity[12] = {0};

    DSD_MEMCPY(data, orig_data, 12);
    golay.encode_12(data, parity);

    /* Flip two bits */
    data[4] ^= 1;
    parity[8] ^= 1;

    int reliab[24];
    for (int i = 0; i < 24; i++) {
        reliab[i] = 200;
    }

    int fixed = 0;
    int result = check_and_fix_golay_24_12_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "Two errors should be corrected");

    for (int i = 0; i < 12; i++) {
        ASSERT_EQ(data[i], orig_data[i], "Corrected data should match original");
    }
}

static void
test_golay_12_three_errors() {
    /* Test 3 errors - within Golay(24,12) hard decode capability */
    DSDGolay24 golay;
    char orig_data[12] = {1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0};
    char data[12] = {0};
    char parity[12] = {0};

    DSD_MEMCPY(data, orig_data, 12);
    golay.encode_12(data, parity);

    /* Flip three bits */
    data[1] ^= 1;
    data[5] ^= 1;
    parity[2] ^= 1;

    int reliab[24];
    for (int i = 0; i < 24; i++) {
        reliab[i] = 200;
    }

    int fixed = 0;
    int result = check_and_fix_golay_24_12_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "Three errors should be correctable");

    for (int i = 0; i < 12; i++) {
        ASSERT_EQ(data[i], orig_data[i], "Corrected data should match original");
    }
}

static void
test_golay_12_prefers_low_reliability_flips() {
    DSDGolay24 golay;
    char orig_data[12] = {0};
    char data[12];
    char parity[12];

    DSD_MEMCPY(data, orig_data, 12);
    golay.encode_12(data, parity);

    /* Four errors force the soft path. With the old inverted penalty this
     * vector selected a different valid codeword by preferring high-confidence
     * flips.
     */
    data[0] ^= 1;
    data[1] ^= 1;
    data[2] ^= 1;
    data[3] ^= 1;

    int reliab[24];
    for (int i = 0; i < 24; i++) {
        reliab[i] = 180;
    }
    reliab[0] = 20;
    reliab[1] = 20;
    reliab[2] = 20;
    reliab[3] = 20;

    int fixed = 0;
    int result = check_and_fix_golay_24_12_soft(data, parity, reliab, &fixed);
    ASSERT_EQ(result, 0, "Low-reliability four-error case should soft-correct");

    for (int i = 0; i < 12; i++) {
        ASSERT_EQ(data[i], orig_data[i], "Soft Golay should prefer low-reliability flips");
    }
}

int
main(void) {
    /* Golay(24,6) tests */
    test_golay_6_no_error();
    test_golay_6_single_error();
    test_golay_6_two_errors();

    /* Golay(24,12) tests */
    test_golay_12_no_error();
    test_golay_12_two_errors();
    test_golay_12_three_errors();
    test_golay_12_prefers_low_reliability_flips();

    if (g_fail_count > 0) {
        DSD_FPRINTF(stderr, "FAILED: %d test(s) failed\n", g_fail_count);
        return 1;
    }

    DSD_FPRINTF(stderr, "PASSED: All soft Golay tests passed\n");
    return 0;
}
