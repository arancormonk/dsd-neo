// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/nonce.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
all_bytes_equal(const unsigned char* bytes, size_t size, unsigned char value) {
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != value) {
            return 0;
        }
    }
    return 1;
}

static int
test_noop_inputs(void) {
    int rc = 0;
    unsigned char bytes[8];
    DSD_MEMSET(bytes, 0xA5, sizeof bytes);

    dsd_nonce_fill(NULL, 16);
    dsd_nonce_fill(bytes, 0);

    rc |= expect_int("zero-size fill preserves buffer", all_bytes_equal(bytes, sizeof bytes, 0xA5), 1);
    return rc;
}

static int
test_bounded_fill(void) {
    int rc = 0;
    unsigned char bytes[24];
    DSD_MEMSET(bytes, 0xA5, sizeof bytes);

    dsd_nonce_fill(bytes + 4, 16);

    rc |= expect_int("prefix sentinel preserved", all_bytes_equal(bytes, 4, 0xA5), 1);
    rc |= expect_int("suffix sentinel preserved", all_bytes_equal(bytes + 20, 4, 0xA5), 1);
    rc |= expect_int("payload bytes written", all_bytes_equal(bytes + 4, 16, 0xA5), 0);
    return rc;
}

static int
test_multiblock_fill(void) {
    int rc = 0;
    unsigned char bytes[33];
    DSD_MEMSET(bytes, 0, sizeof bytes);

    dsd_nonce_fill(bytes, sizeof bytes);

    rc |= expect_int("multi-block payload written", all_bytes_equal(bytes, sizeof bytes, 0), 0);
    rc |= expect_int("separate generated blocks differ", memcmp(bytes, bytes + 8, 8) == 0, 0);
    return rc;
}

static int
test_u16_wrapper(void) {
    uint16_t first = dsd_nonce_u16();
    uint16_t samples[8];
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        samples[i] = dsd_nonce_u16();
        if (samples[i] != first) {
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "FAIL: dsd_nonce_u16 returned the same value for every sample: %u\n", (unsigned)first);
    return 1;
}

int
main(void) {
    int rc = 0;
    rc |= test_noop_inputs();
    rc |= test_bounded_fill();
    rc |= test_multiblock_fill();
    rc |= test_u16_wrapper();
    return rc ? 1 : 0;
}
