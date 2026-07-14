// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/des.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* label, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: expected 0x%02X, got 0x%02X\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
test_des_ofb_known_vector(void) {
    static const uint8_t expect[] = {0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05,
                                     0x67, 0xAE, 0x7A, 0x29, 0x61, 0xDF, 0xA3, 0x45};
    uint8_t output[sizeof(expect)];
    DSD_MEMSET(output, 0, sizeof(output));

    des_ofb_keystream_output(0x0123456789ABCDEFULL, 0x133457799BBCDFF1ULL, output, 2);
    return expect_bytes("des ofb vector", output, expect, sizeof(expect));
}

static int
test_des_xl_fast_forward_offsets(void) {
    static const uint8_t sync_expect[] = {0x77, 0x47, 0x26, 0xF8, 0xF9, 0x51, 0x86, 0xF4, 0x43, 0xB4, 0xC9, 0x57,
                                          0xDE, 0xEC, 0x35, 0xC6, 0xEB, 0xD8, 0x56, 0xEA, 0x68, 0xEC, 0x47, 0x60};
    static const uint8_t late_expect[] = {0x55, 0x13, 0x34, 0xE2, 0xBA, 0xE1, 0x31, 0x90, 0x0A, 0x87, 0x24, 0x80,
                                          0x2F, 0xEB, 0x40, 0x2E, 0x8B, 0xD9, 0x6B, 0x36, 0x99, 0x15, 0x9A, 0x55};
    uint8_t sync_output[216];
    uint8_t late_output[216];
    DSD_MEMSET(sync_output, 0, sizeof(sync_output));
    DSD_MEMSET(late_output, 0, sizeof(late_output));
    sync_output[213] = 0xA5;
    late_output[213] = 0x5A;

    des_xl_keystream_output(0x0123456789ABCDEFULL, 0x133457799BBCDFF1ULL, sync_output, 0);
    des_xl_keystream_output(0x0123456789ABCDEFULL, 0x133457799BBCDFF1ULL, late_output, 1);

    int rc = 0;
    rc |= expect_bytes("des-xl sync offset", sync_output, sync_expect, sizeof(sync_expect));
    rc |= expect_bytes("des-xl late offset", late_output, late_expect, sizeof(late_expect));
    rc |= expect_u8("des-xl sync length", sync_output[213], 0xA5);
    rc |= expect_u8("des-xl late length", late_output[213], 0x5A);
    return rc;
}

static int
test_tdea_tofb_known_vector(void) {
    static const uint8_t key[24] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x23, 0x45, 0x67, 0x89,
                                    0xAB, 0xCD, 0xEF, 0x01, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23};
    static const uint8_t expect[] = {0xF2, 0xAF, 0xD8, 0x4E, 0xE8, 0x09, 0xE2, 0xB5,
                                     0x5E, 0x96, 0x2F, 0x92, 0x23, 0x78, 0x89, 0x5D};
    uint8_t output[sizeof(expect)];
    DSD_MEMSET(output, 0, sizeof(output));

    tdea_tofb_keystream_output(0x0123456789ABCDEFULL, key, output, 2);

    return expect_bytes("tdea tofb vector", output, expect, sizeof(expect));
}

int
main(void) {
    int rc = 0;
    rc |= test_des_ofb_known_vector();
    rc |= test_des_xl_fast_forward_offsets();
    rc |= test_tdea_tofb_known_vector();
    return rc;
}
