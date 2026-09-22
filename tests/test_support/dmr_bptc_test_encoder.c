// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/* Systematic Hamming row/column encoder, checked against an independent DMR burst. */
#include "dmr_bptc_test_encoder.h"
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/bptc.h>
#include <stdio.h>
#include <string.h>

void
dmr_test_encode_bptc_196x96(const uint8_t payload[96], const uint8_t reserved[3], uint8_t info[196]) {
    static const uint8_t H15[4][15] = {
        {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
        {0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0},
        {0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0},
        {1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1},
    };
    static const uint8_t H13[4][13] = {
        {1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
        {1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0},
        {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0},
        {1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1},
    };
    uint8_t m[13][15] = {{0}};
    for (size_t r = 0; r < 3; ++r) {
        m[0][2 - r] = reserved[r];
    }
    DSD_MEMCPY(&m[0][3], payload, 8);
    for (size_t i = 1; i <= 8; ++i) {
        DSD_MEMCPY(m[i], payload + 8 + (i - 1) * 11, 11);
    }
    for (size_t i = 0; i < 9; ++i) {
        for (size_t r = 0; r < 4; ++r) {
            for (size_t j = 0; j < 11; ++j) {
                m[i][11 + r] ^= m[i][j] & H15[r][j];
            }
        }
    }
    for (size_t j = 0; j < 15; ++j) {
        for (size_t r = 0; r < 4; ++r) {
            for (size_t i = 0; i < 9; ++i) {
                m[9 + r][j] ^= m[i][j] & H13[r][i];
            }
        }
    }
    uint8_t deint[196] = {0};
    for (size_t i = 0; i < 13; ++i) {
        for (size_t j = 0; j < 15; ++j) {
            deint[1 + i * 15 + j] = m[i][j];
        }
    }
    for (size_t i = 0; i < 196; ++i) {
        info[i] = deint[BPTCDeInterleavingIndex[i]];
    }
}

int
dmr_test_check_reference_burst(void) {
    // Reference from test_dmr_event_crc.c; R={0,0,1} represents reserved value 4.
    static const uint8_t bytes[12] = {0x02, 0x50, 0, 0, 0x2A, 0, 0, 0x18, 0x81, 0, 0, 0};
    static const uint8_t reserved[3] = {0, 0, 1};
    static const uint8_t ras_burst[25] = {
        0x48, 0x14, 0x82, 0x24, 0x24, 0x3A, 0x08, 0xA8, 0x01, 0x60, 0x11, 0x21, 0x01,
        0x52, 0x8B, 0x85, 0x08, 0x60, 0x0C, 0x40, 0x19, 0x20, 0x46, 0x0C, 0xA0,
    };
    uint8_t payload[96];
    uint8_t info[196];
    uint8_t packed[25] = {0};
    dsd_unpack_bytes_to_bits(bytes, sizeof bytes, payload, sizeof payload, sizeof bytes);
    dmr_test_encode_bptc_196x96(payload, reserved, info);
    for (size_t i = 0; i < 196; ++i) {
        packed[i / 8] |= (uint8_t)(info[i] << (7 - i % 8));
    }
    if (memcmp(packed, ras_burst, sizeof packed) != 0) {
        DSD_FPRINTF(stderr, "BPTC encoder disagrees with reference burst\n");
        return 0;
    }
    return 1;
}
