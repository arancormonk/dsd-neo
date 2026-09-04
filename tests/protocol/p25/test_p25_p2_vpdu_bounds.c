// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression tests for P25 phase 2 VPDU octet-range handling.
 *
 * Each case is a MAC PDU that read past a stack buffer before the fix. All three were
 * found by FUZZ_PROTOCOL_P25P2_MAC once it started driving the real process_MAC_VPDU()
 * path rather than only the pure parser, and each is preserved as a corpus seed under
 * tests/fuzz/corpus/protocol_p25p2_mac/. They assert nothing beyond "does not read out
 * of bounds": run under asan-ubsan-debug, where the pre-fix code trips ASan.
 */

#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "p25_test_shim.h"

/*
 * MFID A4 (Harris) with a length octet that walks the dump loops past MAC[24].
 * The loops were `for (i = 4; i <= len; i++) MAC[i + len_a]` with both len and len_a
 * taken from the wire, so they read - and printed - adjacent stack memory.
 */
static void
test_harris_mfid_a4_payload_dump(void) {
    const unsigned char mac[24] = {0x00, 0x90, 0x91, 0x01, 0x09, 0x2D, 0x00, 0x4B, 0x6D, 0xA4, 0xA4, 0xA4,
                                   0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4};
    p25_test_process_mac_vpdu_ex(0, mac, (int)sizeof(mac), 0, 0);
}

/*
 * A parsed segment offset at or beyond the end of the MAC PDU. Handlers address fields
 * as MAC[N + len_a], so an offset near the end pushed fixed field reads past the array.
 */
static void
test_segment_offset_past_mac(void) {
    const unsigned char mac[24] = {0x00, 0x31, 0x91, 0x90, 0x47, 0x44, 0x01, 0x41, 0x42, 0x43, 0x44, 0x45,
                                   0xEF, 0xEF, 0xEF, 0xEF, 0xEF, 0xEF, 0x2F, 0x09, 0x49, 0x01, 0xA9, 0xA9};
    p25_test_process_mac_vpdu_ex(1, mac, 20, 0, 1);
}

/*
 * Harris talker alias (opcode 0xA8). l3h_embedded_alias_decode() takes the index of the
 * last readable octet and loops `i <= len`, but the caller passed a count clamped to 24,
 * reading bytes[24] of a 24-element buffer.
 */
static void
test_harris_alias_length_off_by_one(void) {
    const unsigned char mac[24] = {0x00, 0xA8, 0xA4, 0xA4, 0xFB, 0x66, 0x15, 0xA4, 0xFA, 0x12, 0x00, 0x00,
                                   0x62, 0xFF, 0x53, 0xE7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    p25_test_process_mac_vpdu_ex(0, mac, (int)sizeof(mac), 0, 0);
}

/* Sweep every segment offset and length so a future handler cannot reintroduce the class. */
static void
test_offset_and_length_sweep(void) {
    for (int opcode = 0; opcode < 256; opcode += 17) {
        for (int mac_len = 0; mac_len <= 24; mac_len += 6) {
            unsigned char mac[24];
            for (size_t i = 0; i < sizeof(mac); i++) {
                mac[i] = (unsigned char)(0xA4U + i);
            }
            mac[1] = (unsigned char)opcode;
            mac[2] = 0x90; /* Motorola */
            mac[3] = 0xFF; /* maximal unvalidated length octet */
            p25_test_process_mac_vpdu_ex(0, mac, mac_len, 0, 0);
            mac[2] = 0xA4; /* Harris */
            p25_test_process_mac_vpdu_ex(1, mac, mac_len, 1, 1);
        }
    }
}

int
main(void) {
    test_harris_mfid_a4_payload_dump();
    test_segment_offset_past_mac();
    test_harris_alias_length_off_by_one();
    test_offset_and_length_sweep();

    DSD_FPRINTF(stderr, "P25_P2_VPDU_BOUNDS: PASS\n");
    return 0;
}
