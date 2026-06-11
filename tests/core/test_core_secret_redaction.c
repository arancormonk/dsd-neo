// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/secret_redaction.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_str(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: expected \"%s\", got \"%s\"\n", label, want, got);
        return 1;
    }
    return 0;
}

static int
test_redacted_default(void) {
    char buf[64];
    uint8_t bytes[2] = {0x12U, 0xABU};
    unsigned long long segments[2] = {0x1ULL, 0x2ULL};
    int rc = 0;
    rc |=
        expect_str("redacted decimal", dsd_secret_format_decimal(buf, sizeof buf, 0, 123ULL, 5U), DSD_SECRET_REDACTED);
    rc |= expect_str("redacted hex", dsd_secret_format_hex(buf, sizeof buf, 0, 0xABULL, 4U, 1), DSD_SECRET_REDACTED);
    rc |= expect_str("redacted segments", dsd_secret_format_u64_segments(buf, sizeof buf, 0, segments, 2U),
                     DSD_SECRET_REDACTED);
    rc |= expect_str("redacted bytes", dsd_secret_format_byte_hex(buf, sizeof buf, 0, bytes, sizeof bytes),
                     DSD_SECRET_REDACTED);
    rc |= expect_str("redacted string", dsd_secret_format_string(buf, sizeof buf, 0, "ABCDEF"), DSD_SECRET_REDACTED);
    return rc;
}

static int
test_revealed_decimal_and_hex(void) {
    char buf[64];
    int rc = 0;
    rc |= expect_str("decimal width", dsd_secret_format_decimal(buf, sizeof buf, 1, 42ULL, 5U), "00042");
    rc |= expect_str("decimal no width", dsd_secret_format_decimal(buf, sizeof buf, 1, 42ULL, 0U), "42");
    rc |= expect_str("hex width", dsd_secret_format_hex(buf, sizeof buf, 1, 0x1AULL, 4U, 0), "001A");
    rc |= expect_str("hex prefix", dsd_secret_format_hex(buf, sizeof buf, 1, 0x1AULL, 4U, 1), "0x001A");
    return rc;
}

static int
test_revealed_segments_and_bytes(void) {
    char buf[128];
    unsigned long long segments[2] = {0x1122ULL, 0xAABBULL};
    uint8_t bytes[3] = {0x12U, 0xABU, 0x00U};
    int rc = 0;
    rc |= expect_str("segments", dsd_secret_format_u64_segments(buf, sizeof buf, 1, segments, 2U),
                     "0000000000001122 000000000000AABB");
    rc |= expect_str("bytes", dsd_secret_format_byte_hex(buf, sizeof buf, 1, bytes, sizeof bytes), "12AB00");
    rc |= expect_str("string", dsd_secret_format_string(buf, sizeof buf, 1, "ABCDEF"), "ABCDEF");
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_redacted_default();
    rc |= test_revealed_decimal_and_hex();
    rc |= test_revealed_segments_and_bytes();
    return rc;
}
