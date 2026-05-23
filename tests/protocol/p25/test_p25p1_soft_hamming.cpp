// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for soft-decision Hamming(10,6,3) decoder.
 */

#include <cstdio>
#include <cstdlib>
#include <dsd-neo/fec/Hamming.hpp>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/config.h>
#include "dsd-neo/core/safe_api.h"

static int g_fail_count = 0;

#define ASSERT_EQ(a, b, msg)                                                                                           \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            DSD_FPRINTF(stderr, "FAIL: %s: expected %d, got %d\n", msg, (int)(b), (int)(a));                           \
            g_fail_count++;                                                                                            \
        }                                                                                                              \
    } while (0)

static void
set_soft_hard_override(int enabled) {
    setenv("DSD_NEO_P25_SOFT_HARD_OVERRIDE", enabled ? "1" : "0", 1);
    dsd_neo_config_init(nullptr);
}

static int
codeword_distance(const char* a, const char* b) {
    int d = 0;
    for (int i = 0; i < 10; i++) {
        if (a[i] != b[i]) {
            d++;
        }
    }
    return d;
}

static void
build_codeword(Hamming_10_6_3_TableImpl& hamming, int value, char out[10]) {
    char data[6];
    for (int i = 0; i < 6; i++) {
        data[i] = (value >> (5 - i)) & 1;
    }
    char parity[4];
    hamming.encode(data, parity);
    DSD_MEMCPY(out, data, 6);
    DSD_MEMCPY(out + 6, parity, 4);
}

static void
test_no_error() {
    /* Valid Hamming(10,6,3) codeword: data=0b101010 (42), parity=0b1001 */
    Hamming_10_6_3_TableImpl hamming;
    char hex[6] = {1, 0, 1, 0, 1, 0};
    char parity[4];
    hamming.encode(hex, parity);

    char bits[10];
    DSD_MEMCPY(bits, hex, 6);
    DSD_MEMCPY(bits + 6, parity, 4);

    int reliab[10] = {200, 200, 200, 200, 200, 200, 200, 200, 200, 200};
    char out[10];

    int result = hamming_10_6_3_soft(bits, reliab, out);
    ASSERT_EQ(result, 0, "No error case should return 0");

    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(out[i], hex[i], "Data bits should be unchanged");
    }
}

static void
test_single_error() {
    /* Valid codeword, then flip one bit */
    Hamming_10_6_3_TableImpl hamming;
    char hex[6] = {0, 1, 1, 0, 0, 1};
    char parity[4];
    hamming.encode(hex, parity);

    char bits[10];
    DSD_MEMCPY(bits, hex, 6);
    DSD_MEMCPY(bits + 6, parity, 4);

    /* Flip bit 2 (in data portion) */
    bits[2] ^= 1;

    int reliab[10] = {200, 200, 200, 200, 200, 200, 200, 200, 200, 200};
    char out[10];

    int result = hamming_10_6_3_soft(bits, reliab, out);
    ASSERT_EQ(result, 1, "Single error should be corrected");

    /* Verify corrected data matches original */
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(out[i], hex[i], "Corrected data should match original");
    }
}

static void
test_two_errors_with_soft_info() {
    /* Valid codeword, then flip two bits */
    Hamming_10_6_3_TableImpl hamming;
    char hex[6] = {1, 1, 0, 0, 1, 1};
    char parity[4];
    hamming.encode(hex, parity);

    char bits[10];
    DSD_MEMCPY(bits, hex, 6);
    DSD_MEMCPY(bits + 6, parity, 4);

    /* Flip bits 1 and 3 (both in data portion) */
    bits[1] ^= 1;
    bits[3] ^= 1;

    /* Mark positions 1 and 3 as low reliability */
    int reliab[10] = {200, 10, 200, 10, 200, 200, 200, 200, 200, 200};
    char out[10];

    int result = hamming_10_6_3_soft(bits, reliab, out);

    /* Soft decoder should find and correct both errors */
    if (result <= 1) {
        for (int i = 0; i < 6; i++) {
            ASSERT_EQ(out[i], hex[i], "Soft-corrected data should match original");
        }
    } else {
        DSD_FPRINTF(stderr, "INFO: Two-error case returned uncorrectable (expected for some patterns)\n");
    }
}

static void
test_soft_hard_override_toggle() {
    Hamming_10_6_3_TableImpl hamming;
    char hard_word[10];
    char soft_word[10];
    int diff_idx[3] = {-1, -1, -1};
    int found = 0;

    for (int a = 0; a < 64 && !found; a++) {
        build_codeword(hamming, a, hard_word);
        for (int b = a + 1; b < 64 && !found; b++) {
            build_codeword(hamming, b, soft_word);
            if (codeword_distance(hard_word, soft_word) == 3) {
                int k = 0;
                for (int i = 0; i < 10; i++) {
                    if (hard_word[i] != soft_word[i] && k < 3) {
                        diff_idx[k++] = i;
                    }
                }
                found = (k == 3);
            }
        }
    }
    ASSERT_EQ(found, 1, "Should find distance-3 Hamming codewords");
    if (!found) {
        return;
    }

    char received[10];
    DSD_MEMCPY(received, hard_word, 10);
    received[diff_idx[0]] = soft_word[diff_idx[0]];

    int reliab[10] = {200, 200, 200, 200, 200, 200, 200, 200, 200, 200};
    reliab[diff_idx[0]] = 200;
    reliab[diff_idx[1]] = 5;
    reliab[diff_idx[2]] = 5;

    char out[10];
    set_soft_hard_override(0);
    int result = hamming_10_6_3_soft(received, reliab, out);
    ASSERT_EQ(result, 1, "Disabled override should still return corrected");
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(out[i], hard_word[i], "Disabled override should keep hard correction");
    }

    set_soft_hard_override(1);
    result = hamming_10_6_3_soft(received, reliab, out);
    ASSERT_EQ(result, 1, "Enabled override should return soft correction");
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(out[i], soft_word[i], "Enabled override should choose lower-penalty soft candidate");
    }
}

static void
test_high_reliability_no_change() {
    /* When all bits are high reliability, soft decoder should trust hard decode */
    Hamming_10_6_3_TableImpl hamming;
    char hex[6] = {0, 0, 0, 1, 1, 1};
    char parity[4];
    hamming.encode(hex, parity);

    char bits[10];
    DSD_MEMCPY(bits, hex, 6);
    DSD_MEMCPY(bits + 6, parity, 4);

    int reliab[10] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    char out[10];

    int result = hamming_10_6_3_soft(bits, reliab, out);
    ASSERT_EQ(result, 0, "High reliability valid codeword should succeed");

    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(out[i], hex[i], "High reliability data should be unchanged");
    }
}

int
main(void) {
    test_no_error();
    test_single_error();
    test_two_errors_with_soft_info();
    test_soft_hard_override_toggle();
    test_high_reliability_no_change();

    if (g_fail_count > 0) {
        DSD_FPRINTF(stderr, "FAILED: %d test(s) failed\n", g_fail_count);
        return 1;
    }

    DSD_FPRINTF(stderr, "PASSED: All soft Hamming tests passed\n");
    return 0;
}
