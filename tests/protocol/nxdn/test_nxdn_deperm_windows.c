// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression checks for NXDN decoded-bit CRC windows and SACCH sequencing.
 */

#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static void
write_bits_u16(uint8_t* bits, size_t start, uint16_t value, size_t nbits) {
    for (size_t i = 0U; i < nbits; i++) {
        const size_t shift = nbits - 1U - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static int
expect_u16(const char* tag, uint16_t got, uint16_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%04X want 0x%04X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    {
        uint8_t trellis_bits[96];
        DSD_MEMSET(trellis_bits, 0, sizeof(trellis_bits));

        for (size_t i = 0U; i < 80U; i++) {
            trellis_bits[i] = (uint8_t)(((i * 7U) + 3U) & 1U);
        }

        const uint16_t crc = crc12f(trellis_bits, 80);
        write_bits_u16(trellis_bits, 80U, crc, 12U);
        write_bits_u16(trellis_bits, 92U, 0x0DU, 4U);

        rc |= expect_u16("facch-crc12-payload-window", nxdn_facch_crc12_payload_from_trellis(trellis_bits), crc);
        rc |= expect_u16("facch-crc12-check-window", nxdn_facch_crc12_check_from_trellis(trellis_bits), crc);

        trellis_bits[92] ^= 1U;
        trellis_bits[93] ^= 1U;
        trellis_bits[94] ^= 1U;
        trellis_bits[95] ^= 1U;

        rc |= expect_u16("facch-crc12-payload-ignores-tail", nxdn_facch_crc12_payload_from_trellis(trellis_bits), crc);
        rc |= expect_u16("facch-crc12-check-ignores-tail", nxdn_facch_crc12_check_from_trellis(trellis_bits), crc);
    }

    {
        uint8_t trellis_bits[208];
        DSD_MEMSET(trellis_bits, 0, sizeof(trellis_bits));

        for (size_t i = 0U; i < 184U; i++) {
            trellis_bits[i] = (uint8_t)(((i * 5U) + (i / 3U) + 1U) & 1U);
        }

        const uint16_t crc = crc15(trellis_bits, 184);
        write_bits_u16(trellis_bits, 184U, crc, 15U);
        write_bits_u16(trellis_bits, 199U, 0x0155U, 9U);

        rc |= expect_u16("facch2-crc15-payload-window", nxdn_facch2_udch_crc15_payload_from_trellis(trellis_bits), crc);
        rc |= expect_u16("facch2-crc15-check-window", nxdn_facch2_udch_crc15_check_from_trellis(trellis_bits), crc);

        for (size_t i = 199U; i < 208U; i++) {
            trellis_bits[i] ^= 1U;
        }

        rc |= expect_u16("facch2-crc15-payload-ignores-tail", nxdn_facch2_udch_crc15_payload_from_trellis(trellis_bits),
                         crc);
        rc |=
            expect_u16("facch2-crc15-check-ignores-tail", nxdn_facch2_udch_crc15_check_from_trellis(trellis_bits), crc);
    }

    rc |= expect_int("sacch-sequence-start", nxdn_sacch_segment_sequence_is_valid(1U, 3, 0), 1);
    rc |= expect_int("sacch-sequence-next-1", nxdn_sacch_segment_sequence_is_valid(1U, 0, 1), 1);
    rc |= expect_int("sacch-sequence-next-2", nxdn_sacch_segment_sequence_is_valid(1U, 1, 2), 1);
    rc |= expect_int("sacch-sequence-next-3", nxdn_sacch_segment_sequence_is_valid(1U, 2, 3), 1);
    rc |= expect_int("sacch-sequence-out-of-order", nxdn_sacch_segment_sequence_is_valid(1U, 0, 2), 0);
    rc |= expect_int("sacch-sequence-bad-crc", nxdn_sacch_segment_sequence_is_valid(0U, 0, 1), 0);

    if (rc == 0) {
        printf("NXDN_DEPERM_WINDOWS: OK\n");
    }
    return rc;
}
