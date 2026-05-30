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
#include "dsd-neo/core/safe_api.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

uint64_t
ConvertBitIntoBytes(const uint8_t* bits, uint32_t n) { // NOLINT(misc-use-internal-linkage)
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
        DSD_FPRINTF(stderr, "%s: protocol 0x%02X got '%s' want '%s'\n", tag, (unsigned)protocol, got ? got : "(null)",
                    want ? want : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_segment(const char* tag, uint8_t protocol, uint8_t control, uint8_t want_segment, uint8_t want_len) {
    uint8_t got_segment = 0U;
    uint8_t got_len = 0U;
    int rc = m17_meta_text_segment_info(protocol, control, &got_segment, &got_len);
    if (rc != 0 || got_segment != want_segment || got_len != want_len) {
        DSD_FPRINTF(stderr, "%s: rc=%d got %u/%u want %u/%u\n", tag, rc, (unsigned)got_segment, (unsigned)got_len,
                    (unsigned)want_segment, (unsigned)want_len);
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
    rc |= expect_name("otakd", 0x69U, "OTA Key Delivery");
    rc |= expect_name("meta-text", 0x80U, "Meta Text Data V2");
    rc |= expect_name("meta-text-v3", 0x83U, "Meta Text Data V3");
    rc |= expect_name("pdu-gnss", 0x91U, "PDU GNSS Position Data");
    rc |= expect_name("arb-data-fme", 0x99U, "1600 Arbitrary Data");
    rc |= expect_name("unknown", 0x7FU, NULL);

    rc |= expect_segment("meta-v2-first-of-two", 0x80U, 0x31U, 1U, 2U);
    rc |= expect_segment("meta-v2-fourth-of-four", 0x80U, 0xF8U, 4U, 4U);
    rc |= expect_segment("meta-v3-nibble", 0x83U, 0xA5U, 5U, 10U);
    rc |= (m17_meta_text_segment_info(0x99U, 0x11U, NULL, NULL) != -1);

    uint8_t unused_segment = 0U;
    uint8_t unused_len = 0U;
    rc |= (m17_meta_text_segment_info(0x99U, 0x11U, &unused_segment, &unused_len) != -2);

    if (rc == 0) {
        printf("M17_PROTOCOL_NAMES: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
