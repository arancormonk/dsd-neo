// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <cstring>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

static void
set_symbol(char* symbols, int index, int value) {
    for (int bit = 0; bit < 6; bit++) {
        symbols[(index * 6) + bit] = (char)((value >> (5 - bit)) & 1);
    }
}

static int
get_symbol(const char* symbols, int index) {
    int value = 0;
    for (int bit = 0; bit < 6; bit++) {
        value = (value << 1) | (symbols[(index * 6) + bit] & 1);
    }
    return value;
}

static void
fill_data(char* symbols, int count, int seed) {
    for (int i = 0; i < count; i++) {
        set_symbol(symbols, i, ((seed + (i * 7)) & 0x3F));
    }
}

static void
corrupt_symbol(char* symbols, int index, int mask) {
    set_symbol(symbols, index, get_symbol(symbols, index) ^ (mask & 0x3F));
}

static int
expect_eq_int(const char* name, int got, int expected) {
    if (got != expected) {
        DSD_FPRINTF(stderr, "%s: expected %d, got %d\n", name, expected, got);
        return 1;
    }
    return 0;
}

static int
test_erasure_mapping(void) {
    uint8_t data_reliab[20];
    uint8_t parity_reliab[16];
    int erasures[16];

    DSD_MEMSET(data_reliab, 255, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 255, sizeof(parity_reliab));
    parity_reliab[3] = 10;
    data_reliab[4] = 20;

    int n = p25p1_build_rs_erasures(data_reliab, 20, parity_reliab, 16, erasures, 16);
    int rc = 0;
    rc |= expect_eq_int("hdu erasure count", n, 2);
    rc |= expect_eq_int("hdu parity position", erasures[0], 3);
    rc |= expect_eq_int("hdu data position", erasures[1], 20);

    DSD_MEMSET(data_reliab, 255, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 255, sizeof(parity_reliab));
    parity_reliab[7] = 8;
    data_reliab[9] = 9;
    n = p25p1_build_rs_erasures(data_reliab, 16, parity_reliab, 8, erasures, 8);
    rc |= expect_eq_int("ldu2 erasure count", n, 2);
    rc |= expect_eq_int("ldu2 parity position", erasures[0], 7);
    rc |= expect_eq_int("ldu2 data position", erasures[1], 17);

    DSD_MEMSET(data_reliab, 255, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 255, sizeof(parity_reliab));
    n = p25p1_build_rs_erasures(data_reliab, 12, parity_reliab, 12, erasures, 12);
    rc |= expect_eq_int("high confidence erasure count", n, 0);
    return rc;
}

