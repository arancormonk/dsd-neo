// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for M17 packet protocol naming.
 */

#include <dsd-neo/protocol/m17/m17_parse.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint64_t
ConvertBitIntoBytes(uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0; i < n; i++) {
        v = (v << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return v;
}

static int
expect_name(const char* tag, uint8_t protocol, const char* want) {
    const char* got = m17_packet_protocol_name(protocol);
    if (want == NULL && got == NULL) {
        return 0;
    }
    if (want == NULL || got == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "%s: protocol 0x%02X got '%s' want '%s'\n", tag, (unsigned)protocol, got ? got : "(null)",
                want ? want : "(null)");
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    rc |= expect_name("raw", 0x00U, "Raw");
    rc |= expect_name("aprs", 0x02U, "APRS");
    rc |= expect_name("sms", 0x05U, "SMS");
    rc |= expect_name("tle", 0x07U, "TLE");
    rc |= expect_name("meta-text", 0x80U, "Meta Text Data");
    rc |= expect_name("unknown", 0x7FU, NULL);

    if (rc == 0) {
        printf("M17_PROTOCOL_NAMES: OK\n");
    }
    return rc;
}
