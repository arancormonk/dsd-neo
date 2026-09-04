// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

/*
 * Independent P25 NID test-vector generator.
 *
 * Each row is one systematic input bit followed by the 47 BCH parity bits.
 * The parity rows come from the published P25 NID generator matrix used by
 * sdrtrunk; its final, separate NID parity bit is omitted here.
 */
static inline void
p25_test_generate_nid_codeword(const char info[16], char codeword[63]) {
    static const std::uint64_t parity_rows[16] = {
        0xCD930BDD3B2AULL, 0xAB5A8E33A6BEULL, 0x983E4CC4E874ULL, 0x4C1F2662743AULL,
        0xEB9C98EC0136ULL, 0xB85D47AB3BB0ULL, 0x5C2EA3D59DD8ULL, 0x2E1751EACEECULL,
        0x170BA8F56776ULL, 0xC616DFA78890ULL, 0x630B6FD3C448ULL, 0x3185B7E9E224ULL,
        0x18C2DBF4F112ULL, 0xC1F2662743A2ULL, 0xAD6A38CE9AFBULL, 0x9B2617BA7657ULL,
    };

    std::uint64_t packed = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (info[bit]) {
            packed ^= (UINT64_C(1) << (62 - bit)) | (parity_rows[bit] >> 1);
        }
    }

    for (int bit = 0; bit < 63; bit++) {
        codeword[bit] = (char)((packed >> (62 - bit)) & UINT64_C(1));
    }
}
