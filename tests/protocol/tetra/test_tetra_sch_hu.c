// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * SCH/HU loopback (EN 300 392-2 8.3.1.4.3): 92 MAC bits, CRC-16, four zero tail
 * bits, the rate-2/3 signalling code and the (168,13) interleaver.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  PASS: %s\n", msg);                                                                               \
        } else {                                                                                                       \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                                           \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* 16 CRC bits that make tetra_crc16_ccitt_bits(data||crc, 108) == 0x1D0F. */
static void
append_crc(uint8_t* bits, int data_len) {
    for (unsigned guess = 0; guess < 65536u; guess++) {
        for (int i = 0; i < 16; i++) {
            bits[data_len + i] = (uint8_t)((guess >> (15 - i)) & 1u);
        }
        if (tetra_crc16_ccitt_bits(bits, data_len + 16) == TETRA_CRC16_OK) {
            return;
        }
    }
}

static void
mother4(const uint8_t* in, int n, uint8_t* out) {
    int st = 0;
    for (int k = 0; k < n; k++) {
        const int d1 = st & 1, d2 = (st >> 1) & 1, d3 = (st >> 2) & 1, d4 = (st >> 3) & 1;
        const int bit = in[k] & 1;
        out[4 * k + 0] = (uint8_t)(bit ^ d1 ^ d4);
        out[4 * k + 1] = (uint8_t)(bit ^ d2 ^ d3 ^ d4);
        out[4 * k + 2] = (uint8_t)(bit ^ d1 ^ d2 ^ d4);
        out[4 * k + 3] = (uint8_t)(bit ^ d1 ^ d3 ^ d4);
        st = ((st << 1) & 0xE) | bit;
    }
}

int
main(void) {
    uint8_t type2[112];
    uint8_t mother[112 * 4];
    uint8_t type3[168];
    uint8_t type4[168];
    uint16_t soft[168];
    uint8_t out[92];

    printf("=== TETRA SCH/HU ===\n");
    CHECK(tetra_compute_scramb_seed(86, 28, 0x19) == 0x15801C67u,
          "862 network seed from MCC 86 / MNC 28 / colour 0x19");

    /* A fixed xorshift32 pattern, so a failure reproduces. */
    uint32_t x = 11u;
    DSD_MEMSET(type2, 0, sizeof(type2));
    for (int i = 0; i < 92; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        type2[i] = (uint8_t)(x & 1u);
    }
    append_crc(type2, 92);
    CHECK(tetra_crc16_ccitt_bits(type2, 108) == TETRA_CRC16_OK, "constructed CRC passes");

    mother4(type2, 112, mother);
    CHECK(tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_2_3, mother, 112 * 4, type3, 168) == 168,
          "rate 2/3 punctures 112 bits to 168");
    tetra_block_interleave(type3, type4, 168, 13);
    for (int i = 0; i < 168; i++) {
        soft[i] = type4[i] ? 0xffffu : 0u;
    }

    CHECK(tetra_sch_hu_decode(soft, out) == 1, "clean SCH/HU decodes");
    CHECK(memcmp(out, type2, 92) == 0, "92 MAC bits match");

    soft[20] ^= 0xffffu;
    soft[80] ^= 0xffffu;
    soft[140] ^= 0xffffu;
    CHECK(tetra_sch_hu_decode(soft, out) == 1 && memcmp(out, type2, 92) == 0, "three channel errors corrected");

    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