static int
test_hdu_soft_rs(void) {
    char data[20 * 6];
    char parity[16 * 6];
    char expected[20 * 6];
    int erasures[10];

    fill_data(data, 20, 3);
    encode_reedsolomon_36_20_17(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    for (int i = 0; i < 10; i++) {
        corrupt_symbol(data, i, 0x21 + i);
        erasures[i] = 16 + i;
    }

    char hard_data[20 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_redsolomon_36_20_17(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "hdu hard RS unexpectedly corrected 10 symbol errors\n");
        return 1;
    }
    if (check_and_fix_redsolomon_36_20_17_soft(data, parity, erasures, 10) != 0) {
        DSD_FPRINTF(stderr, "hdu soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "hdu soft RS data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_hdu_soft_rs_mixed_errors_and_erasures(void) {
    char data[20 * 6];
    char parity[16 * 6];
    char expected[20 * 6];
    int erasures[10];

    fill_data(data, 20, 29);
    encode_reedsolomon_36_20_17(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    corrupt_symbol(data, 0, 0x07);
    corrupt_symbol(data, 1, 0x19);
    for (int i = 0; i < 10; i++) {
        corrupt_symbol(data, i + 2, 0x21 + i);
        erasures[i] = 16 + i + 2;
    }

    char hard_data[20 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_redsolomon_36_20_17(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "hdu hard RS unexpectedly corrected mixed 12 symbol errors\n");
        return 1;
    }
    if (check_and_fix_redsolomon_36_20_17_soft(data, parity, erasures, 10) != 0) {
        DSD_FPRINTF(stderr, "hdu mixed errors+erasures soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "hdu mixed errors+erasures data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_hdu_golay24_6_wrappers(void) {
    char hex[6];
    char parity[12];
    char expected[6];

    set_symbol(hex, 0, 0x2A);
    DSD_MEMCPY(expected, hex, sizeof(hex));
    encode_golay_24_6(hex, parity);

    char data_fix[6];
    char parity_fix[12];
    DSD_MEMCPY(data_fix, hex, sizeof(data_fix));
    DSD_MEMCPY(parity_fix, parity, sizeof(parity_fix));
    data_fix[0] ^= 1;
    data_fix[5] ^= 1;
    parity_fix[3] ^= 1;

    int fixed = 0;
    if (check_and_fix_golay_24_6(data_fix, parity_fix, &fixed) != 0) {
        DSD_FPRINTF(stderr, "hdu Golay(24,6) failed to correct three errors\n");
        return 1;
    }
    if (std::memcmp(data_fix, expected, sizeof(data_fix)) != 0 || fixed <= 0) {
        DSD_FPRINTF(stderr, "hdu Golay(24,6) corrected data mismatch fixed=%d\n", fixed);
        return 1;
    }

    DSD_MEMCPY(data_fix, hex, sizeof(data_fix));
    DSD_MEMCPY(parity_fix, parity, sizeof(parity_fix));
    data_fix[0] ^= 1;
    data_fix[1] ^= 1;
    data_fix[2] ^= 1;
    parity_fix[0] ^= 1;
    fixed = 0;
    if (check_and_fix_golay_24_6(data_fix, parity_fix, &fixed) == 0) {
        DSD_FPRINTF(stderr, "hdu Golay(24,6) unexpectedly corrected four errors\n");
        return 1;
    }
    return 0;
}

static int
test_hdu_ranked_reliability_above_threshold(void) {
    char data[20 * 6];
    char parity[16 * 6];
    char expected[20 * 6];
    uint8_t data_reliab[20];
    uint8_t parity_reliab[16];

    fill_data(data, 20, 47);
    encode_reedsolomon_36_20_17(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    DSD_MEMSET(data_reliab, 200, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 200, sizeof(parity_reliab));
    for (int i = 0; i < 10; i++) {
        corrupt_symbol(data, i, 0x11 + i);
        data_reliab[i] = 100;
    }

    if (p25p1_rs_36_20_17_soft_reliability(NULL, parity, data_reliab, parity_reliab) != 1
        || p25p1_rs_36_20_17_soft_reliability(data, NULL, data_reliab, parity_reliab) != 1
        || p25p1_rs_36_20_17_soft_reliability(data, parity, NULL, parity_reliab) != 1
        || p25p1_rs_36_20_17_soft_reliability(data, parity, data_reliab, NULL) != 1) {
        DSD_FPRINTF(stderr, "hdu ranked reliability NULL guard failed\n");
        return 1;
    }

    char hard_data[20 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_redsolomon_36_20_17(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "hdu hard RS unexpectedly corrected 10 weak high-confidence errors\n");
        return 1;
    }
    if (p25p1_rs_36_20_17_soft_reliability(data, parity, data_reliab, parity_reliab) != 0) {
        DSD_FPRINTF(stderr, "hdu ranked reliability soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "hdu ranked reliability data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu1_soft_rs(void) {
    char data[12 * 6];
    char parity[12 * 6];
    char expected[12 * 6];
    int erasures[7];

    fill_data(data, 12, 11);
    encode_reedsolomon_24_12_13(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    for (int i = 0; i < 7; i++) {
        corrupt_symbol(data, i, 0x13 + i);
        erasures[i] = 12 + i;
    }

    char hard_data[12 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_reedsolomon_24_12_13(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "ldu1 hard RS unexpectedly corrected 7 symbol errors\n");
        return 1;
    }
    if (check_and_fix_reedsolomon_24_12_13_soft(data, parity, erasures, 7) != 0) {
        DSD_FPRINTF(stderr, "ldu1 soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "ldu1 soft RS data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu1_soft_rs_mixed_errors_and_erasures(void) {
    char data[12 * 6];
    char parity[12 * 6];
    char expected[12 * 6];
    int erasures[7];

    fill_data(data, 12, 41);
    encode_reedsolomon_24_12_13(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    corrupt_symbol(data, 0, 0x05);
    corrupt_symbol(data, 1, 0x26);
    for (int i = 0; i < 7; i++) {
        corrupt_symbol(data, i + 2, 0x11 + i);
        erasures[i] = 12 + i + 2;
    }

    char hard_data[12 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_reedsolomon_24_12_13(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "ldu1 hard RS unexpectedly corrected mixed 9 symbol errors\n");
        return 1;
    }
    if (check_and_fix_reedsolomon_24_12_13_soft(data, parity, erasures, 7) != 0) {
        DSD_FPRINTF(stderr, "ldu1 mixed errors+erasures soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "ldu1 mixed errors+erasures data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu1_ranked_reliability_above_threshold(void) {
    char data[12 * 6];
    char parity[12 * 6];
    char expected[12 * 6];
    uint8_t data_reliab[12];
    uint8_t parity_reliab[12];

    fill_data(data, 12, 37);
    encode_reedsolomon_24_12_13(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    DSD_MEMSET(data_reliab, 200, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 200, sizeof(parity_reliab));
    for (int i = 0; i < 7; i++) {
        corrupt_symbol(data, i, 0x17 + i);
        data_reliab[i] = 100;
    }

    if (p25p1_rs_24_12_13_soft_reliability(NULL, parity, data_reliab, parity_reliab) != 1
        || p25p1_rs_24_12_13_soft_reliability(data, NULL, data_reliab, parity_reliab) != 1
        || p25p1_rs_24_12_13_soft_reliability(data, parity, NULL, parity_reliab) != 1
        || p25p1_rs_24_12_13_soft_reliability(data, parity, data_reliab, NULL) != 1) {
        DSD_FPRINTF(stderr, "ldu1 ranked reliability NULL guard failed\n");
        return 1;
    }

    char hard_data[12 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_reedsolomon_24_12_13(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "ldu1 hard RS unexpectedly corrected 7 weak high-confidence errors\n");
        return 1;
    }
    if (p25p1_rs_24_12_13_soft_reliability(data, parity, data_reliab, parity_reliab) != 0) {
        DSD_FPRINTF(stderr, "ldu1 ranked reliability soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "ldu1 ranked reliability data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu2_soft_rs(void) {
    char data[16 * 6];
    char parity[8 * 6];
    char expected[16 * 6];
    int erasures[5];

    fill_data(data, 16, 23);
    encode_reedsolomon_24_16_9(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    for (int i = 0; i < 5; i++) {
        corrupt_symbol(data, i, 0x0D + i);
        erasures[i] = 8 + i;
    }

    char hard_data[16 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_reedsolomon_24_16_9(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "ldu2 hard RS unexpectedly corrected 5 symbol errors\n");
        return 1;
    }
    if (check_and_fix_reedsolomon_24_16_9_soft(data, parity, erasures, 5) != 0) {
        DSD_FPRINTF(stderr, "ldu2 soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "ldu2 soft RS data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu2_soft_rs_mixed_errors_and_erasures(void) {
    char data[16 * 6];
    char parity[8 * 6];
    char expected[16 * 6];
    int erasures[5];

    fill_data(data, 16, 53);
    encode_reedsolomon_24_16_9(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    corrupt_symbol(data, 0, 0x3A);
    for (int i = 0; i < 5; i++) {
        corrupt_symbol(data, i + 1, 0x09 + i);
        erasures[i] = 8 + i + 1;
    }

    char hard_data[16 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_reedsolomon_24_16_9(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "ldu2 hard RS unexpectedly corrected mixed 6 symbol errors\n");
        return 1;
    }
    if (check_and_fix_reedsolomon_24_16_9_soft(data, parity, erasures, 5) != 0) {
        DSD_FPRINTF(stderr, "ldu2 mixed errors+erasures soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "ldu2 mixed errors+erasures data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu2_ranked_reliability_above_threshold(void) {
    char data[16 * 6];
    char parity[8 * 6];
    char expected[16 * 6];
    uint8_t data_reliab[16];
    uint8_t parity_reliab[8];

    fill_data(data, 16, 61);
    encode_reedsolomon_24_16_9(data, parity);
    DSD_MEMCPY(expected, data, sizeof(data));

    DSD_MEMSET(data_reliab, 200, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 200, sizeof(parity_reliab));
    for (int i = 0; i < 5; i++) {
        corrupt_symbol(data, i, 0x15 + i);
        data_reliab[i] = 100;
    }

    char hard_data[16 * 6];
    DSD_MEMCPY(hard_data, data, sizeof(data));
    if (check_and_fix_reedsolomon_24_16_9(hard_data, parity) == 0) {
        DSD_FPRINTF(stderr, "ldu2 hard RS unexpectedly corrected 5 weak high-confidence errors\n");
        return 1;
    }
    if (p25p1_rs_24_16_9_soft_reliability(data, parity, data_reliab, parity_reliab) != 0) {
        DSD_FPRINTF(stderr, "ldu2 ranked reliability soft RS failed\n");
        return 1;
    }
    if (std::memcmp(data, expected, sizeof(data)) != 0) {
        DSD_FPRINTF(stderr, "ldu2 ranked reliability data mismatch\n");
        return 1;
    }
    return 0;
}

static int
test_ldu2_ranked_reliability_null_guards(void) {
    char data[16 * 6];
    char parity[8 * 6];
    uint8_t data_reliab[16];
    uint8_t parity_reliab[8];

    fill_data(data, 16, 17);
    encode_reedsolomon_24_16_9(data, parity);

    DSD_MEMSET(data_reliab, 200, sizeof(data_reliab));
    DSD_MEMSET(parity_reliab, 200, sizeof(parity_reliab));

    if (p25p1_rs_24_16_9_soft_reliability(NULL, parity, data_reliab, parity_reliab) != 1
        || p25p1_rs_24_16_9_soft_reliability(data, NULL, data_reliab, parity_reliab) != 1
        || p25p1_rs_24_16_9_soft_reliability(data, parity, NULL, parity_reliab) != 1
        || p25p1_rs_24_16_9_soft_reliability(data, parity, data_reliab, NULL) != 1) {
        DSD_FPRINTF(stderr, "ldu2 ranked reliability NULL guard failed\n");
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_erasure_mapping();
    rc |= test_hdu_soft_rs();
    rc |= test_hdu_soft_rs_mixed_errors_and_erasures();
    rc |= test_hdu_golay24_6_wrappers();
    rc |= test_hdu_ranked_reliability_above_threshold();
    rc |= test_ldu1_soft_rs();
    rc |= test_ldu1_soft_rs_mixed_errors_and_erasures();
    rc |= test_ldu1_ranked_reliability_above_threshold();
    rc |= test_ldu2_soft_rs();
    rc |= test_ldu2_soft_rs_mixed_errors_and_erasures();
    rc |= test_ldu2_ranked_reliability_above_threshold();
    rc |= test_ldu2_ranked_reliability_null_guards();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASSED: P25P1 soft RS tests passed\n");
    }
    return rc;
}
